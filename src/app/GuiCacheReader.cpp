#include "app/GuiCacheReader.h"

#include "jmap/cache/Database.h"
#include "jmap/cache/MessageViewService.h"

#include <QSqlError>
#include <QSqlQuery>

#include <algorithm>
#include <utility>

namespace javelin::app
{
    namespace
    {
        [[nodiscard]] QString queryError(const QString& operation, const QSqlQuery& query)
        {
            return operation + QStringLiteral(": ") + query.lastError().text();
        }

        [[nodiscard]] std::variant<javelin::jmap::cache::ReadOnlyDatabaseConnection, QString>
        openDatabase(const QString& databasePath)
        {
            if (databasePath.isEmpty())
                return QStringLiteral("the daemon did not provide a cache path");

            auto opened =
                javelin::jmap::cache::ReadOnlyThreadConnectionFactory{
                    {.connectionNamePrefix = QStringLiteral("javelin-gui-snapshot"),
                     .databasePath = databasePath}}
                    .openForCurrentThread("workspace");
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&opened))
                return error->message;
            return std::get<javelin::jmap::cache::ReadOnlyDatabaseConnection>(std::move(opened));
        }

        [[nodiscard]] QString senderText(const QSqlQuery& query, const int nameColumn,
                                         const int addressColumn)
        {
            const auto name = query.value(nameColumn).toString().trimmed();
            const auto address = query.value(addressColumn).toString().trimmed();
            if (name.isEmpty())
                return address;
            if (address.isEmpty() || name.compare(address, Qt::CaseInsensitive) == 0)
                return name;
            return QStringLiteral("%1 <%2>").arg(name, address);
        }
    } // namespace

    GuiCacheReader::GuiCacheReader(QString databasePath) : m_databasePath(std::move(databasePath))
    {
    }

    GuiCacheReader::SnapshotResult GuiCacheReader::loadSnapshot() const
    {
        auto opened = openDatabase(m_databasePath);
        if (const auto* error = std::get_if<QString>(&opened))
            return *error;
        auto connection =
            std::get<javelin::jmap::cache::ReadOnlyDatabaseConnection>(std::move(opened));

        CacheSnapshot snapshot;
        QSqlQuery accounts{connection.database()};
        if (!accounts.exec(QStringLiteral(
                "SELECT account_id,email_address FROM accounts ORDER BY email_address,account_id")))
            return queryError(QStringLiteral("Load accounts"), accounts);
        while (accounts.next())
        {
            snapshot.accounts.push_back({.accountId = accounts.value(0).toString(),
                                         .address = accounts.value(1).toString()});
        }

        QSqlQuery mailboxes{connection.database()};
        if (!mailboxes.exec(
                QStringLiteral("SELECT account_id,mailbox_id,name,COALESCE(role,''),total_emails,"
                               "unread_emails FROM mailboxes ORDER BY account_id,sort_order,name")))
            return queryError(QStringLiteral("Load mailboxes"), mailboxes);
        while (mailboxes.next())
        {
            snapshot.mailboxes.push_back({
                .accountId = mailboxes.value(0).toString(),
                .mailboxId = mailboxes.value(1).toString(),
                .name = mailboxes.value(2).toString(),
                .role = mailboxes.value(3).toString(),
                .totalEmails = mailboxes.value(4).toULongLong(),
                .unreadEmails = mailboxes.value(5).toULongLong(),
            });
        }
        return snapshot;
    }

    GuiCacheReader::MessageListResult GuiCacheReader::loadMessages(const QString& accountId,
                                                                   const QString& mailboxId,
                                                                   const std::size_t limit) const
    {
        auto opened = openDatabase(m_databasePath);
        if (const auto* error = std::get_if<QString>(&opened))
            return *error;
        auto connection =
            std::get<javelin::jmap::cache::ReadOnlyDatabaseConnection>(std::move(opened));

        QSqlQuery query{connection.database()};
        query.prepare(QStringLiteral(
            "SELECT e.account_id,e.email_id,COALESCE(e.thread_id,''),e.subject,e.preview,"
            "COALESCE(e.received_at,''),e.has_attachment,"
            "COALESCE(a.display_name,''),COALESCE(a.address,''),"
            "NOT EXISTS(SELECT 1 FROM email_keywords seen WHERE seen.account_id=e.account_id "
            "AND seen.email_id=e.email_id AND seen.keyword='$seen'),"
            "EXISTS(SELECT 1 FROM email_keywords flagged WHERE flagged.account_id=e.account_id "
            "AND flagged.email_id=e.email_id AND flagged.keyword='$flagged') "
            "FROM emails e JOIN email_mailboxes em ON em.account_id=e.account_id "
            "AND em.email_id=e.email_id LEFT JOIN email_addresses a ON a.account_id=e.account_id "
            "AND a.email_id=e.email_id AND a.field_name='from' AND a.position=0 "
            "WHERE e.account_id=:account_id AND em.mailbox_id=:mailbox_id "
            "ORDER BY e.received_at DESC,e.email_id DESC LIMIT :limit"));
        query.bindValue(QStringLiteral(":account_id"), accountId);
        query.bindValue(QStringLiteral(":mailbox_id"), mailboxId);
        query.bindValue(QStringLiteral(":limit"),
                        static_cast<qulonglong>(std::clamp<std::size_t>(limit, 1, 500)));
        if (!query.exec())
            return queryError(QStringLiteral("Load messages"), query);

        std::vector<MessageSummary> messages;
        while (query.next())
        {
            messages.push_back({
                .accountId = query.value(0).toString(),
                .emailId = query.value(1).toString(),
                .threadId = query.value(2).toString(),
                .mailboxId = mailboxId,
                .subject = query.value(3).toString(),
                .preview = query.value(4).toString(),
                .sender = senderText(query, 7, 8),
                .receivedAt = query.value(5).toString(),
                .unread = query.value(9).toBool(),
                .flagged = query.value(10).toBool(),
                .hasAttachment = query.value(6).toBool(),
            });
        }
        return messages;
    }

    GuiCacheReader::MessageDetailResult GuiCacheReader::loadMessage(const QString& accountId,
                                                                    const QString& emailId) const
    {
        auto opened = openDatabase(m_databasePath);
        if (const auto* error = std::get_if<QString>(&opened))
            return *error;
        auto connection =
            std::get<javelin::jmap::cache::ReadOnlyDatabaseConnection>(std::move(opened));

        QSqlQuery query{connection.database()};
        query.prepare(
            QStringLiteral("SELECT e.subject,e.received_at,e.preview,COALESCE(a.display_name,''),"
                           "COALESCE(a.address,'') FROM emails e LEFT JOIN email_addresses a ON "
                           "a.account_id=e.account_id AND a.email_id=e.email_id AND "
                           "a.field_name='from' AND a.position=0 WHERE e.account_id=:account_id "
                           "AND e.email_id=:email_id"));
        query.bindValue(QStringLiteral(":account_id"), accountId);
        query.bindValue(QStringLiteral(":email_id"), emailId);
        if (!query.exec())
            return queryError(QStringLiteral("Load message"), query);
        if (!query.next())
            return QStringLiteral("The selected message is no longer in the local cache.");

        MessageDetail detail{.subject = query.value(0).toString(),
                             .sender = senderText(query, 3, 4),
                             .receivedAt = query.value(1).toString(),
                             .preview = query.value(2).toString(),
                             .body = query.value(2).toString()};
        javelin::jmap::cache::MessageViewService messageView{connection};
        const auto viewResult = messageView.load(accountId.toStdString(), emailId.toStdString());
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&viewResult))
            return error->message;
        const auto& view =
            std::get<std::optional<javelin::jmap::cache::MessageViewSnapshot>>(viewResult);
        if (view.has_value())
        {
            if (view->plainTextBody.has_value())
            {
                detail.body = QString::fromStdString(view->plainTextBody->value);
            }
            else if (view->htmlRenderDocument.has_value())
            {
                detail.body = QString::fromStdString(view->htmlRenderDocument->html);
                detail.bodyIsHtml = true;
            }
        }
        return detail;
    }
} // namespace javelin::app
