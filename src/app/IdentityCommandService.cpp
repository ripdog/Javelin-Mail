#include "app/IdentityCommandService.h"

#include "app/AccountConnectionProvider.h"
#include "app/ApplicationErrorCoordinator.h"
#include "app/MailApplicationPorts.h"
#include "app/WorkScheduler.h"
#include "jmap/cache/AccountReadRepository.h"
#include "jmap/identity/IdentityService.h"

#include <KLocalizedString>
#include <QUuid>

#include <utility>

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
        liveSettings(const AccountConnectionSettings& settings)
        {
            return {
                .sessionUrl = settings.sessionUrl,
                .loginEmail = settings.loginEmail,
                .apiKey = settings.apiKey,
            };
        }

        struct IdentityConnection
        {
            std::string ownerAccountId;
            AccountConnectionSettings settings;
        };

        [[nodiscard]] javelin::jmap::OperationError missingConfiguration()
        {
            return {
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = QStringLiteral("Synchronization is not configured for this account."),
            };
        }

        [[nodiscard]] javelin::jmap::OperationError unavailableAccount()
        {
            return {
                .code = javelin::jmap::OperationErrorCode::NotFound,
                .message = QStringLiteral("The sender Identity account is no longer available."),
            };
        }

        [[nodiscard]] javelin::jmap::OperationError unsupportedAccount()
        {
            return {
                .code = javelin::jmap::OperationErrorCode::UnsupportedCapability,
                .message =
                    QStringLiteral("This account does not support JMAP submission identities."),
            };
        }

        [[nodiscard]] std::variant<IdentityConnection, javelin::jmap::OperationError>
        resolveIdentityConnection(javelin::jmap::cache::AccountReader& accountReader,
                                  AccountConnectionProvider& connectionProvider,
                                  const std::string_view accountId)
        {
            const auto found = accountReader.findById(accountId);
            if (const auto* cacheError = std::get_if<javelin::jmap::cache::DatabaseError>(&found))
                return javelin::jmap::operationError(*cacheError);
            const auto& account =
                std::get<std::optional<javelin::jmap::cache::CachedAccount>>(found);
            if (!account.has_value())
                return unavailableAccount();
            if (!account->hasSubmissionCapability)
                return unsupportedAccount();
            const auto ownerAccountId =
                account->ownerAccountId.empty() ? account->accountId : account->ownerAccountId;
            const auto settings = connectionProvider.connectionSettingsFor(ownerAccountId);
            if (!settings.has_value())
                return missingConfiguration();
            return IdentityConnection{
                .ownerAccountId = ownerAccountId,
                .settings = *settings,
            };
        }

        template <typename Result>
        void observe(ApplicationErrorCoordinator& coordinator,
                     const AccountConnectionSettings& settings, const std::string_view accountId,
                     QString operation, const Result& result)
        {
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                coordinator.reportFailure(settings, accountId, std::move(operation), *error);
            else
                coordinator.reportSuccess(settings.connectionId);
        }

        [[nodiscard]] MailCacheChange identityCacheChange(const std::string_view accountId,
                                                          const bool optimisticProjection)
        {
            MailCacheChange change;
            change.accountId = QString::fromStdString(std::string{accountId});
            change.optimisticProjection = optimisticProjection;
            change.identitiesChanged = true;
            return change;
        }
    } // namespace

    IdentityCommandService::IdentityCommandService(
        javelin::jmap::identity::IdentityService& service,
        javelin::jmap::cache::AccountReader& accountReader,
        AccountConnectionProvider& connectionProvider,
        ApplicationErrorCoordinator& errorCoordinator, WorkScheduler& workScheduler,
        MailCacheChangePublisher& cacheChangePublisher)
        : m_service(service), m_accountReader(accountReader),
          m_connectionProvider(connectionProvider), m_errorCoordinator(errorCoordinator),
          m_workScheduler(workScheduler), m_cacheChangePublisher(cacheChangePublisher)
    {
    }

    QCoro::Task<javelin::jmap::identity::IdentityListResult>
    IdentityCommandService::requestSenderIdentities(std::string accountId)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto resolved =
            resolveIdentityConnection(m_accountReader, m_connectionProvider, accountId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&resolved))
            co_return *error;
        const auto& connection = std::get<IdentityConnection>(resolved);
        auto result = co_await m_service.refresh(liveSettings(connection.settings),
                                                 connection.ownerAccountId, accountId);
        observe(m_errorCoordinator, connection.settings, connection.ownerAccountId,
                i18n("Load sender identities"), result);
        if (!std::holds_alternative<javelin::jmap::OperationError>(result))
        {
            m_cacheChangePublisher.publishCacheChange(identityCacheChange(accountId, false));
        }
        co_return result;
    }

    QCoro::Task<javelin::jmap::identity::IdentitySaveResult>
    IdentityCommandService::saveSenderIdentity(std::string accountId,
                                               javelin::jmap::domain::Identity identity)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto resolved =
            resolveIdentityConnection(m_accountReader, m_connectionProvider, accountId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&resolved))
            co_return *error;
        const auto& connection = std::get<IdentityConnection>(resolved);
        const auto operationGroupId =
            QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        auto result = co_await m_service.save(
            liveSettings(connection.settings), connection.ownerAccountId, accountId,
            std::move(identity), operationGroupId, [this, accountId]
            { m_cacheChangePublisher.publishCacheChange(identityCacheChange(accountId, true)); });
        observe(m_errorCoordinator, connection.settings, connection.ownerAccountId,
                i18n("Save sender identity"), result);
        m_cacheChangePublisher.publishCacheChange(identityCacheChange(accountId, true));
        co_return result;
    }

    QCoro::Task<javelin::jmap::identity::IdentityDeleteResult>
    IdentityCommandService::deleteSenderIdentity(std::string accountId, std::string identityId)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto resolved =
            resolveIdentityConnection(m_accountReader, m_connectionProvider, accountId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&resolved))
            co_return *error;
        const auto& connection = std::get<IdentityConnection>(resolved);
        const auto operationGroupId =
            QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        auto result = co_await m_service.remove(
            liveSettings(connection.settings), connection.ownerAccountId, accountId,
            std::move(identityId), operationGroupId, [this, accountId]
            { m_cacheChangePublisher.publishCacheChange(identityCacheChange(accountId, true)); });
        observe(m_errorCoordinator, connection.settings, connection.ownerAccountId,
                i18n("Delete sender identity"), result);
        m_cacheChangePublisher.publishCacheChange(identityCacheChange(accountId, true));
        co_return result;
    }
} // namespace javelin::app
