#include "jmap/cache/NotificationRepository.h"

#include <QSqlError>
#include <QSqlQuery>

namespace javelin::jmap::cache
{
    namespace
    {
        [[nodiscard]] DatabaseError queryError(const QString& operation, const QSqlQuery& query)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = operation + QStringLiteral(": ") + query.lastError().text(),
            };
        }
    } // namespace

    NotificationRepository::NotificationRepository(DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    std::variant<std::vector<javelin::jmap::sync::RefreshNotificationCandidate>, DatabaseError>
    NotificationRepository::claimUnreadMailboxEmails(const std::string_view accountId,
                                                     const std::string_view mailboxId)
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        auto& database = m_connection.database();
        if (!database.transaction())
        {
            return DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                 .message = QStringLiteral("Begin notification claim: ") +
                                            database.lastError().text()};
        }

        QSqlQuery emails{database};
        emails.prepare(QStringLiteral(
            "SELECT e.email_id, e.thread_id, e.subject, e.received_at, "
            "EXISTS(SELECT 1 FROM email_keywords k WHERE k.account_id = e.account_id "
            "AND k.email_id = e.email_id AND k.keyword = '$seen') "
            "FROM emails e JOIN email_mailboxes m ON m.account_id = e.account_id "
            "AND m.email_id = e.email_id WHERE e.account_id = :account_id "
            "AND m.mailbox_id = :mailbox_id ORDER BY e.received_at, e.email_id"));
        emails.bindValue(QStringLiteral(":account_id"),
                         QString::fromStdString(std::string{accountId}));
        emails.bindValue(QStringLiteral(":mailbox_id"),
                         QString::fromStdString(std::string{mailboxId}));
        if (!emails.exec())
        {
            database.rollback();
            return queryError(QStringLiteral("Read notification mailbox"), emails);
        }

        QSqlQuery observe{database};
        observe.prepare(QStringLiteral(
            "INSERT OR IGNORE INTO observed_notification_emails (account_id, email_id) "
            "VALUES (:account_id, :email_id)"));

        std::vector<javelin::jmap::sync::RefreshNotificationCandidate> candidates;
        while (emails.next())
        {
            const auto emailId = emails.value(0).toString();
            observe.bindValue(QStringLiteral(":account_id"),
                              QString::fromStdString(std::string{accountId}));
            observe.bindValue(QStringLiteral(":email_id"), emailId);
            if (!observe.exec())
            {
                database.rollback();
                return queryError(QStringLiteral("Record observed notification email"), observe);
            }
            if (observe.numRowsAffected() == 1 && !emails.value(4).toBool())
            {
                candidates.push_back(javelin::jmap::sync::RefreshNotificationCandidate{
                    .emailId = emailId.toStdString(),
                    .threadId = emails.value(1).toString().toStdString(),
                    .subject = emails.value(2).isNull()
                                   ? std::nullopt
                                   : std::optional{emails.value(2).toString().toStdString()},
                    .receivedAt = emails.value(3).toString().toStdString(),
                });
            }
            observe.finish();
        }

        if (!database.commit())
        {
            database.rollback();
            return DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                 .message = QStringLiteral("Commit notification claim: ") +
                                            database.lastError().text()};
        }
        return candidates;
    }

} // namespace javelin::jmap::cache
