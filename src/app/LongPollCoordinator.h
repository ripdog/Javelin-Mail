#pragma once

#include "app/LongPollService.h"

#include <QObject>

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
        void stop();
        [[nodiscard]] LongPollService::Status status() const;

      Q_SIGNALS:
        void statusChanged(javelin::app::LongPollService::Status status);
        void mailStateChanged(const QString& accountId, bool requiresCatchUpRefresh);
        void accountMailStateChanged(const QString& accountId, const QString& refreshedMailboxId);
        void mailboxRefreshed(const QString& accountId, const QString& mailboxId,
                              bool scrollToNewest);
        void notificationRaised(const QString& accountId, const QString& mailboxId,
                                const QString& threadId, const QString& emailId,
                                const QString& mailboxName, const QString& title,
                                const QString& message);

      private:
        void connectService(LongPollService& service);
        void updateStatus();

        javelin::jmap::cache::DatabaseConnection& m_databaseConnection;
        javelin::jmap::api::AbstractTransport& m_transport;
        QNetworkAccessManager& m_networkAccessManager;
        javelin::jmap::cache::AccountRepository& m_accountRepository;
        javelin::jmap::cache::QueryService& m_queryService;
        std::unordered_map<std::string, std::unique_ptr<LongPollService>> m_services;
        LongPollService::Status m_status = LongPollService::Status::Disconnected;
    };

} // namespace javelin::app
