#include "jmap/cache/MessageContentRepository.h"

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

    MessageContentRepository::MessageContentRepository(DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    std::optional<DatabaseError> MessageContentRepository::replaceForEmail(
        const std::string_view accountId, const std::string_view emailId,
        const std::vector<EmailPart>& parts, const std::vector<EmailBodyValue>& bodyValues)
    {
        if (const auto error = m_connection.validate())
        {
            return error;
        }

        QSqlDatabase& database = m_connection.database();
        if (!database.transaction())
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = "Begin message content transaction: " + database.lastError().text(),
            };
        }

        QSqlQuery deleteBodies{database};
        deleteBodies.prepare("DELETE FROM email_body_values WHERE account_id = :account_id AND "
                             "email_id = :email_id");
        deleteBodies.bindValue(":account_id", QString::fromStdString(std::string{accountId}));
        deleteBodies.bindValue(":email_id", QString::fromStdString(std::string{emailId}));
        if (!deleteBodies.exec())
        {
            database.rollback();
            return makeQueryError("Delete email body values", deleteBodies);
        }

        QSqlQuery deleteParts{database};
        deleteParts.prepare(
            "DELETE FROM email_parts WHERE account_id = :account_id AND email_id = :email_id");
        deleteParts.bindValue(":account_id", QString::fromStdString(std::string{accountId}));
        deleteParts.bindValue(":email_id", QString::fromStdString(std::string{emailId}));
        if (!deleteParts.exec())
        {
            database.rollback();
            return makeQueryError("Delete email parts", deleteParts);
        }

        QSqlQuery insertPart{database};
        insertPart.prepare(
            "INSERT INTO email_parts ("
            "account_id, email_id, part_id, parent_part_id, blob_id, kind, media_type, name, "
            "charset, disposition, cid, size, is_inline_renderable, is_body_section"
            ") VALUES ("
            ":account_id, :email_id, :part_id, :parent_part_id, :blob_id, :kind, :media_type, "
            ":name, :charset, :disposition, :cid, :size, :is_inline_renderable, :is_body_section)");
        for (const auto& part : parts)
        {
            insertPart.bindValue(":account_id", QString::fromStdString(std::string{accountId}));
            insertPart.bindValue(":email_id", QString::fromStdString(part.emailId));
            insertPart.bindValue(":part_id", QString::fromStdString(part.partId));
            insertPart.bindValue(":parent_part_id",
                                 part.parentPartId.has_value()
                                     ? QVariant{QString::fromStdString(*part.parentPartId)}
                                     : QVariant{});
            insertPart.bindValue(":blob_id", part.blobId.has_value()
                                                 ? QVariant{QString::fromStdString(*part.blobId)}
                                                 : QVariant{});
            insertPart.bindValue(":kind", QString::fromStdString(part.kind));
            insertPart.bindValue(":media_type", QString::fromStdString(part.mediaType));
            insertPart.bindValue(":name", part.name.has_value()
                                              ? QVariant{QString::fromStdString(*part.name)}
                                              : QVariant{});
            insertPart.bindValue(":charset", part.charset.has_value()
                                                 ? QVariant{QString::fromStdString(*part.charset)}
                                                 : QVariant{});
            insertPart.bindValue(":disposition",
                                 part.disposition.has_value()
                                     ? QVariant{QString::fromStdString(*part.disposition)}
                                     : QVariant{});
            insertPart.bindValue(":cid", part.cid.has_value()
                                             ? QVariant{QString::fromStdString(*part.cid)}
                                             : QVariant{});
            insertPart.bindValue(":size", static_cast<qulonglong>(part.size));
            insertPart.bindValue(":is_inline_renderable", part.isInlineRenderable ? 1 : 0);
            insertPart.bindValue(":is_body_section", part.isBodySection ? 1 : 0);
            if (!insertPart.exec())
            {
                database.rollback();
                return makeQueryError("Insert email part", insertPart);
            }
        }

        QSqlQuery insertBody{database};
        insertBody.prepare("INSERT INTO email_body_values ("
                           "account_id, email_id, part_id, blob_id, is_truncated, value"
                           ") VALUES ("
                           ":account_id, :email_id, :part_id, :blob_id, :is_truncated, :value)");
        for (const auto& bodyValue : bodyValues)
        {
            insertBody.bindValue(":account_id", QString::fromStdString(std::string{accountId}));
            insertBody.bindValue(":email_id", QString::fromStdString(bodyValue.emailId));
            insertBody.bindValue(":part_id", QString::fromStdString(bodyValue.partId));
            insertBody.bindValue(":blob_id",
                                 bodyValue.blobId.has_value()
                                     ? QVariant{QString::fromStdString(*bodyValue.blobId)}
                                     : QVariant{});
            insertBody.bindValue(":is_truncated", bodyValue.isTruncated ? 1 : 0);
            insertBody.bindValue(":value", QString::fromStdString(bodyValue.value));
            if (!insertBody.exec())
            {
                database.rollback();
                return makeQueryError("Insert email body value", insertBody);
            }
        }

        if (!database.commit())
        {
            database.rollback();
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = "Commit message content transaction: " + database.lastError().text(),
            };
        }

        return std::nullopt;
    }

    std::variant<std::vector<EmailPart>, DatabaseError>
    MessageContentRepository::loadParts(const std::string_view accountId,
                                        const std::string_view emailId) const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare("SELECT part_id, parent_part_id, blob_id, kind, media_type, name, charset, "
                      "disposition, "
                      "cid, size, is_inline_renderable, is_body_section "
                      "FROM email_parts WHERE account_id = :account_id AND email_id = :email_id "
                      "ORDER BY part_id");
        query.bindValue(":account_id", QString::fromStdString(std::string{accountId}));
        query.bindValue(":email_id", QString::fromStdString(std::string{emailId}));
        if (!query.exec())
        {
            return makeQueryError("Read email parts", query);
        }

        std::vector<EmailPart> parts;
        while (query.next())
        {
            parts.push_back(EmailPart{
                .emailId = std::string{emailId},
                .partId = query.value(0).toString().toStdString(),
                .parentPartId = query.value(1).isNull()
                                    ? std::nullopt
                                    : std::optional{query.value(1).toString().toStdString()},
                .blobId = query.value(2).isNull()
                              ? std::nullopt
                              : std::optional{query.value(2).toString().toStdString()},
                .kind = query.value(3).toString().toStdString(),
                .mediaType = query.value(4).toString().toStdString(),
                .name = query.value(5).isNull()
                            ? std::nullopt
                            : std::optional{query.value(5).toString().toStdString()},
                .charset = query.value(6).isNull()
                               ? std::nullopt
                               : std::optional{query.value(6).toString().toStdString()},
                .disposition = query.value(7).isNull()
                                   ? std::nullopt
                                   : std::optional{query.value(7).toString().toStdString()},
                .cid = query.value(8).isNull()
                           ? std::nullopt
                           : std::optional{query.value(8).toString().toStdString()},
                .size = query.value(9).toULongLong(),
                .isInlineRenderable = query.value(10).toInt() != 0,
                .isBodySection = query.value(11).toInt() != 0,
            });
        }

        return parts;
    }

    std::variant<std::vector<EmailBodyValue>, DatabaseError>
    MessageContentRepository::loadBodyValues(const std::string_view accountId,
                                             const std::string_view emailId) const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(
            "SELECT part_id, blob_id, is_truncated, value "
            "FROM email_body_values WHERE account_id = :account_id AND email_id = :email_id "
            "ORDER BY part_id");
        query.bindValue(":account_id", QString::fromStdString(std::string{accountId}));
        query.bindValue(":email_id", QString::fromStdString(std::string{emailId}));
        if (!query.exec())
        {
            return makeQueryError("Read email body values", query);
        }

        std::vector<EmailBodyValue> values;
        while (query.next())
        {
            values.push_back(EmailBodyValue{
                .emailId = std::string{emailId},
                .partId = query.value(0).toString().toStdString(),
                .blobId = query.value(1).isNull()
                              ? std::nullopt
                              : std::optional{query.value(1).toString().toStdString()},
                .isTruncated = query.value(2).toInt() != 0,
                .value = query.value(3).toString().toStdString(),
            });
        }

        return values;
    }

} // namespace javelin::jmap::cache
