#pragma once

#include "storage/sqlite/DatabaseConnection.h"

#include "app/AccountConnectionSettings.h"

#include <QCoroTask>
#include <QObject>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace javelin::app
{
    class WorkScheduler;
    class MailIndexService;
} // namespace javelin::app
namespace javelin::jmap
{
    class JmapCore;
}
namespace javelin::jmap::cache
{
    class DatabaseConnection;
}

namespace javelin::app
{
    struct FullSyncAccountConfiguration
    {
        AccountConnectionSettings settings;
        std::string accountId;
        std::vector<std::string> mailboxIds;
    };

    class FullMailSyncService final : public QObject
    {
        Q_OBJECT

      public:
        FullMailSyncService(javelin::jmap::cache::DatabaseConnection& connection,
                            javelin::jmap::JmapCore& core, WorkScheduler& scheduler,
                            MailIndexService& indexService, QObject* parent = nullptr);

        void applySettings(std::vector<FullSyncAccountConfiguration> configurations);
        void refreshMailboxVisibility(std::string_view accountId);
        void requestCatchUp(std::string_view accountId);
        void requestMailboxResync(std::string_view accountId, std::string_view mailboxId);

      Q_SIGNALS:
        void mailboxWindowCommitted(QString accountId, QString mailboxId, quint64 offset,
                                    quint64 limit);
        void messageContentCommitted(QString accountId, QString emailId);

      private:
        struct Scope
        {
            std::string accountId;
            std::string mailboxId;
            std::string jobId;
        };

        void schedulePump();
        void pump();
        [[nodiscard]] QCoro::Task<void> run(Scope scope);
        [[nodiscard]] QCoro::Task<bool> waitForBackgroundNetwork(std::string jobId);
        [[nodiscard]] bool hasDiskSpace(std::string_view accountId, std::string_view mailboxId,
                                        std::uint64_t remainingBytes) const;
        [[nodiscard]] std::optional<bool> mailboxSubscribed(std::string_view accountId,
                                                            std::string_view mailboxId) const;
        [[nodiscard]] std::optional<AccountConnectionSettings>
        settingsFor(std::string_view accountId) const;

        javelin::jmap::cache::DatabaseConnection& m_connection;
        javelin::jmap::JmapCore& m_core;
        WorkScheduler& m_scheduler;
        MailIndexService& m_indexService;
        std::unordered_map<std::string, AccountConnectionSettings> m_settings;
        std::unordered_map<std::string, Scope> m_scopes;
        std::unordered_set<std::string> m_runningAccounts;
        std::unordered_set<std::string> m_dirtyAccounts;
        bool m_pumpScheduled = false;
    };
} // namespace javelin::app
