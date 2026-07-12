#pragma once

#include "app/LongPollService.h"
#include "jmap/query/EmailListSort.h"
#include "jmap/search/EmailSearch.h"

#include <QObject>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace javelin::app
{

    struct MailboxWindowIntent
    {
        std::string accountId;
        std::string mailboxId;
        std::size_t offset = 0;
        std::size_t limit = 0;
        javelin::jmap::query::EmailListSort sort;
    };

    struct SearchWindowIntent
    {
        std::string accountId;
        javelin::jmap::search::EmailSearchCriteria criteria;
        std::size_t offset = 0;
        std::size_t limit = 0;
        javelin::jmap::query::EmailListSort sort;
    };

    struct LongPollAccountConfiguration
    {
        javelin::jmap::LiveConnectionSettings settings;
        std::string accountId;
        std::vector<std::string> mailboxIds;
    };

    class LongPollCoordinator final : public QObject
    {
        Q_OBJECT

      public:
        LongPollCoordinator(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                            javelin::jmap::JmapCore& jmapCore,
                            javelin::jmap::api::AbstractTransport& transport,
                            QNetworkAccessManager& networkAccessManager,
                            javelin::jmap::cache::AccountRepository& accountRepository,
                            javelin::jmap::cache::QueryService& queryService,
                            QObject* parent = nullptr);

        void applySettings(std::vector<LongPollAccountConfiguration> configurations);
        [[nodiscard]] std::uint64_t observeMailbox(std::string accountId, std::string mailboxId);
        void unobserveMailbox(std::uint64_t observationId);
        [[nodiscard]] bool requestAccountSynchronization(std::string_view accountId);
        [[nodiscard]] QCoro::Task<javelin::jmap::MailboxPageResult>
        requestMailboxWindow(MailboxWindowIntent intent);
        [[nodiscard]] QCoro::Task<javelin::jmap::MessageSearchResult>
        requestSearchWindow(SearchWindowIntent intent);
        void stop();

      Q_SIGNALS:
        void accountStatusChanged(const QString& accountId,
                                  javelin::app::LongPollService::Status status);
        void cacheCommitted(javelin::app::MailCacheChange change);
        void notificationRaised(const QString& accountId, const QString& mailboxId,
                                const QString& threadId, const QString& emailId,
                                const QString& mailboxName, const QString& title,
                                const QString& message);

      private:
        void connectService(const std::string& accountId, LongPollService& service);
        void applyAccountConfiguration(const std::string& accountId);

        struct MailboxObservation
        {
            std::string accountId;
            std::string mailboxId;
        };

        javelin::jmap::cache::DatabaseConnection& m_databaseConnection;
        javelin::jmap::JmapCore& m_jmapCore;
        javelin::jmap::api::AbstractTransport& m_transport;
        QNetworkAccessManager& m_networkAccessManager;
        javelin::jmap::cache::AccountRepository& m_accountRepository;
        javelin::jmap::cache::QueryService& m_queryService;
        std::unordered_map<std::string, std::unique_ptr<LongPollService>> m_services;
        std::unordered_map<std::string, LongPollAccountConfiguration> m_configurations;
        std::unordered_map<std::uint64_t, MailboxObservation> m_observations;
        std::uint64_t m_nextObservationId = 1;
    };

} // namespace javelin::app
