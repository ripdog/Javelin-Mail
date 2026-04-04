#include "jmap/cache/InlinePartPayloadRepository.h"

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
                .message = operation + ": " + query.lastError().text(),
            };
        }

    } // namespace

    InlinePartPayloadRepository::InlinePartPayloadRepository(DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    std::optional<DatabaseError>
    InlinePartPayloadRepository::upsert(const std::string_view accountId,
                                        const InlinePartPayload& payload)
    {
        if (const auto error = m_connection.validate())
        {
            return error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(
            "INSERT INTO inline_part_payloads ("
            "account_id, email_id, part_id, blob_id, media_type, payload"
            ") VALUES ("
            ":account_id, :email_id, :part_id, :blob_id, :media_type, :payload"
            ") ON CONFLICT(account_id, email_id, part_id) DO UPDATE SET "
            "blob_id = excluded.blob_id, "
            "media_type = excluded.media_type, "
            "payload = excluded.payload, "
            "fetched_at = CURRENT_TIMESTAMP");
        query.bindValue(":account_id", QString::fromStdString(std::string{accountId}));
        query.bindValue(":email_id", QString::fromStdString(payload.emailId));
        query.bindValue(":part_id", QString::fromStdString(payload.partId));
        query.bindValue(":blob_id", QString::fromStdString(payload.blobId));
        query.bindValue(":media_type", QString::fromStdString(payload.mediaType));
        query.bindValue(":payload", payload.payload);
        if (!query.exec())
        {
            return makeQueryError("Upsert inline part payload", query);
        }

        return std::nullopt;
    }

    std::variant<std::optional<InlinePartPayload>, DatabaseError>
    InlinePartPayloadRepository::find(const std::string_view accountId, const std::string_view emailId,
                                      const std::string_view partId) const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(
            "SELECT blob_id, media_type, payload "
            "FROM inline_part_payloads "
            "WHERE account_id = :account_id AND email_id = :email_id AND part_id = :part_id");
        query.bindValue(":account_id", QString::fromStdString(std::string{accountId}));
        query.bindValue(":email_id", QString::fromStdString(std::string{emailId}));
        query.bindValue(":part_id", QString::fromStdString(std::string{partId}));
        if (!query.exec())
        {
            return makeQueryError("Read inline part payload", query);
        }

        if (!query.next())
        {
            return std::optional<InlinePartPayload>{std::nullopt};
        }

        return std::optional<InlinePartPayload>{
            InlinePartPayload{
                .emailId = std::string{emailId},
                .partId = std::string{partId},
                .blobId = query.value(0).toString().toStdString(),
                .mediaType = query.value(1).toString().toStdString(),
                .payload = query.value(2).toByteArray(),
            }};
    }

} // namespace javelin::jmap::cache
