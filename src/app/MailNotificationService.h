#pragma once

#include "storage/sqlite/DatabaseConnection.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include <optional>
#include <string_view>

namespace javelin::app
{
    class MailNotificationDeliveryPort;

    class MailNotificationService final : public QObject
    {
        Q_OBJECT

      public:
        explicit MailNotificationService(
            javelin::jmap::cache::DatabaseConnection& databaseConnection,
            QObject* parent = nullptr);

        void setDeliveryPort(MailNotificationDeliveryPort* deliveryPort);
        void accountChanged(const QString& accountId);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError> recoverDispatches();

      private:
        using RetryMap = QHash<QString, QSet<QString>>;

        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        markDelivered(std::string_view accountId, const QStringList& emailIds);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        releaseDispatches(std::string_view accountId, const QStringList& emailIds);

        void rememberLocalRetry(RetryMap& retries, QString accountId, const QStringList& emailIds);
        void scheduleLocalRetry();
        void retryLocalFailures();
        void queueDeliveryRetry(QString accountId);
        void retryDeliveries();

        javelin::jmap::cache::DatabaseConnection& m_databaseConnection;
        MailNotificationDeliveryPort* m_deliveryPort = nullptr;
        QTimer m_localRetryTimer;
        QTimer m_deliveryRetryTimer;
        QSet<QString> m_deliveryRetryAccounts;
        RetryMap m_markDeliveredRetries;
        RetryMap m_releaseDispatchRetries;
        unsigned int m_localRetryAttempts = 0;
    };

} // namespace javelin::app
