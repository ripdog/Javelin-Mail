#include "jmap/cache/SubmissionRepository.h"

#include <QSqlError>
#include <QSqlQuery>

namespace javelin::jmap::cache
{

    namespace
    {

        [[nodiscard]] DatabaseError makeQueryError(const QString& operation, const QSqlQuery& query)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = operation + QStringLiteral(": ") + query.lastError().text(),
            };
        }

    } // namespace

    SubmissionRepository::SubmissionRepository(DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    std::optional<DatabaseError> SubmissionRepository::upsert(DatabaseTransaction& transaction,
                                                              const SubmissionRecord& record)
    {
        if (const auto error = m_connection.validate())
            return error;
        if (!transaction.isActive() || &transaction.connection() != &m_connection)
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Submission update requires a matching transaction"),
            };
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "INSERT INTO submissions ("
            "account_id, submission_id, email_id, thread_id, envelope_json, undo_status, "
            "delivery_status, state"
            ") VALUES ("
            ":account_id, :submission_id, :email_id, :thread_id, '{}', :undo_status, "
            ":delivery_status, NULL"
            ") ON CONFLICT(account_id, submission_id) DO UPDATE SET "
            "email_id = excluded.email_id, "
            "thread_id = excluded.thread_id, "
            "undo_status = excluded.undo_status, "
            "delivery_status = excluded.delivery_status"));
        query.bindValue(QStringLiteral(":account_id"), QString::fromStdString(record.accountId));
        query.bindValue(QStringLiteral(":submission_id"),
                        QString::fromStdString(record.submissionId));
        query.bindValue(QStringLiteral(":email_id"), QString::fromStdString(record.emailId));
        query.bindValue(QStringLiteral(":thread_id"),
                        record.threadId.has_value()
                            ? QVariant{QString::fromStdString(*record.threadId)}
                            : QVariant{});
        query.bindValue(QStringLiteral(":undo_status"),
                        record.undoStatus.has_value() ? QVariant{*record.undoStatus} : QVariant{});
        query.bindValue(QStringLiteral(":delivery_status"),
                        record.deliveryStatusJson.has_value() ? QVariant{*record.deliveryStatusJson}
                                                              : QVariant{});
        if (!query.exec())
        {
            return makeQueryError(QStringLiteral("Upsert submission"), query);
        }

        return std::nullopt;
    }

} // namespace javelin::jmap::cache
