#pragma once

#include "storage/sqlite/DatabaseConnection.h"

#include "jmap/cache/MailboxMessageReadRepository.h"
#include "jmap/cache/ThreadReadRepository.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QString>

#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace javelin::app
{
    struct MessageListThreadMembersSnapshot
    {
        std::vector<javelin::jmap::cache::MessageListItem> items;
        bool complete = false;
    };

    using MessageListThreadMembersResult =
        std::variant<MessageListThreadMembersSnapshot, javelin::jmap::cache::DatabaseError>;

    [[nodiscard]] inline MessageListThreadMembersResult
    loadMessageListThreadMembers(const QString& databasePath, const std::string& accountId,
                                 const std::optional<std::string>& mailboxId,
                                 const std::string& threadId)
    {
        javelin::jmap::cache::ReadOnlyThreadConnectionFactory factory{
            {.connectionNamePrefix = QStringLiteral("javelin-thread-members"),
             .databasePath = databasePath}};
        auto connectionResult = factory.openForCurrentThread("snapshot");
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&connectionResult))
        {
            return *error;
        }
        auto connection =
            std::get<javelin::jmap::cache::ReadOnlyDatabaseConnection>(std::move(connectionResult));
        auto database = connection.database();
        if (!database.transaction())
        {
            return javelin::jmap::cache::databaseError(
                QStringLiteral("Begin message-list Thread snapshot"), database.lastError());
        }
        javelin::jmap::cache::MailboxMessageReadRepository mailboxMessages{connection};
        javelin::jmap::cache::ThreadReadRepository threadReader{connection};
        bool completeOfflineMailbox = false;
        if (mailboxId.has_value())
        {
            const auto offlineState = mailboxMessages.offlineMailboxComplete(accountId, *mailboxId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&offlineState))
            {
                static_cast<void>(database.rollback());
                return *error;
            }
            completeOfflineMailbox = std::get<bool>(offlineState);
        }
        QSqlQuery coverage{database};
        coverage.prepare(QStringLiteral(
            "SELECT t.membership_freshness,t.member_count,COUNT(m.email_id),COUNT(e.email_id) FROM "
            "threads t LEFT "
            "JOIN thread_email_members m ON m.account_id=t.account_id AND "
            "m.thread_id=t.thread_id LEFT JOIN emails e ON e.account_id=m.account_id AND "
            "e.email_id=m.email_id AND e.thread_id=m.thread_id AND NOT EXISTS(SELECT 1 FROM "
            "email_summary_refresh_requests refresh WHERE refresh.account_id=m.account_id AND "
            "refresh.email_id=m.email_id) WHERE t.account_id=:account_id AND "
            "t.thread_id=:thread_id GROUP BY t.account_id,t.thread_id,t.membership_freshness,"
            "t.member_count"));
        coverage.bindValue(QStringLiteral(":account_id"), QString::fromStdString(accountId));
        coverage.bindValue(QStringLiteral(":thread_id"), QString::fromStdString(threadId));
        if (!coverage.exec())
        {
            static_cast<void>(database.rollback());
            return javelin::jmap::cache::databaseError(
                QStringLiteral("Read message-list Thread coverage"), coverage.lastError());
        }
        const bool normalizedThreadComplete =
            coverage.next() && coverage.value(0).toString() == QStringLiteral("current") &&
            coverage.value(1).toULongLong() == coverage.value(2).toULongLong() &&
            coverage.value(1).toULongLong() == coverage.value(3).toULongLong();
        if (!normalizedThreadComplete && !completeOfflineMailbox)
        {
            if (!database.commit())
            {
                return javelin::jmap::cache::databaseError(
                    QStringLiteral("Finish incomplete message-list Thread snapshot"),
                    database.lastError());
            }
            return MessageListThreadMembersSnapshot{};
        }

        auto itemsResult =
            normalizedThreadComplete
                ? threadReader.listThreadMessages(accountId, threadId)
                : threadReader.listMailboxThreadMessages(
                      accountId, *mailboxId, threadId,
                      javelin::jmap::cache::MailboxThreadMembershipSource::CompleteOfflineMailbox);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&itemsResult))
        {
            static_cast<void>(database.rollback());
            return *error;
        }
        auto snapshot = MessageListThreadMembersSnapshot{
            .items = std::get<std::vector<javelin::jmap::cache::MessageListItem>>(
                std::move(itemsResult)),
            .complete = true,
        };
        if (!database.commit())
        {
            return javelin::jmap::cache::databaseError(
                QStringLiteral("Finish complete message-list Thread snapshot"),
                database.lastError());
        }
        return snapshot;
    }
} // namespace javelin::app
