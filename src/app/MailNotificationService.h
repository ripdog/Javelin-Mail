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
    class MailNotificationService final : public QObject
    {
        Q_OBJECT

      public:
        explicit MailNotificationService(
            javelin::jmap::cache::DatabaseConnection& databaseConnection,
            QObject* parent = nullptr);

        void accountChanged(const QString& accountId);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        markDelivered(std::string_view accountId, const QStringList& emailIds);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        releaseDispatches(std::string_view accountId, const QStringList& emailIds);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError> recoverDispatches();

      Q_SIGNALS:
        void notificationRaised(const QString& accountId, const QString& mailboxId,
                                const QString& threadId, const QString& emailId,
                                const QString& mailboxName, const QString& title,
                                const QString& message, const QStringList& deliveredEmailIds);
        void deliveryRetryRequired(const QString& accountId);

      private:
        using RetryMap = QHash<QString, QSet<QString>>;

        void rememberLocalRetry(RetryMap& retries, QString accountId, const QStringList& emailIds);
        void scheduleLocalRetry();
        void retryLocalFailures();

        javelin::jmap::cache::DatabaseConnection& m_databaseConnection;
        QTimer m_localRetryTimer;
        RetryMap m_markDeliveredRetries;
        RetryMap m_releaseDispatchRetries;
        unsigned int m_localRetryAttempts = 0;
    };

} // namespace javelin::app
