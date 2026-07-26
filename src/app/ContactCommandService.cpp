#include "app/ContactCommandService.h"

#include "app/AccountConnectionProvider.h"
#include "app/ApplicationErrorCoordinator.h"
#include "app/ContactCommandPreparation.h"
#include "app/WorkScheduler.h"
#include "app/undo/UndoManager.h"
#include "jmap/api/PatchObject.h"
#include "jmap/cache/ContactRepository.h"
#include "jmap/contacts/ContactService.h"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <ranges>
#include <unordered_map>
#include <utility>
#include <variant>

namespace javelin::app
{
    namespace
    {
        class ForegroundWorkScope final
        {
          public:
            explicit ForegroundWorkScope(WorkScheduler& scheduler) : m_scheduler(scheduler)
            {
                m_scheduler.beginForegroundWork();
            }

            ~ForegroundWorkScope()
            {
                m_scheduler.endForegroundWork();
            }

          private:
            WorkScheduler& m_scheduler;
        };

        [[nodiscard]] javelin::jmap::LiveConnectionSettings
        toLiveConnectionSettings(const AccountConnectionSettings& settings)
        {
            return {
                .sessionUrl = settings.sessionUrl,
                .loginEmail = settings.loginEmail,
                .apiKey = settings.apiKey,
            };
        }

        [[nodiscard]] javelin::jmap::OperationError missingConfiguration()
        {
            return {
                .code = javelin::jmap::OperationErrorCode::PreconditionFailed,
                .message = QStringLiteral("Account synchronization is not configured."),
            };
        }

        template <typename Result>
        [[nodiscard]] Result observeResult(ApplicationErrorCoordinator& coordinator,
                                           const AccountConnectionSettings& settings,
                                           const std::string_view ownerAccountId, QString operation,
                                           Result result)
        {
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                coordinator.reportFailure(settings, ownerAccountId, std::move(operation), *error);
            else
                coordinator.reportSuccess(settings.connectionId);
            return result;
        }

        [[nodiscard]] std::optional<undo::ContactCardItemHistory>
        createdHistoryItem(const std::string& accountId, const std::string& document)
        {
            const auto contact =
                javelin::jmap::contacts::summarizeContact(accountId, {
                                                                         .id = {},
                                                                         .uid = {},
                                                                         .kind = {},
                                                                         .document = document,
                                                                     });
            if (!contact.has_value() || contact->uid.empty() || contact->addressBookIds.empty())
                return std::nullopt;
            return undo::ContactCardItemHistory{
                .addressBookId = contact->addressBookIds.front(),
                .currentCardId = std::nullopt,
                .uid = contact->uid,
                .beforeDocumentJson = std::nullopt,
                .afterDocumentJson = document,
            };
        }

        [[nodiscard]] undo::ContactCardItemHistory
        existingHistoryItem(const javelin::jmap::contacts::ContactSummary& contact,
                            std::optional<std::string> after)
        {
            return {
                .addressBookId =
                    contact.addressBookIds.empty() ? std::string{} : contact.addressBookIds.front(),
                .currentCardId = contact.id,
                .uid = contact.uid,
                .beforeDocumentJson = contact.document,
                .afterDocumentJson = std::move(after),
            };
        }
    } // namespace

    ContactCommandService::ContactCommandService(
        AccountConnectionProvider& connectionProvider,
        javelin::jmap::contacts::ContactService& contactService,
        javelin::jmap::cache::ContactRepository& contactRepository,
        ApplicationErrorCoordinator& errorCoordinator, WorkScheduler& workScheduler,
        undo::UndoManager& undoManager)
        : m_connectionProvider(connectionProvider), m_contactService(contactService),
          m_contactRepository(contactRepository), m_errorCoordinator(errorCoordinator),
          m_workScheduler(workScheduler), m_undoManager(undoManager)
    {
    }

    QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
    ContactCommandService::mutateAddressBook(std::string ownerAccountId, AddressBookCommand command)
    {
        auto prepared = prepareAddressBookMutation(std::move(command));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&prepared))
            co_return *error;
        co_return co_await submitAddressBooks(
            std::move(ownerAccountId),
            std::get<javelin::jmap::api::AddressBookSetRequest>(std::move(prepared)));
    }

    QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
    ContactCommandService::saveContact(std::string ownerAccountId, SaveContactCommand command)
    {
        auto prepared = prepareSaveContact(std::move(command));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&prepared))
            co_return *error;
        co_return co_await submitContactCards(
            std::move(ownerAccountId),
            std::get<javelin::jmap::api::ContactCardSetRequest>(std::move(prepared)),
            QStringLiteral("Save contact"));
    }

    QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
    ContactCommandService::setContactsStarred(std::string ownerAccountId,
                                              SetContactsStarredCommand command)
    {
        auto prepared = prepareSetContactsStarred(std::move(command));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&prepared))
            co_return *error;
        co_return co_await submitContactCards(
            std::move(ownerAccountId),
            std::get<javelin::jmap::api::ContactCardSetRequest>(std::move(prepared)),
            QStringLiteral("Change starred contacts"));
    }

    QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
    ContactCommandService::deleteContacts(std::string ownerAccountId, DeleteContactsCommand command)
    {
        auto prepared = prepareDeleteContacts(std::move(command));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&prepared))
            co_return *error;
        co_return co_await submitContactCards(
            std::move(ownerAccountId),
            std::get<javelin::jmap::api::ContactCardSetRequest>(std::move(prepared)),
            QStringLiteral("Delete contacts"));
    }

    QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
    ContactCommandService::createContactGroup(std::string ownerAccountId,
                                              CreateContactGroupCommand command)
    {
        auto prepared =
            m_contactService.prepareCreateGroup({.accountId = std::move(command.accountId),
                                                 .addressBookId = std::move(command.addressBookId),
                                                 .name = std::move(command.name)});
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&prepared))
            co_return *error;
        co_return co_await submitContactCards(
            std::move(ownerAccountId),
            std::get<javelin::jmap::api::ContactCardSetRequest>(std::move(prepared)),
            QStringLiteral("Create contact group"));
    }

    QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
    ContactCommandService::setContactGroupMembership(std::string ownerAccountId,
                                                     SetContactGroupMembershipCommand command)
    {
        auto prepared =
            m_contactService.prepareGroupMembership({.accountId = std::move(command.accountId),
                                                     .groupId = std::move(command.groupId),
                                                     .memberUids = std::move(command.memberUids),
                                                     .included = command.included});
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&prepared))
            co_return *error;
        auto request = std::get<javelin::jmap::api::ContactCardSetRequest>(std::move(prepared));
        if (request.update.empty())
            co_return javelin::jmap::contacts::ContactMutationSummary{
                .accountId = std::move(request.accountId),
                .newState = request.ifInState.value_or(std::string{}),
                .createdId = std::nullopt,
                .createdIds = {},
            };
        co_return co_await submitContactCards(std::move(ownerAccountId), std::move(request),
                                              QStringLiteral("Change contact group membership"));
    }

    QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
    ContactCommandService::copyContact(std::string ownerAccountId, CopyContactCommand command)
    {
        auto prepared = prepareCopyContact(std::move(command));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&prepared))
            co_return *error;
        co_return co_await submitContactCopy(
            std::move(ownerAccountId),
            std::get<javelin::jmap::api::ContactCardCopyRequest>(std::move(prepared)),
            QStringLiteral("Copy contact"));
    }

    QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
    ContactCommandService::importContacts(std::string ownerAccountId, ImportContactsCommand command)
    {
        auto prepared = prepareImportContacts(std::move(command));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&prepared))
            co_return *error;
        co_return co_await submitContactCards(
            std::move(ownerAccountId),
            std::get<javelin::jmap::api::ContactCardSetRequest>(std::move(prepared)),
            QStringLiteral("Import contacts"));
    }

    QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
    ContactCommandService::mergeContacts(std::string ownerAccountId, MergeContactsCommand command)
    {
        auto prepared = prepareMergeContacts(std::move(command));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&prepared))
            co_return *error;
        co_return co_await submitContactCards(
            std::move(ownerAccountId),
            std::get<javelin::jmap::api::ContactCardSetRequest>(std::move(prepared)),
            QStringLiteral("Merge contacts"));
    }

    QCoro::Task<javelin::jmap::contacts::ContactUploadResult>
    ContactCommandService::uploadContactMedia(std::string ownerAccountId, std::string accountId,
                                              QByteArray payload, std::string mediaType)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto settings = m_connectionProvider.connectionSettingsFor(ownerAccountId);
        if (!settings.has_value())
            co_return missingConfiguration();
        co_return observeResult(
            m_errorCoordinator, *settings, ownerAccountId, QStringLiteral("Upload contact media"),
            co_await m_contactService.uploadMedia(toLiveConnectionSettings(*settings),
                                                  ownerAccountId, std::move(accountId),
                                                  std::move(payload), std::move(mediaType)));
    }

    QCoro::Task<javelin::jmap::contacts::ContactDownloadResult>
    ContactCommandService::downloadContactMedia(std::string ownerAccountId, std::string accountId,
                                                std::string blobId, std::string mediaType)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto settings = m_connectionProvider.connectionSettingsFor(ownerAccountId);
        if (!settings.has_value())
            co_return missingConfiguration();
        co_return observeResult(
            m_errorCoordinator, *settings, ownerAccountId, QStringLiteral("Download contact media"),
            co_await m_contactService.downloadMedia(toLiveConnectionSettings(*settings),
                                                    ownerAccountId, std::move(accountId),
                                                    std::move(blobId), std::move(mediaType)));
    }

    QCoro::Task<undo::AuthoritativeContactsResult>
    ContactCommandService::getAuthoritativeContacts(std::string ownerAccountId,
                                                    std::string accountId)
    {
        const auto settings = m_connectionProvider.connectionSettingsFor(ownerAccountId);
        if (!settings.has_value())
            co_return missingConfiguration();
        auto refreshed = co_await m_contactService.refreshAll(toLiveConnectionSettings(*settings),
                                                              ownerAccountId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&refreshed))
            co_return *error;
        auto contacts = m_contactRepository.listContacts(accountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&contacts))
            co_return javelin::jmap::operationError(*error);
        auto state = m_contactRepository.contactState(accountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&state))
            co_return javelin::jmap::operationError(*error);
        co_return undo::AuthoritativeContacts{
            .state = std::get<std::optional<std::string>>(state).value_or(std::string{}),
            .contacts =
                std::get<std::vector<javelin::jmap::contacts::ContactSummary>>(std::move(contacts)),
        };
    }

    QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
    ContactCommandService::applyContactCardsFromHistory(
        std::string ownerAccountId, javelin::jmap::api::ContactCardSetRequest request,
        const undo::CommandOrigin)
    {
        const auto settings = m_connectionProvider.connectionSettingsFor(ownerAccountId);
        if (!settings.has_value())
            co_return missingConfiguration();
        co_return co_await m_contactService.setContactCards(
            toLiveConnectionSettings(*settings), std::move(ownerAccountId), std::move(request));
    }

    QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
    ContactCommandService::submitAddressBooks(std::string ownerAccountId,
                                              javelin::jmap::api::AddressBookSetRequest request)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto settings = m_connectionProvider.connectionSettingsFor(ownerAccountId);
        if (!settings.has_value())
            co_return missingConfiguration();
        co_return observeResult(
            m_errorCoordinator, *settings, ownerAccountId, QStringLiteral("Change address books"),
            co_await m_contactService.setAddressBooks(toLiveConnectionSettings(*settings),
                                                      ownerAccountId, std::move(request)));
    }

    QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
    ContactCommandService::submitContactCards(std::string ownerAccountId,
                                              javelin::jmap::api::ContactCardSetRequest request,
                                              QString operationDescription)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto settings = m_connectionProvider.connectionSettingsFor(ownerAccountId);
        if (!settings.has_value())
            co_return missingConfiguration();
        undo::ContactCardHistory history{
            .connectionId = settings->connectionId,
            .accountId = request.accountId,
            .items = {},
        };
        std::unordered_map<std::string, std::size_t> creationItems;
        for (const auto& [creationId, document] : request.create)
        {
            auto item = createdHistoryItem(request.accountId, document.json);
            if (!item.has_value())
                co_return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::PreconditionFailed,
                    .message = QStringLiteral(
                        "The created contact cannot be represented in operation history."),
                };
            creationItems.emplace(creationId, history.items.size());
            history.items.push_back(std::move(*item));
        }
        for (const auto& [contactId, document] : request.update)
        {
            auto found = m_contactRepository.findContact(request.accountId, contactId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&found))
                co_return javelin::jmap::operationError(*error);
            const auto& contact =
                std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(found);
            if (!contact.has_value())
                co_return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::NotFound,
                    .message = QStringLiteral("The contact is no longer available."),
                };
            const auto after =
                javelin::jmap::api::applyPatchObject(contact->document, document.json);
            if (!std::holds_alternative<std::string>(after))
                co_return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::PreconditionFailed,
                    .message = QStringLiteral("The contact update cannot be applied to its "
                                              "current cached document."),
                };
            history.items.push_back(existingHistoryItem(*contact, std::get<std::string>(after)));
        }
        for (const auto& contactId : request.destroy)
        {
            auto found = m_contactRepository.findContact(request.accountId, contactId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&found))
                co_return javelin::jmap::operationError(*error);
            const auto& contact =
                std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(found);
            if (!contact.has_value())
                co_return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::NotFound,
                    .message = QStringLiteral("The contact is no longer available."),
                };
            history.items.push_back(existingHistoryItem(*contact, std::nullopt));
        }

        auto preparedResult = m_undoManager.prepareNormal(
            operationDescription, undo::HistoryDomain::Contacts, history, std::nullopt);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&preparedResult))
            co_return javelin::jmap::operationError(*error);
        auto prepared = std::get<std::optional<undo::HistoryEntry>>(std::move(preparedResult));
        auto result = observeResult(
            m_errorCoordinator, *settings, ownerAccountId, std::move(operationDescription),
            co_await m_contactService.setContactCards(toLiveConnectionSettings(*settings),
                                                      ownerAccountId, std::move(request)));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            if (prepared.has_value())
            {
                if (javelin::jmap::isTransientError(*error) &&
                    !javelin::jmap::isAuthenticationError(*error))
                {
                    auto committed = m_undoManager.commitNormal(std::move(*prepared));
                    if (const auto* databaseError =
                            std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
                        co_return javelin::jmap::operationError(*databaseError);
                    const auto& entry = std::get<undo::HistoryEntry>(committed);
                    if (const auto databaseError = m_undoManager.setEntryStatus(
                            entry.entryId, undo::HistoryEntryStatus::BlockedUnknown,
                            error->message))
                        co_return javelin::jmap::operationError(*databaseError);
                }
                else if (const auto databaseError = m_undoManager.discardNormal(prepared->entryId))
                    co_return javelin::jmap::operationError(*databaseError);
            }
            co_return *error;
        }

        if (prepared.has_value())
        {
            auto& committedHistory = std::get<undo::ContactCardHistory>(prepared->payload);
            const auto& summary = std::get<javelin::jmap::contacts::ContactMutationSummary>(result);
            for (const auto& mapping : summary.createdIds)
                if (const auto found = creationItems.find(mapping.creationId);
                    found != creationItems.end())
                    committedHistory.items[found->second].currentCardId = mapping.serverId;
            if (summary.createdIds.size() != creationItems.size())
            {
                static_cast<void>(m_undoManager.setEntryStatus(
                    prepared->entryId, undo::HistoryEntryStatus::BlockedPartial,
                    QStringLiteral("The server returned incomplete contact creation identities.")));
            }
            else
            {
                auto committed = m_undoManager.commitNormal(std::move(*prepared));
                if (const auto* databaseError =
                        std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
                    co_return javelin::jmap::operationError(*databaseError);
            }
        }
        co_return result;
    }

    QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
    ContactCommandService::submitContactCopy(std::string ownerAccountId,
                                             javelin::jmap::api::ContactCardCopyRequest request,
                                             QString operationDescription)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto settings = m_connectionProvider.connectionSettingsFor(ownerAccountId);
        if (!settings.has_value())
            co_return missingConfiguration();
        undo::ContactCardHistory history{
            .connectionId = settings->connectionId,
            .accountId = request.accountId,
            .items = {},
        };
        std::unordered_map<std::string, std::size_t> creationItems;
        for (const auto& [creationId, patch] : request.create)
        {
            auto sourceId = std::string{};
            if (auto parsed = javelin::jmap::contacts::summarizeContact(
                    request.fromAccountId,
                    {.id = {}, .uid = {}, .kind = {}, .document = patch.json}))
                sourceId = parsed->id;
            if (sourceId.empty())
            {
                glz::generic value;
                auto json = patch.json;
                if (!glz::read_json(value, json) && value.is_object())
                {
                    const auto id = value.get_object().find("id");
                    if (id != value.get_object().end() && id->second.is_string())
                        sourceId = id->second.get_string();
                }
            }
            auto source = m_contactRepository.findContact(request.fromAccountId, sourceId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&source))
                co_return javelin::jmap::operationError(*error);
            const auto& contact =
                std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(source);
            if (!contact.has_value())
                co_return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::NotFound,
                    .message = QStringLiteral("The copied contact is no longer available."),
                };
            auto destination = javelin::jmap::api::applyPatchObject(contact->document, patch.json);
            if (!std::holds_alternative<std::string>(destination))
                co_return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::PreconditionFailed,
                    .message = QStringLiteral("The contact copy document is invalid."),
                };
            auto item = createdHistoryItem(request.accountId, std::get<std::string>(destination));
            if (!item.has_value())
                co_return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::PreconditionFailed,
                    .message =
                        QStringLiteral("The copied contact cannot be represented in history."),
                };
            creationItems.emplace(creationId, history.items.size());
            history.items.push_back(std::move(*item));
        }
        auto preparedResult = m_undoManager.prepareNormal(
            operationDescription, undo::HistoryDomain::Contacts, history, std::nullopt);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&preparedResult))
            co_return javelin::jmap::operationError(*error);
        auto prepared = std::get<std::optional<undo::HistoryEntry>>(std::move(preparedResult));
        auto result = observeResult(
            m_errorCoordinator, *settings, ownerAccountId, std::move(operationDescription),
            co_await m_contactService.copyContactCards(toLiveConnectionSettings(*settings),
                                                       ownerAccountId, std::move(request)));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            if (prepared.has_value())
            {
                if (javelin::jmap::isTransientError(*error) &&
                    !javelin::jmap::isAuthenticationError(*error))
                {
                    auto committed = m_undoManager.commitNormal(std::move(*prepared));
                    if (const auto* databaseError =
                            std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
                        co_return javelin::jmap::operationError(*databaseError);
                    const auto& entry = std::get<undo::HistoryEntry>(committed);
                    if (const auto databaseError = m_undoManager.setEntryStatus(
                            entry.entryId, undo::HistoryEntryStatus::BlockedUnknown,
                            error->message))
                        co_return javelin::jmap::operationError(*databaseError);
                }
                else if (const auto databaseError = m_undoManager.discardNormal(prepared->entryId))
                    co_return javelin::jmap::operationError(*databaseError);
            }
            co_return *error;
        }
        if (prepared.has_value())
        {
            auto& committedHistory = std::get<undo::ContactCardHistory>(prepared->payload);
            const auto& summary = std::get<javelin::jmap::contacts::ContactMutationSummary>(result);
            for (const auto& mapping : summary.createdIds)
                if (const auto found = creationItems.find(mapping.creationId);
                    found != creationItems.end())
                    committedHistory.items[found->second].currentCardId = mapping.serverId;
            if (summary.createdIds.size() != creationItems.size())
                co_return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::ProtocolViolation,
                    .message =
                        QStringLiteral("The contact copy response omitted a created identity."),
                };
            auto committed = m_undoManager.commitNormal(std::move(*prepared));
            if (const auto* databaseError =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
                co_return javelin::jmap::operationError(*databaseError);
        }
        co_return result;
    }
} // namespace javelin::app
