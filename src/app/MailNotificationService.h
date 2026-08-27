#pragma once

#include "storage/sqlite/DatabaseConnection.h"

#include <QObject>
#include <QStringList>

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

      private:
        javelin::jmap::cache::DatabaseConnection& m_databaseConnection;
    };

} // namespace javelin::app
