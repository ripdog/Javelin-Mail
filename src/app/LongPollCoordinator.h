#pragma once

#include "app/LongPollService.h"

#include <QObject>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace javelin::app
{

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
                            javelin::jmap::api::AbstractTransport& transport,
                            QNetworkAccessManager& networkAccessManager,
                            javelin::jmap::cache::AccountRepository& accountRepository,
                            javelin::jmap::cache::QueryService& queryService,
                            QObject* parent = nullptr);

        void applySettings(std::vector<LongPollAccountConfiguration> configurations);
        [[nodiscard]] std::uint64_t observeMailbox(std::string accountId, std::string mailboxId);
        void unobserveMailbox(std::uint64_t observationId);
        [[nodiscard]] bool requestAccountSynchronization(std::string_view accountId);
        void stop();

      Q_SIGNALS:
        void accountStatusChanged(const QString& accountId,
                                  javelin::app::LongPollService::Status status);
        void mailStateChanged(const QString& accountId, bool requiresCatchUpRefresh);
        void accountMailStateChanged(const QString& accountId, const QString& refreshedMailboxId);
        void mailboxRefreshed(const QString& accountId, const QString& mailboxId,
                              bool scrollToNewest);
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
