#include "jmap/cache/RawMessageSourceRepository.h"

#include "jmap/cache/MimeMessageParser.h"

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

    RawMessageSourceRepository::RawMessageSourceRepository(DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    std::optional<DatabaseError>
    RawMessageSourceRepository::upsert(const std::string_view accountId,
                                       const RawMessageSource& source)
    {
        if (const auto error = m_connection.validate())
        {
            return error;
        }

        auto& database = m_connection.database();
        if (!database.transaction())
        {
            return DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                 .message = QStringLiteral("Begin raw message source update: ") +
                                            database.lastError().text()};
        }

        QSqlQuery query{database};
        query.prepare(QStringLiteral("INSERT INTO raw_message_sources ("
                                     "account_id, email_id, blob_id, payload, fetched_at"
                                     ") VALUES ("
                                     ":account_id, :email_id, :blob_id, :payload, CURRENT_TIMESTAMP"
                                     ") ON CONFLICT(account_id, email_id) DO UPDATE SET "
                                     "blob_id = excluded.blob_id, "
                                     "payload = excluded.payload, "
                                     "fetched_at = CURRENT_TIMESTAMP"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":email_id"), QString::fromStdString(source.emailId));
        query.bindValue(QStringLiteral(":blob_id"), QString::fromStdString(source.blobId));
        query.bindValue(QStringLiteral(":payload"), source.payload);
        if (!query.exec())
        {
            database.rollback();
            return makeQueryError(QStringLiteral("Upsert raw message source"), query);
        }

        const auto parsed = parseMessageSource(source.emailId, source.payload);
        QString body;
        if (parsed.plainTextBody.has_value())
        {
            body += QString::fromStdString(parsed.plainTextBody->value);
        }
        if (parsed.htmlBody.has_value())
        {
            if (!body.isEmpty())
            {
                body += QLatin1Char('\n');
            }
            body += QString::fromStdString(parsed.htmlBody->value);
        }

        QSqlQuery indexQuery{database};
        indexQuery.prepare(QStringLiteral(
            "UPDATE email_search_fts SET body = :body, body_blob_id = :blob_id WHERE "
            "account_id = :account_id AND "
            "email_id = :email_id"));
        indexQuery.bindValue(QStringLiteral(":body"), body);
        indexQuery.bindValue(QStringLiteral(":blob_id"), QString::fromStdString(source.blobId));
        indexQuery.bindValue(QStringLiteral(":account_id"),
                             QString::fromStdString(std::string{accountId}));
        indexQuery.bindValue(QStringLiteral(":email_id"), QString::fromStdString(source.emailId));
        if (!indexQuery.exec())
        {
            database.rollback();
            return makeQueryError(QStringLiteral("Index raw message source"), indexQuery);
        }

        if (!database.commit())
        {
            return DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                 .message = QStringLiteral("Commit raw message source update: ") +
                                            database.lastError().text()};
        }

        return std::nullopt;
    }

    std::optional<DatabaseError>
    RawMessageSourceRepository::remove(const std::string_view accountId,
                                       const std::string_view emailId)
    {
        if (const auto error = m_connection.validate())
        {
            return error;
        }

        auto& database = m_connection.database();
        if (!database.transaction())
        {
            return DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                 .message = QStringLiteral("Begin raw message source removal: ") +
                                            database.lastError().text()};
        }
        QSqlQuery query{database};
        query.prepare(QStringLiteral(
            "DELETE FROM raw_message_sources WHERE account_id = :account_id AND email_id = "
            ":email_id"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":email_id"), QString::fromStdString(std::string{emailId}));
        if (!query.exec())
        {
            database.rollback();
            return makeQueryError(QStringLiteral("Delete raw message source"), query);
        }

        QSqlQuery indexQuery{database};
        indexQuery.prepare(
            QStringLiteral("UPDATE email_search_fts SET body = '', body_blob_id = '' WHERE "
                           "account_id = :account_id AND "
                           "email_id = :email_id"));
        indexQuery.bindValue(QStringLiteral(":account_id"),
                             QString::fromStdString(std::string{accountId}));
        indexQuery.bindValue(QStringLiteral(":email_id"),
                             QString::fromStdString(std::string{emailId}));
        if (!indexQuery.exec())
        {
            database.rollback();
            return makeQueryError(QStringLiteral("Remove raw message source index"), indexQuery);
        }
        if (!database.commit())
        {
            return DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                 .message = QStringLiteral("Commit raw message source removal: ") +
                                            database.lastError().text()};
        }

        return std::nullopt;
    }

    std::variant<std::optional<RawMessageSource>, DatabaseError>
    RawMessageSourceRepository::find(const std::string_view accountId,
                                     const std::string_view emailId) const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral("SELECT blob_id, payload "
                                     "FROM raw_message_sources "
                                     "WHERE account_id = :account_id AND email_id = :email_id"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":email_id"), QString::fromStdString(std::string{emailId}));
        if (!query.exec())
        {
            return makeQueryError(QStringLiteral("Read raw message source"), query);
        }

        if (!query.next())
        {
            return std::optional<RawMessageSource>{std::nullopt};
        }

        return std::optional<RawMessageSource>{RawMessageSource{
            .emailId = std::string{emailId},
            .blobId = query.value(0).toString().toStdString(),
            .payload = query.value(1).toByteArray(),
        }};
    }

} // namespace javelin::jmap::cache
