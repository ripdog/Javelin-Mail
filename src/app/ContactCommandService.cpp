#include "app/ContactCommandService.h"

#include "app/AccountConnectionProvider.h"
#include "app/ApplicationErrorCoordinator.h"
#include "app/ContactCommandPreparation.h"
#include "app/WorkScheduler.h"
#include "jmap/contacts/ContactService.h"

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
    } // namespace

    ContactCommandService::ContactCommandService(
        AccountConnectionProvider& connectionProvider,
        javelin::jmap::contacts::ContactService& contactService,
        ApplicationErrorCoordinator& errorCoordinator, WorkScheduler& workScheduler)
        : m_connectionProvider(connectionProvider), m_contactService(contactService),
          m_errorCoordinator(errorCoordinator), m_workScheduler(workScheduler)
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
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto settings = m_connectionProvider.connectionSettingsFor(ownerAccountId);
        if (!settings.has_value())
            co_return missingConfiguration();
        co_return observeResult(m_errorCoordinator, *settings, ownerAccountId,
                                QStringLiteral("Create contact group"),
                                co_await m_contactService.createGroup(
                                    toLiveConnectionSettings(*settings), ownerAccountId,
                                    javelin::jmap::contacts::CreateContactGroupCommand{
                                        .accountId = std::move(command.accountId),
                                        .addressBookId = std::move(command.addressBookId),
                                        .name = std::move(command.name),
                                    }));
    }

    QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
    ContactCommandService::setContactGroupMembership(std::string ownerAccountId,
                                                     SetContactGroupMembershipCommand command)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto settings = m_connectionProvider.connectionSettingsFor(ownerAccountId);
        if (!settings.has_value())
            co_return missingConfiguration();
        co_return observeResult(m_errorCoordinator, *settings, ownerAccountId,
                                QStringLiteral("Change contact group membership"),
                                co_await m_contactService.setGroupMembership(
                                    toLiveConnectionSettings(*settings), ownerAccountId,
                                    javelin::jmap::contacts::SetContactGroupMembershipCommand{
                                        .accountId = std::move(command.accountId),
                                        .groupId = std::move(command.groupId),
                                        .memberUids = std::move(command.memberUids),
                                        .included = command.included,
                                    }));
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
        co_return observeResult(
            m_errorCoordinator, *settings, ownerAccountId, std::move(operationDescription),
            co_await m_contactService.setContactCards(toLiveConnectionSettings(*settings),
                                                      ownerAccountId, std::move(request)));
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
        co_return observeResult(
            m_errorCoordinator, *settings, ownerAccountId, std::move(operationDescription),
            co_await m_contactService.copyContactCards(toLiveConnectionSettings(*settings),
                                                       ownerAccountId, std::move(request)));
    }
} // namespace javelin::app
