#include "jmap/cache/NotificationDispatchRepository.h"

#include <QSqlError>
#include <QSqlQuery>

namespace javelin::jmap::cache
{
    namespace
    {
        [[nodiscard]] QString kindValue(const NotificationDispatchKind kind)
        {
            switch (kind)
            {
            case NotificationDispatchKind::Mail:
                return QStringLiteral("mail");
            case NotificationDispatchKind::Calendar:
                return QStringLiteral("calendar");
            }
            Q_UNREACHABLE();
        }

        [[nodiscard]] DatabaseError queryError(const QString& operation, const QSqlQuery& query)
        {
            return databaseError(operation, query.lastError());
        }

        [[nodiscard]] std::optional<DatabaseError>
        validateTransaction(DatabaseConnection& connection, DatabaseTransaction& transaction)
        {
            if (const auto error = connection.validate())
                return error;
            if (!transaction.isActive() || &transaction.connection() != &connection)
            {
                return DatabaseError{
                    .code = DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral(
                        "Notification dispatch requires an active matching transaction"),
                };
            }
            return std::nullopt;
        }
    } // namespace

    NotificationDispatchRepository::NotificationDispatchRepository(DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    std::variant<bool, DatabaseError>
    NotificationDispatchRepository::claim(DatabaseTransaction& transaction,
                                          const NotificationDispatchKind kind,
                                          const std::string_view claimKey)
    {
        if (const auto error = validateTransaction(m_connection, transaction))
            return *error;
        QSqlQuery query{m_connection.database()};
        query.prepare(
            QStringLiteral("INSERT OR IGNORE INTO notification_dispatch_claims(kind,claim_key) "
                           "VALUES(:kind,:claim_key)"));
        query.bindValue(QStringLiteral(":kind"), kindValue(kind));
        query.bindValue(QStringLiteral(":claim_key"),
                        QString::fromStdString(std::string{claimKey}));
        if (!query.exec())
            return queryError(QStringLiteral("Claim notification dispatch"), query);
        return query.numRowsAffected() == 1;
    }

    std::optional<DatabaseError>
    NotificationDispatchRepository::release(DatabaseTransaction& transaction,
                                            const NotificationDispatchKind kind,
                                            const std::string_view claimKey)
    {
        if (const auto error = validateTransaction(m_connection, transaction))
            return error;
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "DELETE FROM notification_dispatch_claims WHERE kind=:kind AND claim_key=:claim_key"));
        query.bindValue(QStringLiteral(":kind"), kindValue(kind));
        query.bindValue(QStringLiteral(":claim_key"),
                        QString::fromStdString(std::string{claimKey}));
        if (!query.exec())
            return queryError(QStringLiteral("Release notification dispatch"), query);
        return std::nullopt;
    }

    std::optional<DatabaseError>
    NotificationDispatchRepository::recover(const NotificationDispatchKind kind)
    {
        const DatabaseWriteScope writeScope{m_connection};
        if (const auto error = m_connection.validate())
            return error;
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral("DELETE FROM notification_dispatch_claims WHERE kind=:kind"));
        query.bindValue(QStringLiteral(":kind"), kindValue(kind));
        if (!query.exec())
            return queryError(QStringLiteral("Recover notification dispatches"), query);
        return std::nullopt;
    }

} // namespace javelin::jmap::cache
