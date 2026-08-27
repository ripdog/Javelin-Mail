#pragma once

#include "app/AccountConnectionSettings.h"
#include "app/AccountRefreshApplicationPorts.h"
#include "app/CalendarInvitationAccountSource.h"
#include "app/MailApplicationTypes.h"
#include "app/account/AccountSyncCoordinator.h"
#include "app/account/EndpointRetryGate.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <QObject>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class QNetworkAccessManager;

namespace javelin::jmap
{
    class AccountBootstrapClient;
    class SessionRefreshClient;
} // namespace javelin::jmap

namespace javelin::jmap::api
{
    class JmapMethodTransport;
    class WebSocketFailureCooldowns;
} // namespace javelin::jmap::api

namespace javelin::jmap::cache
{
    class AccountRepository;
    class MailboxReader;
} // namespace javelin::jmap::cache

namespace javelin::app
{
    class ApplicationErrorCoordinator;
    class WorkScheduler;

    struct AccountSyncConfiguration
    {
        AccountConnectionSettings settings;
        std::string accountId;
        std::vector<std::string> mailboxIds;
        std::vector<std::string> fullSyncMailboxIds;
        std::vector<std::string> notificationMailboxIds;

        friend bool operator==(const AccountSyncConfiguration&,
                               const AccountSyncConfiguration&) = default;
    };

    struct AccountSyncConfigurationView
    {
        const AccountSyncConfiguration& second;
    };

    class AccountRuntimeManager final : public QObject, public CalendarInvitationAccountSource
    {
        Q_OBJECT

      public:
        AccountRuntimeManager(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                              javelin::jmap::SessionRefreshClient& sessionRefreshClient,
                              javelin::jmap::AccountBootstrapClient& accountBootstrapClient,
                              javelin::jmap::api::JmapMethodTransport& methodTransport,
                              QNetworkAccessManager& networkAccessManager,
                              javelin::jmap::api::WebSocketFailureCooldowns& cooldowns,
                              javelin::jmap::cache::AccountRepository& accountRepository,
                              javelin::jmap::cache::MailboxReader& mailboxReader,
                              ApplicationErrorCoordinator& errorCoordinator,
                              WorkScheduler& workScheduler, QObject* parent = nullptr);

        void applySettings(std::vector<AccountSyncConfiguration> configurations);
        void
        setAuthenticationRefreshHandler(javelin::jmap::auth::AccessTokenRefreshHandler handler);
        void networkBecameReachable();
        void setObservedMailboxIds(std::string accountId, std::vector<std::string> mailboxIds);
        [[nodiscard]] std::unordered_map<std::string, AccountSyncCoordinator::Status>
        accountStatuses() const;
        [[nodiscard]] std::optional<AccountConnectionSettings>
        connectionSettingsFor(std::string_view ownerAccountId) const override;
        [[nodiscard]] std::optional<AccountSyncConfigurationView>
        configurationFor(std::string_view accountId) const;
        [[nodiscard]] bool requestAccountSynchronization(std::string_view accountId);
        [[nodiscard]] bool requestMailboxSynchronization(std::string_view accountId,
                                                         std::string_view mailboxId);
        [[nodiscard]] std::vector<std::string> configuredAccountIds() const override;
        void refreshAccountConfiguration(std::string_view accountId);
        [[nodiscard]] QCoro::Task<javelin::jmap::LiveRefreshResult>
        bootstrapAccount(AccountBootstrapIntent intent);

      Q_SIGNALS:
        void accountStatusChanged(const QString& accountId,
                                  javelin::app::AccountSyncCoordinator::Status status);
        void sessionCapabilitiesChanged(const QString& ownerAccountId);
        void senderIdentityStateChanged(const QString& ownerAccountId);
        void cacheCommitted(javelin::app::MailCacheChange change);
        void contactStateChanged(const QString& ownerAccountId);
        void calendarStateChanged(const QString& ownerAccountId,
                                  const javelin::jmap::sync::AccountTypeStateMap& changedStates);
        void notificationEventsCommitted(const QString& accountId);
        void accountConfigured(const QString& accountId);
        void accountRemoved(const QString& accountId);
        void networkReachable();
        void sessionRefreshed(const QString& ownerAccountId);
        void stateChangeCatchUpRequired(const QString& ownerAccountId);
        void configurationSetChanged();

      private:
        void applyAccountConfiguration(const std::string& accountId);
        void refreshConfiguredSessions();
        void startSessionRefresh(const std::string& ownerAccountId,
                                 const AccountConnectionSettings& settings);
        void connectCoordinator(const std::string& accountId, AccountSyncCoordinator& coordinator);

        javelin::jmap::cache::DatabaseConnection& m_databaseConnection;
        javelin::jmap::SessionRefreshClient& m_sessionRefreshClient;
        javelin::jmap::AccountBootstrapClient& m_accountBootstrapClient;
        javelin::jmap::api::JmapMethodTransport& m_methodTransport;
        QNetworkAccessManager& m_networkAccessManager;
        javelin::jmap::api::WebSocketFailureCooldowns& m_transportCooldowns;
        javelin::jmap::cache::AccountRepository& m_accountRepository;
        javelin::jmap::cache::MailboxReader& m_mailboxReader;
        ApplicationErrorCoordinator& m_errorCoordinator;
        WorkScheduler& m_workScheduler;
        EndpointRetryGate m_endpointRetryGate;
        javelin::jmap::auth::AccessTokenRefreshHandler m_authenticationRefreshHandler;
        std::unordered_map<std::string, std::unique_ptr<AccountSyncCoordinator>> m_coordinators;
        std::unordered_map<std::string, AccountSyncConfiguration> m_configurations;
        std::unordered_map<std::string, std::vector<std::string>> m_observedMailboxIds;
        std::unordered_set<std::string> m_sessionRefreshesInFlight;
    };

} // namespace javelin::app
