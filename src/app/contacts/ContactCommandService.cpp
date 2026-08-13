#include "app/ContactCommandService.h"

#include "app/AccountConnectionProvider.h"
#include "app/ApplicationErrorCoordinator.h"
#include "app/ContactCommandPreparation.h"
#include "app/WorkScheduler.h"
#include "app/undo/UndoManager.h"
#include "jmap/api/PatchObject.h"
#include "jmap/cache/ContactRepository.h"
#include "jmap/contacts/ContactMediaService.h"
#include "jmap/contacts/ContactMutationEngine.h"
#include "jmap/contacts/ContactSyncEngine.h"

#include <glaze/glaze.hpp>

#include <KLocalizedString>

#include <QLoggingCategory>
#include <QUuid>

#include <algorithm>
#include <ranges>
#include <unordered_map>
#include <utility>
#include <variant>

namespace javelin::app
{
    Q_LOGGING_CATEGORY(logContactCommands, "application.contacts.commands")

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
                .message = i18n("Account synchronization is not configured."),
            };
        }

        struct ResolvedHistoryConnection
        {
            std::string ownerAccountId;
            AccountConnectionSettings settings;
        };

        [[nodiscard]] std::variant<ResolvedHistoryConnection, javelin::jmap::OperationError>
        resolveHistoryConnection(const AccountConnectionProvider& connectionProvider,
                                 javelin::jmap::cache::ContactRepository& contactRepository,
                                 const std::string_view routingId, const std::string_view accountId)
        {
            if (auto settings = connectionProvider.connectionSettingsFor(routingId))
                return ResolvedHistoryConnection{
                    .ownerAccountId = std::string{routingId},
                    .settings = std::move(*settings),
                };

            const auto accounts = contactRepository.listAccounts();
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&accounts))
                return javelin::jmap::operationError(*error);
            const auto& available =
                std::get<std::vector<javelin::jmap::cache::ContactAccount>>(accounts);
            const auto account = std::ranges::find(
                available, accountId, &javelin::jmap::cache::ContactAccount::accountId);
            if (account == available.end())
                return missingConfiguration();
            auto settings = connectionProvider.connectionSettingsFor(account->ownerAccountId);
            if (!settings.has_value() || settings->connectionId != routingId)
                return missingConfiguration();
            return ResolvedHistoryConnection{
                .ownerAccountId = account->ownerAccountId,
                .settings = std::move(*settings),
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
        javelin::jmap::contacts::ContactSyncEngine& syncEngine,
        javelin::jmap::contacts::ContactMutationEngine& mutationEngine,
        javelin::jmap::contacts::ContactMediaService& mediaService,
        javelin::jmap::cache::ContactRepository& contactRepository,
        ApplicationErrorCoordinator& errorCoordinator, WorkScheduler& workScheduler,
        undo::UndoManager& undoManager)
        : m_connectionProvider(connectionProvider), m_contactSyncEngine(syncEngine),
          m_contactMutationEngine(mutationEngine), m_contactMediaService(mediaService),
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
            i18n("Save contact"));
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
            i18n("Change starred contacts"));
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
            i18n("Delete contacts"));
    }

    QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
    ContactCommandService::createContactGroup(std::string ownerAccountId,
                                              CreateContactGroupCommand command)
    {
        const QString actionDescription =
            i18n("Create contact group “%1”", QString::fromStdString(command.name));
        auto prepared = m_contactMutationEngine.prepareCreateGroup(
            {.accountId = std::move(command.accountId),
             .addressBookId = std::move(command.addressBookId),
             .name = std::move(command.name)});
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&prepared))
            co_return *error;
        co_return co_await submitContactCards(
            std::move(ownerAccountId),
            std::get<javelin::jmap::api::ContactCardSetRequest>(std::move(prepared)),
            i18n("Create contact group"), actionDescription);
    }

    QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
    ContactCommandService::deleteContactGroup(std::string ownerAccountId,
                                              DeleteContactGroupCommand command)
    {
        QString groupName = QString::fromStdString(command.groupId);
        const auto cached = m_contactRepository.findContact(command.accountId, command.groupId);
        if (const auto* contact =
                std::get_if<std::optional<javelin::jmap::contacts::ContactSummary>>(&cached);
            contact != nullptr && contact->has_value())
            groupName = QString::fromStdString(contact->value().displayName);
        const QString actionDescription = i18n("Delete contact group “%1” (%2)", groupName,
                                               QString::fromStdString(command.groupId));
        auto prepared = prepareDeleteContactGroup(std::move(command));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&prepared))
            co_return *error;
        co_return co_await submitContactCards(
            std::move(ownerAccountId),
            std::get<javelin::jmap::api::ContactCardSetRequest>(std::move(prepared)),
            i18n("Delete contact group"), actionDescription);
    }

    QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
    ContactCommandService::setContactGroupMembership(std::string ownerAccountId,
                                                     SetContactGroupMembershipCommand command)
    {
        QString groupName = QString::fromStdString(command.groupId);
        const auto cached = m_contactRepository.findContact(command.accountId, command.groupId);
        if (const auto* contact =
                std::get_if<std::optional<javelin::jmap::contacts::ContactSummary>>(&cached);
            contact != nullptr && contact->has_value())
            groupName = QString::fromStdString(contact->value().displayName);
        const auto memberCount = command.memberUids.size();
        const QString actionDescription =
            QStringLiteral("%1 %2 contact%3 %4 group “%5” (%6)")
                .arg(command.included ? QStringLiteral("Add") : QStringLiteral("Remove"))
                .arg(memberCount)
                .arg(memberCount == 1 ? QString{} : QStringLiteral("s"))
                .arg(command.included ? QStringLiteral("to") : QStringLiteral("from"))
                .arg(groupName, QString::fromStdString(command.groupId));
        auto prepared = m_contactMutationEngine.prepareGroupMembership(
            {.accountId = std::move(command.accountId),
             .groupId = std::move(command.groupId),
             .memberUids = std::move(command.memberUids),
             .included = command.included});
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&prepared))
            co_return *error;
        auto request = std::get<javelin::jmap::api::ContactCardSetRequest>(std::move(prepared));
        if (request.update.empty())
        {
            qCInfo(logContactCommands).noquote()
                << actionDescription << "account=" << QString::fromStdString(request.accountId)
                << "outcome=no_change";
            co_return javelin::jmap::contacts::ContactMutationSummary{
                .accountId = std::move(request.accountId),
                .newState = request.ifInState.value_or(std::string{}),
                .createdId = std::nullopt,
                .createdIds = {},
                .receipt = {},
            };
        }
        co_return co_await submitContactCards(std::move(ownerAccountId), std::move(request),
                                              i18n("Change contact group membership"),
                                              actionDescription);
    }

    QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
    ContactCommandService::copyContact(std::string ownerAccountId, CopyContactCommand command)
    {
        if (!command.destinationOwnerAccountId.empty() &&
            command.destinationOwnerAccountId != ownerAccountId)
        {
            const auto destinationOwnerAccountId = command.destinationOwnerAccountId;
            auto prepared = prepareCrossConnectionCopy(std::move(command));
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&prepared))
                co_return *error;
            co_return co_await submitContactCards(
                destinationOwnerAccountId,
                std::get<javelin::jmap::api::ContactCardSetRequest>(std::move(prepared)),
                i18n("Copy contact"));
        }
        auto prepared = prepareCopyContact(std::move(command));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&prepared))
            co_return *error;
        co_return co_await submitContactCopy(
            std::move(ownerAccountId),
            std::get<javelin::jmap::api::ContactCardCopyRequest>(std::move(prepared)),
            i18n("Copy contact"));
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
            i18n("Import contacts"));
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
            i18n("Merge contacts"));
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
            m_errorCoordinator, *settings, ownerAccountId, i18n("Upload contact media"),
            co_await m_contactMediaService.uploadMedia(toLiveConnectionSettings(*settings),
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
            m_errorCoordinator, *settings, ownerAccountId, i18n("Download contact media"),
            co_await m_contactMediaService.downloadMedia(toLiveConnectionSettings(*settings),
                                                         ownerAccountId, std::move(accountId),
                                                         std::move(blobId), std::move(mediaType)));
    }

    QCoro::Task<undo::AuthoritativeContactsResult>
    ContactCommandService::getAuthoritativeContacts(std::string ownerAccountId,
                                                    std::string accountId)
    {
        auto resolved = resolveHistoryConnection(m_connectionProvider, m_contactRepository,
                                                 ownerAccountId, accountId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&resolved))
            co_return *error;
        auto connection = std::get<ResolvedHistoryConnection>(std::move(resolved));
        auto refreshed = co_await m_contactSyncEngine.refreshAll(
            toLiveConnectionSettings(connection.settings), connection.ownerAccountId);
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

    undo::AuthoritativeContactsResult
    ContactCommandService::getEffectiveContacts(const std::string_view accountId)
    {
        auto contacts = m_contactRepository.listContacts(accountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&contacts))
            return javelin::jmap::operationError(*error);
        auto state = m_contactRepository.contactState(accountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&state))
            return javelin::jmap::operationError(*error);
        return undo::AuthoritativeContacts{
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
        auto resolved = resolveHistoryConnection(m_connectionProvider, m_contactRepository,
                                                 ownerAccountId, request.accountId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&resolved))
            co_return *error;
        auto connection = std::get<ResolvedHistoryConnection>(std::move(resolved));
        co_return co_await m_contactMutationEngine.setContactCards(
            toLiveConnectionSettings(connection.settings), std::move(connection.ownerAccountId),
            std::move(request), {.refreshAndRetryStateMismatch = true});
    }

    QCoro::Task<undo::AuthoritativeAddressBooksResult>
    ContactCommandService::getAuthoritativeAddressBooks(std::string ownerAccountId,
                                                        std::string accountId)
    {
        auto resolved = resolveHistoryConnection(m_connectionProvider, m_contactRepository,
                                                 ownerAccountId, accountId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&resolved))
            co_return *error;
        auto connection = std::get<ResolvedHistoryConnection>(std::move(resolved));
        auto refreshed = co_await m_contactSyncEngine.refreshAll(
            toLiveConnectionSettings(connection.settings), connection.ownerAccountId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&refreshed))
            co_return *error;
        auto books = m_contactRepository.listAddressBooks(accountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&books))
            co_return javelin::jmap::operationError(*error);
        auto state = m_contactRepository.addressBookState(accountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&state))
            co_return javelin::jmap::operationError(*error);
        co_return undo::AuthoritativeAddressBooks{
            .state = std::get<std::optional<std::string>>(state).value_or(std::string{}),
            .addressBooks =
                std::get<std::vector<javelin::jmap::api::AddressBook>>(std::move(books)),
        };
    }

    undo::AuthoritativeAddressBooksResult
    ContactCommandService::getEffectiveAddressBooks(const std::string_view accountId)
    {
        auto books = m_contactRepository.listAddressBooks(accountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&books))
            return javelin::jmap::operationError(*error);
        auto state = m_contactRepository.addressBookState(accountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&state))
            return javelin::jmap::operationError(*error);
        return undo::AuthoritativeAddressBooks{
            .state = std::get<std::optional<std::string>>(state).value_or(std::string{}),
            .addressBooks =
                std::get<std::vector<javelin::jmap::api::AddressBook>>(std::move(books)),
        };
    }

    QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
    ContactCommandService::applyAddressBooksFromHistory(
        std::string ownerAccountId, javelin::jmap::api::AddressBookSetRequest request,
        const undo::CommandOrigin)
    {
        auto resolved = resolveHistoryConnection(m_connectionProvider, m_contactRepository,
                                                 ownerAccountId, request.accountId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&resolved))
            co_return *error;
        auto connection = std::get<ResolvedHistoryConnection>(std::move(resolved));
        co_return co_await m_contactMutationEngine.setAddressBooks(
            toLiveConnectionSettings(connection.settings), std::move(connection.ownerAccountId),
            std::move(request));
    }

    QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
    ContactCommandService::submitAddressBooks(std::string ownerAccountId,
                                              javelin::jmap::api::AddressBookSetRequest request)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto settings = m_connectionProvider.connectionSettingsFor(ownerAccountId);
        if (!settings.has_value())
            co_return missingConfiguration();
        auto listed = m_contactRepository.listAddressBooks(request.accountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&listed))
            co_return javelin::jmap::operationError(*error);
        const auto& books = std::get<std::vector<javelin::jmap::api::AddressBook>>(listed);
        const auto defaultBook =
            std::ranges::find(books, true, &javelin::jmap::api::AddressBook::isDefault);
        undo::AddressBookHistory history{
            .connectionId = ownerAccountId,
            .accountId = request.accountId,
            .currentAddressBookId = std::nullopt,
            .beforeDocumentJson = std::nullopt,
            .afterDocumentJson = std::nullopt,
            .beforeDefaultAddressBookId =
                defaultBook == books.end() ? std::nullopt : std::optional{defaultBook->id},
            .afterDefaultAddressBookId =
                request.onSuccessSetIsDefault.has_value()
                    ? request.onSuccessSetIsDefault
                    : (defaultBook == books.end() ? std::nullopt : std::optional{defaultBook->id}),
            .affectedCards = {},
        };
        std::optional<std::string> creationId;
        if (!request.create.empty())
        {
            const auto& [id, document] = *request.create.begin();
            creationId = id;
            history.afterDocumentJson = document.json;
        }
        else if (!request.update.empty())
        {
            const auto& [id, document] = *request.update.begin();
            const auto found = std::ranges::find(books, id, &javelin::jmap::api::AddressBook::id);
            if (found == books.end())
                co_return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::NotFound,
                    .message = i18n("The address book is no longer available."),
                };
            history.currentAddressBookId = id;
            history.beforeDocumentJson = javelin::jmap::api::serializeAddressBookDocument(*found);
            const auto after =
                javelin::jmap::api::applyPatchObject(*history.beforeDocumentJson, document.json);
            if (!std::holds_alternative<std::string>(after))
                co_return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::PreconditionFailed,
                    .message = i18n("The address-book update is invalid."),
                };
            history.afterDocumentJson = std::get<std::string>(after);
        }
        else if (!request.destroy.empty())
        {
            const auto& id = request.destroy.front();
            const auto found = std::ranges::find(books, id, &javelin::jmap::api::AddressBook::id);
            if (found == books.end())
                co_return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::NotFound,
                    .message = i18n("The address book is no longer available."),
                };
            history.currentAddressBookId = id;
            history.beforeDocumentJson = javelin::jmap::api::serializeAddressBookDocument(*found);
            if (request.onDestroyRemoveContents)
            {
                auto contacts = m_contactRepository.listContacts(request.accountId, id);
                if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&contacts))
                    co_return javelin::jmap::operationError(*error);
                for (const auto& contact :
                     std::get<std::vector<javelin::jmap::contacts::ContactSummary>>(contacts))
                    history.affectedCards.push_back(existingHistoryItem(contact, std::nullopt));
            }
        }
        else if (request.onSuccessSetIsDefault.has_value())
        {
            const auto found = std::ranges::find(books, *request.onSuccessSetIsDefault,
                                                 &javelin::jmap::api::AddressBook::id);
            if (found == books.end())
                co_return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::NotFound,
                    .message = i18n("The address book is no longer available."),
                };
            history.currentAddressBookId = found->id;
            history.beforeDocumentJson = javelin::jmap::api::serializeAddressBookDocument(*found);
            history.afterDocumentJson = history.beforeDocumentJson;
        }
        auto preparedResult = m_undoManager.prepareNormal(
            i18n("Change Address Books"), undo::HistoryDomain::Contacts, history, std::nullopt);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&preparedResult))
            co_return javelin::jmap::operationError(*error);
        auto prepared = std::get<std::optional<undo::HistoryEntry>>(std::move(preparedResult));
        auto result = observeResult(
            m_errorCoordinator, *settings, ownerAccountId, i18n("Change address books"),
            co_await m_contactMutationEngine.setAddressBooks(toLiveConnectionSettings(*settings),
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
                else
                    static_cast<void>(m_undoManager.discardNormal(prepared->entryId));
            }
            co_return *error;
        }
        if (prepared.has_value())
        {
            auto& committedHistory = std::get<undo::AddressBookHistory>(prepared->payload);
            const auto& summary = std::get<javelin::jmap::contacts::ContactMutationSummary>(result);
            if (creationId.has_value())
            {
                const auto mapping =
                    std::ranges::find(summary.createdIds, *creationId,
                                      &javelin::jmap::contacts::CreatedContactMapping::creationId);
                if (mapping == summary.createdIds.end())
                    co_return javelin::jmap::OperationError{
                        .code = javelin::jmap::OperationErrorCode::ProtocolViolation,
                        .message = i18n("The address-book response omitted its created id."),
                    };
                committedHistory.currentAddressBookId = mapping->serverId;
            }
            auto committed = m_undoManager.commitNormal(std::move(*prepared));
            if (const auto* databaseError =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
                co_return javelin::jmap::operationError(*databaseError);
        }
        co_return result;
    }

    QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
    ContactCommandService::submitContactCards(std::string ownerAccountId,
                                              javelin::jmap::api::ContactCardSetRequest request,
                                              QString operationDescription,
                                              QString actionDescription)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto settings = m_connectionProvider.connectionSettingsFor(ownerAccountId);
        if (!settings.has_value())
            co_return missingConfiguration();
        if (actionDescription.isEmpty())
            actionDescription = operationDescription;
        const auto traceId =
            QUuid::createUuid().toString(QUuid::WithoutBraces).left(8).toStdString();
        qCInfo(logContactCommands).noquote()
            << "action=" << QString::fromStdString(traceId) << actionDescription
            << "account=" << QString::fromStdString(request.accountId)
            << "state=" << QString::fromStdString(request.ifInState.value_or(std::string{"<none>"}))
            << "creates=" << request.create.size() << "updates=" << request.update.size()
            << "destroys=" << request.destroy.size();
        const auto reportError = [this, &settings, &ownerAccountId,
                                  &operationDescription](javelin::jmap::OperationError error)
        {
            m_errorCoordinator.reportFailure(*settings, ownerAccountId, operationDescription,
                                             error);
            return error;
        };
        undo::ContactCardHistory history{
            .connectionId = ownerAccountId,
            .accountId = request.accountId,
            .items = {},
        };
        std::unordered_map<std::string, std::size_t> creationItems;
        for (const auto& [creationId, document] : request.create)
        {
            auto item = createdHistoryItem(request.accountId, document.json);
            if (!item.has_value())
                co_return reportError(javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::PreconditionFailed,
                    .message = QStringLiteral(
                        "The created contact cannot be represented in operation history."),
                });
            creationItems.emplace(creationId, history.items.size());
            history.items.push_back(std::move(*item));
        }
        for (const auto& [contactId, document] : request.update)
        {
            auto found = m_contactRepository.findContact(request.accountId, contactId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&found))
                co_return reportError(javelin::jmap::operationError(*error));
            const auto& contact =
                std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(found);
            if (!contact.has_value())
                co_return reportError(javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::NotFound,
                    .message = QStringLiteral("The contact is no longer available."),
                });
            const auto after =
                javelin::jmap::api::applyPatchObject(contact->document, document.json);
            if (!std::holds_alternative<std::string>(after))
                co_return reportError(javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::PreconditionFailed,
                    .message = QStringLiteral("The contact update cannot be applied to its "
                                              "current cached document."),
                });
            history.items.push_back(existingHistoryItem(*contact, std::get<std::string>(after)));
        }
        for (const auto& contactId : request.destroy)
        {
            auto found = m_contactRepository.findContact(request.accountId, contactId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&found))
                co_return reportError(javelin::jmap::operationError(*error));
            const auto& contact =
                std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(found);
            if (!contact.has_value())
                co_return reportError(javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::NotFound,
                    .message = QStringLiteral("The contact is no longer available."),
                });
            history.items.push_back(existingHistoryItem(*contact, std::nullopt));
        }

        auto preparedResult = m_undoManager.prepareNormal(
            operationDescription, undo::HistoryDomain::Contacts, history, std::nullopt);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&preparedResult))
            co_return reportError(javelin::jmap::operationError(*error));
        auto prepared = std::get<std::optional<undo::HistoryEntry>>(std::move(preparedResult));
        auto result = co_await m_contactMutationEngine.setContactCards(
            toLiveConnectionSettings(*settings), ownerAccountId, std::move(request),
            {.refreshAndRetryStateMismatch = true, .traceId = traceId});
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            qCWarning(logContactCommands).noquote()
                << "action=" << QString::fromStdString(traceId) << "outcome=failure"
                << "code=" << QString::fromLatin1(javelin::jmap::toString(error->code))
                << "protocolType="
                << QString::fromStdString(error->protocolType.value_or(std::string{"<none>"}))
                << "message=" << error->message;
            if (prepared.has_value())
            {
                if (javelin::jmap::isTransientError(*error) &&
                    !javelin::jmap::isAuthenticationError(*error))
                {
                    auto committed = m_undoManager.commitNormal(std::move(*prepared));
                    if (const auto* databaseError =
                            std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
                        co_return reportError(javelin::jmap::operationError(*databaseError));
                    const auto& entry = std::get<undo::HistoryEntry>(committed);
                    if (const auto databaseError = m_undoManager.setEntryStatus(
                            entry.entryId, undo::HistoryEntryStatus::BlockedUnknown,
                            error->message))
                        co_return reportError(javelin::jmap::operationError(*databaseError));
                }
                else if (const auto databaseError = m_undoManager.discardNormal(prepared->entryId))
                    co_return reportError(javelin::jmap::operationError(*databaseError));
            }
            co_return reportError(*error);
        }
        const auto& summary = std::get<javelin::jmap::contacts::ContactMutationSummary>(result);
        qCInfo(logContactCommands).noquote()
            << "action=" << QString::fromStdString(traceId) << "outcome=accepted"
            << "state=" << QString::fromStdString(summary.newState)
            << "acceptedObjects=" << summary.receipt.acceptedObjectIds.size()
            << "rejectedObjects=" << summary.receipt.rejectedObjectIds.size();

        if (prepared.has_value())
        {
            auto& committedHistory = std::get<undo::ContactCardHistory>(prepared->payload);
            for (const auto& mapping : summary.createdIds)
                if (const auto found = creationItems.find(mapping.creationId);
                    found != creationItems.end())
                    committedHistory.items[found->second].currentCardId = mapping.serverId;
            if (summary.createdIds.size() != creationItems.size())
            {
                static_cast<void>(m_undoManager.setEntryStatus(
                    prepared->entryId, undo::HistoryEntryStatus::BlockedPartial,
                    i18n("The server returned incomplete contact creation identities.")));
            }
            else
            {
                auto committed = m_undoManager.commitNormal(std::move(*prepared));
                if (const auto* databaseError =
                        std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
                    co_return reportError(javelin::jmap::operationError(*databaseError));
            }
        }
        m_errorCoordinator.reportSuccess(settings->connectionId);
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
            .connectionId = ownerAccountId,
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
                    .message = i18n("The copied contact is no longer available."),
                };
            auto destination = javelin::jmap::api::applyPatchObject(contact->document, patch.json);
            if (!std::holds_alternative<std::string>(destination))
                co_return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::PreconditionFailed,
                    .message = i18n("The contact copy document is invalid."),
                };
            auto item = createdHistoryItem(request.accountId, std::get<std::string>(destination));
            if (!item.has_value())
                co_return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::PreconditionFailed,
                    .message = i18n("The copied contact cannot be represented in history."),
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
            co_await m_contactMutationEngine.copyContactCards(toLiveConnectionSettings(*settings),
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
                    .message = i18n("The contact copy response omitted a created identity."),
                };
            auto committed = m_undoManager.commitNormal(std::move(*prepared));
            if (const auto* databaseError =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
                co_return javelin::jmap::operationError(*databaseError);
        }
        co_return result;
    }
} // namespace javelin::app
