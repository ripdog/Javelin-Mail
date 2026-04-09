#include "jmap/cache/EmailRepository.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>

#include <array>
#include <unordered_set>

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

        [[nodiscard]] QString serializeStringList(const std::vector<std::string>& values)
        {
            QJsonArray array;
            for (const auto& value : values)
            {
                array.push_back(QString::fromStdString(value));
            }
            return QString::fromUtf8(QJsonDocument{array}.toJson(QJsonDocument::Compact));
        }

        [[nodiscard]] std::vector<std::string> deserializeStringList(const QString& json)
        {
            const auto array = QJsonDocument::fromJson(json.toUtf8()).array();
            std::vector<std::string> values;
            values.reserve(static_cast<std::size_t>(array.size()));
            for (const auto& value : array)
            {
                const auto stringValue = value.toString();
                if (!stringValue.isEmpty())
                {
                    values.push_back(stringValue.toStdString());
                }
            }
            return values;
        }

        std::optional<DatabaseError>
        insertAddresses(QSqlDatabase& database, std::string_view accountId,
                        std::string_view emailId, std::string_view fieldName,
                        const std::vector<javelin::jmap::domain::EmailAddress>& addresses)
        {
            QSqlQuery query{database};
            query.prepare(QStringLiteral(
                "INSERT INTO email_addresses ("
                "account_id, email_id, field_name, position, display_name, address"
                ") VALUES ("
                ":account_id, :email_id, :field_name, :position, :display_name, :address)"));

            int position = 0;
            for (const auto& address : addresses)
            {
                query.bindValue(QStringLiteral(":account_id"),
                                QString::fromStdString(std::string{accountId}));
                query.bindValue(QStringLiteral(":email_id"),
                                QString::fromStdString(std::string{emailId}));
                query.bindValue(QStringLiteral(":field_name"),
                                QString::fromStdString(std::string{fieldName}));
                query.bindValue(QStringLiteral(":position"), position);
                query.bindValue(QStringLiteral(":display_name"),
                                address.name.has_value()
                                    ? QVariant{QString::fromStdString(*address.name)}
                                    : QVariant{});
                query.bindValue(QStringLiteral(":address"), QString::fromStdString(address.email));
                if (!query.exec())
                {
                    return makeQueryError(QStringLiteral("Insert email address"), query);
                }

                ++position;
            }

            return std::nullopt;
        }

        [[nodiscard]] std::vector<javelin::jmap::domain::EmailAddress>
        loadAddresses(QSqlDatabase& database, std::string_view accountId, std::string_view emailId,
                      std::string_view fieldName, std::optional<DatabaseError>& error)
        {
            QSqlQuery query{database};
            query.prepare(QStringLiteral(
                "SELECT display_name, address "
                "FROM email_addresses "
                "WHERE account_id = :account_id AND email_id = :email_id AND field_name "
                "= :field_name "
                "ORDER BY position"));
            query.bindValue(QStringLiteral(":account_id"),
                            QString::fromStdString(std::string{accountId}));
            query.bindValue(QStringLiteral(":email_id"),
                            QString::fromStdString(std::string{emailId}));
            query.bindValue(QStringLiteral(":field_name"),
                            QString::fromStdString(std::string{fieldName}));
            if (!query.exec())
            {
                error = makeQueryError(QStringLiteral("Read email addresses"), query);
                return {};
            }

            std::vector<javelin::jmap::domain::EmailAddress> addresses;
            while (query.next())
            {
                addresses.push_back(javelin::jmap::domain::EmailAddress{
                    .name = query.value(0).isNull()
                                ? std::nullopt
                                : std::optional{query.value(0).toString().toStdString()},
                    .email = query.value(1).toString().toStdString(),
                });
            }

            return addresses;
        }

        std::optional<DatabaseError> deleteEmailSummaryChildren(QSqlDatabase& database,
                                                                const std::string_view accountId,
                                                                const std::string_view emailId)
        {
            for (const QString& table :
                 {QStringLiteral("email_addresses"), QStringLiteral("email_keywords"),
                  QStringLiteral("email_mailboxes")})
            {
                QSqlQuery deleteQuery{database};
                deleteQuery.prepare(
                    QStringLiteral(
                        "DELETE FROM %1 WHERE account_id = :account_id AND email_id = :email_id")
                        .arg(table));
                deleteQuery.bindValue(QStringLiteral(":account_id"),
                                      QString::fromStdString(std::string{accountId}));
                deleteQuery.bindValue(QStringLiteral(":email_id"),
                                      QString::fromStdString(std::string{emailId}));
                if (!deleteQuery.exec())
                {
                    return makeQueryError(QStringLiteral("Delete email child rows"), deleteQuery);
                }
            }

            return std::nullopt;
        }

        std::optional<DatabaseError> deleteEmailContent(QSqlDatabase& database,
                                                        const std::string_view accountId,
                                                        const std::string_view emailId)
        {
            for (const QString& table :
                 {QStringLiteral("inline_part_payloads"), QStringLiteral("email_body_values"),
                  QStringLiteral("email_parts")})
            {
                QSqlQuery deleteQuery{database};
                deleteQuery.prepare(
                    QStringLiteral(
                        "DELETE FROM %1 WHERE account_id = :account_id AND email_id = :email_id")
                        .arg(table));
                deleteQuery.bindValue(QStringLiteral(":account_id"),
                                      QString::fromStdString(std::string{accountId}));
                deleteQuery.bindValue(QStringLiteral(":email_id"),
                                      QString::fromStdString(std::string{emailId}));
                if (!deleteQuery.exec())
                {
                    return makeQueryError(QStringLiteral("Delete email content rows"), deleteQuery);
                }
            }

            return std::nullopt;
        }

    } // namespace

    EmailRepository::EmailRepository(DatabaseConnection& connection) : m_connection(connection)
    {
    }

    std::optional<DatabaseError>
    EmailRepository::replaceAll(const std::string_view accountId,
                                const std::vector<javelin::jmap::domain::Email>& emails)
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
                .message = QStringLiteral("Begin email replacement transaction: ") +
                           database.lastError().text(),
            };
        }

        QSqlQuery existingIdsQuery{database};
        existingIdsQuery.prepare(
            QStringLiteral("SELECT email_id FROM emails WHERE account_id = :account_id"));
        existingIdsQuery.bindValue(QStringLiteral(":account_id"),
                                   QString::fromStdString(std::string{accountId}));
        if (!existingIdsQuery.exec())
        {
            database.rollback();
            return makeQueryError(QStringLiteral("Read cached email ids"), existingIdsQuery);
        }

        std::vector<std::string> existingIds;
        while (existingIdsQuery.next())
        {
            existingIds.push_back(existingIdsQuery.value(0).toString().toStdString());
        }

        std::unordered_set<std::string> incomingIds;
        incomingIds.reserve(emails.size());
        for (const auto& email : emails)
        {
            incomingIds.insert(email.id);
        }

        for (const auto& existingId : existingIds)
        {
            if (incomingIds.contains(existingId))
            {
                continue;
            }

            if (const auto error = deleteEmailContent(database, accountId, existingId))
            {
                database.rollback();
                return error;
            }
        }

        for (const QString& table :
             {QStringLiteral("email_addresses"), QStringLiteral("email_keywords"),
              QStringLiteral("email_mailboxes"), QStringLiteral("emails")})
        {
            QSqlQuery deleteQuery{database};
            deleteQuery.prepare(
                QStringLiteral("DELETE FROM %1 WHERE account_id = :account_id").arg(table));
            deleteQuery.bindValue(QStringLiteral(":account_id"),
                                  QString::fromStdString(std::string{accountId}));
            if (!deleteQuery.exec())
            {
                database.rollback();
                return makeQueryError(QStringLiteral("Delete cached email rows"), deleteQuery);
            }
        }

        QSqlQuery emailQuery{database};
        emailQuery.prepare(QStringLiteral(
            "INSERT INTO emails ("
            "account_id, email_id, thread_id, blob_id, received_at, sent_at, message_id_json, "
            "in_reply_to_json, references_json, subject, preview, mailbox_ids_json, "
            "keywords_json, has_attachment, size, state"
            ") VALUES ("
            ":account_id, :email_id, :thread_id, :blob_id, :received_at, :sent_at, "
            ":message_id_json, :in_reply_to_json, :references_json, :subject, :preview, "
            ":mailbox_ids_json, :keywords_json, :has_attachment, :size, :state)"));

        QSqlQuery mailboxQuery{database};
        mailboxQuery.prepare(
            QStringLiteral("INSERT INTO email_mailboxes (account_id, email_id, mailbox_id) "
                           "VALUES (:account_id, :email_id, :mailbox_id)"));

        QSqlQuery keywordQuery{database};
        keywordQuery.prepare(
            QStringLiteral("INSERT INTO email_keywords (account_id, email_id, keyword) "
                           "VALUES (:account_id, :email_id, :keyword)"));

        for (const auto& email : emails)
        {
            emailQuery.bindValue(QStringLiteral(":account_id"),
                                 QString::fromStdString(std::string{accountId}));
            emailQuery.bindValue(QStringLiteral(":email_id"), QString::fromStdString(email.id));
            emailQuery.bindValue(QStringLiteral(":thread_id"),
                                 QString::fromStdString(email.threadId));
            emailQuery.bindValue(QStringLiteral(":blob_id"), QString::fromStdString(email.blobId));
            emailQuery.bindValue(QStringLiteral(":received_at"),
                                 QString::fromStdString(email.receivedAt));
            emailQuery.bindValue(QStringLiteral(":sent_at"),
                                 email.sentAt.has_value()
                                     ? QVariant{QString::fromStdString(*email.sentAt)}
                                     : QVariant{});
            emailQuery.bindValue(QStringLiteral(":message_id_json"),
                                 serializeStringList(email.messageId));
            emailQuery.bindValue(QStringLiteral(":in_reply_to_json"),
                                 serializeStringList(email.inReplyTo));
            emailQuery.bindValue(QStringLiteral(":references_json"),
                                 serializeStringList(email.references));
            emailQuery.bindValue(QStringLiteral(":subject"),
                                 email.subject.has_value()
                                     ? QVariant{QString::fromStdString(*email.subject)}
                                     : QVariant{});
            emailQuery.bindValue(QStringLiteral(":preview"),
                                 email.preview.has_value()
                                     ? QVariant{QString::fromStdString(*email.preview)}
                                     : QVariant{});
            emailQuery.bindValue(QStringLiteral(":mailbox_ids_json"), QStringLiteral("[]"));
            emailQuery.bindValue(QStringLiteral(":keywords_json"), QStringLiteral("{}"));
            emailQuery.bindValue(QStringLiteral(":has_attachment"), email.hasAttachment ? 1 : 0);
            emailQuery.bindValue(QStringLiteral(":size"), static_cast<qulonglong>(email.size));
            emailQuery.bindValue(QStringLiteral(":state"), QVariant{});
            if (!emailQuery.exec())
            {
                database.rollback();
                return makeQueryError(QStringLiteral("Insert email"), emailQuery);
            }

            for (const auto& mailboxId : email.mailboxIds)
            {
                mailboxQuery.bindValue(QStringLiteral(":account_id"),
                                       QString::fromStdString(std::string{accountId}));
                mailboxQuery.bindValue(QStringLiteral(":email_id"),
                                       QString::fromStdString(email.id));
                mailboxQuery.bindValue(QStringLiteral(":mailbox_id"),
                                       QString::fromStdString(mailboxId));
                if (!mailboxQuery.exec())
                {
                    database.rollback();
                    return makeQueryError(QStringLiteral("Insert email mailbox"), mailboxQuery);
                }
            }

            for (const auto& keyword : email.keywords)
            {
                keywordQuery.bindValue(QStringLiteral(":account_id"),
                                       QString::fromStdString(std::string{accountId}));
                keywordQuery.bindValue(QStringLiteral(":email_id"),
                                       QString::fromStdString(email.id));
                keywordQuery.bindValue(QStringLiteral(":keyword"), QString::fromStdString(keyword));
                if (!keywordQuery.exec())
                {
                    database.rollback();
                    return makeQueryError(QStringLiteral("Insert email keyword"), keywordQuery);
                }
            }

            const std::array addressFields{
                std::pair{"from", &email.from},       std::pair{"to", &email.to},
                std::pair{"cc", &email.cc},           std::pair{"bcc", &email.bcc},
                std::pair{"replyTo", &email.replyTo},
            };
            for (const auto& [fieldName, addresses] : addressFields)
            {
                if (const auto error =
                        insertAddresses(database, accountId, email.id, fieldName, *addresses))
                {
                    database.rollback();
                    return error;
                }
            }
        }

        if (!database.commit())
        {
            database.rollback();
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Commit email replacement transaction: ") +
                           database.lastError().text(),
            };
        }

        return std::nullopt;
    }

    std::optional<DatabaseError>
    EmailRepository::upsertMany(const std::string_view accountId,
                                const std::vector<javelin::jmap::domain::Email>& emails)
    {
        if (const auto error = m_connection.validate())
        {
            return error;
        }

        if (emails.empty())
        {
            return std::nullopt;
        }

        QSqlDatabase& database = m_connection.database();
        if (!database.transaction())
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Begin email upsert transaction: ") +
                           database.lastError().text(),
            };
        }

        QSqlQuery emailQuery{database};
        emailQuery.prepare(QStringLiteral(
            "INSERT INTO emails ("
            "account_id, email_id, thread_id, blob_id, received_at, sent_at, message_id_json, "
            "in_reply_to_json, references_json, subject, preview, mailbox_ids_json, "
            "keywords_json, has_attachment, size, state"
            ") VALUES ("
            ":account_id, :email_id, :thread_id, :blob_id, :received_at, :sent_at, "
            ":message_id_json, :in_reply_to_json, :references_json, :subject, :preview, "
            ":mailbox_ids_json, :keywords_json, :has_attachment, :size, :state"
            ") ON CONFLICT(account_id, email_id) DO UPDATE SET "
            "thread_id = excluded.thread_id, "
            "blob_id = excluded.blob_id, "
            "received_at = excluded.received_at, "
            "sent_at = excluded.sent_at, "
            "message_id_json = excluded.message_id_json, "
            "in_reply_to_json = excluded.in_reply_to_json, "
            "references_json = excluded.references_json, "
            "subject = excluded.subject, "
            "preview = excluded.preview, "
            "mailbox_ids_json = excluded.mailbox_ids_json, "
            "keywords_json = excluded.keywords_json, "
            "has_attachment = excluded.has_attachment, "
            "size = excluded.size, "
            "state = excluded.state"));

        QSqlQuery mailboxQuery{database};
        mailboxQuery.prepare(
            QStringLiteral("INSERT INTO email_mailboxes (account_id, email_id, mailbox_id) "
                           "VALUES (:account_id, :email_id, :mailbox_id)"));

        QSqlQuery keywordQuery{database};
        keywordQuery.prepare(
            QStringLiteral("INSERT INTO email_keywords (account_id, email_id, keyword) "
                           "VALUES (:account_id, :email_id, :keyword)"));

        for (const auto& email : emails)
        {
            if (const auto error = deleteEmailSummaryChildren(database, accountId, email.id))
            {
                database.rollback();
                return error;
            }

            emailQuery.bindValue(QStringLiteral(":account_id"),
                                 QString::fromStdString(std::string{accountId}));
            emailQuery.bindValue(QStringLiteral(":email_id"), QString::fromStdString(email.id));
            emailQuery.bindValue(QStringLiteral(":thread_id"),
                                 QString::fromStdString(email.threadId));
            emailQuery.bindValue(QStringLiteral(":blob_id"), QString::fromStdString(email.blobId));
            emailQuery.bindValue(QStringLiteral(":received_at"),
                                 QString::fromStdString(email.receivedAt));
            emailQuery.bindValue(QStringLiteral(":sent_at"),
                                 email.sentAt.has_value()
                                     ? QVariant{QString::fromStdString(*email.sentAt)}
                                     : QVariant{});
            emailQuery.bindValue(QStringLiteral(":message_id_json"),
                                 serializeStringList(email.messageId));
            emailQuery.bindValue(QStringLiteral(":in_reply_to_json"),
                                 serializeStringList(email.inReplyTo));
            emailQuery.bindValue(QStringLiteral(":references_json"),
                                 serializeStringList(email.references));
            emailQuery.bindValue(QStringLiteral(":subject"),
                                 email.subject.has_value()
                                     ? QVariant{QString::fromStdString(*email.subject)}
                                     : QVariant{});
            emailQuery.bindValue(QStringLiteral(":preview"),
                                 email.preview.has_value()
                                     ? QVariant{QString::fromStdString(*email.preview)}
                                     : QVariant{});
            emailQuery.bindValue(QStringLiteral(":mailbox_ids_json"), QStringLiteral("[]"));
            emailQuery.bindValue(QStringLiteral(":keywords_json"), QStringLiteral("{}"));
            emailQuery.bindValue(QStringLiteral(":has_attachment"), email.hasAttachment ? 1 : 0);
            emailQuery.bindValue(QStringLiteral(":size"), static_cast<qulonglong>(email.size));
            emailQuery.bindValue(QStringLiteral(":state"), QVariant{});
            if (!emailQuery.exec())
            {
                database.rollback();
                return makeQueryError(QStringLiteral("Upsert email"), emailQuery);
            }

            for (const auto& mailboxId : email.mailboxIds)
            {
                mailboxQuery.bindValue(QStringLiteral(":account_id"),
                                       QString::fromStdString(std::string{accountId}));
                mailboxQuery.bindValue(QStringLiteral(":email_id"),
                                       QString::fromStdString(email.id));
                mailboxQuery.bindValue(QStringLiteral(":mailbox_id"),
                                       QString::fromStdString(mailboxId));
                if (!mailboxQuery.exec())
                {
                    database.rollback();
                    return makeQueryError(QStringLiteral("Insert email mailbox"), mailboxQuery);
                }
            }

            for (const auto& keyword : email.keywords)
            {
                keywordQuery.bindValue(QStringLiteral(":account_id"),
                                       QString::fromStdString(std::string{accountId}));
                keywordQuery.bindValue(QStringLiteral(":email_id"),
                                       QString::fromStdString(email.id));
                keywordQuery.bindValue(QStringLiteral(":keyword"), QString::fromStdString(keyword));
                if (!keywordQuery.exec())
                {
                    database.rollback();
                    return makeQueryError(QStringLiteral("Insert email keyword"), keywordQuery);
                }
            }

            const std::array addressFields{
                std::pair{"from", &email.from},       std::pair{"to", &email.to},
                std::pair{"cc", &email.cc},           std::pair{"bcc", &email.bcc},
                std::pair{"replyTo", &email.replyTo},
            };
            for (const auto& [fieldName, addresses] : addressFields)
            {
                if (const auto error =
                        insertAddresses(database, accountId, email.id, fieldName, *addresses))
                {
                    database.rollback();
                    return error;
                }
            }
        }

        if (!database.commit())
        {
            database.rollback();
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Commit email upsert transaction: ") +
                           database.lastError().text(),
            };
        }

        return std::nullopt;
    }

    std::optional<DatabaseError>
    EmailRepository::removeMany(const std::string_view accountId,
                                const std::span<const std::string> emailIds)
    {
        if (const auto error = m_connection.validate())
        {
            return error;
        }

        if (emailIds.empty())
        {
            return std::nullopt;
        }

        QSqlDatabase& database = m_connection.database();
        if (!database.transaction())
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Begin email delete transaction: ") +
                           database.lastError().text(),
            };
        }

        QSqlQuery deleteEmailQuery{database};
        deleteEmailQuery.prepare(QStringLiteral(
            "DELETE FROM emails WHERE account_id = :account_id AND email_id = :email_id"));
        for (const auto& emailId : emailIds)
        {
            if (const auto error = deleteEmailSummaryChildren(database, accountId, emailId))
            {
                database.rollback();
                return error;
            }

            if (const auto error = deleteEmailContent(database, accountId, emailId))
            {
                database.rollback();
                return error;
            }

            deleteEmailQuery.bindValue(QStringLiteral(":account_id"),
                                       QString::fromStdString(std::string{accountId}));
            deleteEmailQuery.bindValue(QStringLiteral(":email_id"),
                                       QString::fromStdString(emailId));
            if (!deleteEmailQuery.exec())
            {
                database.rollback();
                return makeQueryError(QStringLiteral("Delete email"), deleteEmailQuery);
            }
        }

        if (!database.commit())
        {
            database.rollback();
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Commit email delete transaction: ") +
                           database.lastError().text(),
            };
        }

        return std::nullopt;
    }

    std::variant<std::optional<javelin::jmap::domain::Email>, DatabaseError>
    EmailRepository::find(const std::string_view accountId, const std::string_view emailId) const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        QSqlDatabase& database = m_connection.database();
        QSqlQuery emailQuery{database};
        emailQuery.prepare(
            QStringLiteral("SELECT blob_id, thread_id, size, received_at, sent_at, "
                           "message_id_json, in_reply_to_json, references_json, "
                           "has_attachment, subject, preview "
                           "FROM emails WHERE account_id = :account_id AND email_id = :email_id"));
        emailQuery.bindValue(QStringLiteral(":account_id"),
                             QString::fromStdString(std::string{accountId}));
        emailQuery.bindValue(QStringLiteral(":email_id"),
                             QString::fromStdString(std::string{emailId}));
        if (!emailQuery.exec())
        {
            return makeQueryError(QStringLiteral("Read email"), emailQuery);
        }

        if (!emailQuery.next())
        {
            return std::optional<javelin::jmap::domain::Email>{std::nullopt};
        }

        QSqlQuery mailboxQuery{database};
        mailboxQuery.prepare(QStringLiteral(
            "SELECT mailbox_id FROM email_mailboxes "
            "WHERE account_id = :account_id AND email_id = :email_id ORDER BY mailbox_id"));
        mailboxQuery.bindValue(QStringLiteral(":account_id"),
                               QString::fromStdString(std::string{accountId}));
        mailboxQuery.bindValue(QStringLiteral(":email_id"),
                               QString::fromStdString(std::string{emailId}));
        if (!mailboxQuery.exec())
        {
            return makeQueryError(QStringLiteral("Read email mailboxes"), mailboxQuery);
        }

        std::vector<std::string> mailboxIds;
        while (mailboxQuery.next())
        {
            mailboxIds.push_back(mailboxQuery.value(0).toString().toStdString());
        }

        QSqlQuery keywordQuery{database};
        keywordQuery.prepare(QStringLiteral(
            "SELECT keyword FROM email_keywords "
            "WHERE account_id = :account_id AND email_id = :email_id ORDER BY keyword"));
        keywordQuery.bindValue(QStringLiteral(":account_id"),
                               QString::fromStdString(std::string{accountId}));
        keywordQuery.bindValue(QStringLiteral(":email_id"),
                               QString::fromStdString(std::string{emailId}));
        if (!keywordQuery.exec())
        {
            return makeQueryError(QStringLiteral("Read email keywords"), keywordQuery);
        }

        std::vector<std::string> keywords;
        while (keywordQuery.next())
        {
            keywords.push_back(keywordQuery.value(0).toString().toStdString());
        }

        std::optional<DatabaseError> addressError;
        auto from = loadAddresses(database, accountId, emailId, "from", addressError);
        if (addressError.has_value())
        {
            return *addressError;
        }
        auto to = loadAddresses(database, accountId, emailId, "to", addressError);
        if (addressError.has_value())
        {
            return *addressError;
        }
        auto cc = loadAddresses(database, accountId, emailId, "cc", addressError);
        if (addressError.has_value())
        {
            return *addressError;
        }
        auto bcc = loadAddresses(database, accountId, emailId, "bcc", addressError);
        if (addressError.has_value())
        {
            return *addressError;
        }
        auto replyTo = loadAddresses(database, accountId, emailId, "replyTo", addressError);
        if (addressError.has_value())
        {
            return *addressError;
        }

        return std::optional<javelin::jmap::domain::Email>{javelin::jmap::domain::Email{
            .id = std::string{emailId},
            .blobId = emailQuery.value(0).toString().toStdString(),
            .threadId = emailQuery.value(1).toString().toStdString(),
            .mailboxIds = std::move(mailboxIds),
            .keywords = std::move(keywords),
            .size = emailQuery.value(2).toULongLong(),
            .receivedAt = emailQuery.value(3).toString().toStdString(),
            .sentAt = emailQuery.value(4).isNull()
                          ? std::nullopt
                          : std::optional{emailQuery.value(4).toString().toStdString()},
            .messageId = deserializeStringList(emailQuery.value(5).toString()),
            .inReplyTo = deserializeStringList(emailQuery.value(6).toString()),
            .references = deserializeStringList(emailQuery.value(7).toString()),
            .hasAttachment = emailQuery.value(8).toInt() != 0,
            .subject = emailQuery.value(9).isNull()
                           ? std::nullopt
                           : std::optional{emailQuery.value(9).toString().toStdString()},
            .from = std::move(from),
            .to = std::move(to),
            .cc = std::move(cc),
            .bcc = std::move(bcc),
            .replyTo = std::move(replyTo),
            .preview = emailQuery.value(10).isNull()
                           ? std::nullopt
                           : std::optional{emailQuery.value(10).toString().toStdString()},
        }};
    }

    std::variant<std::vector<std::string>, DatabaseError>
    EmailRepository::existingIds(const std::string_view accountId,
                                 const std::span<const std::string> emailIds) const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        std::vector<std::string> existing;
        existing.reserve(emailIds.size());

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT 1 FROM emails WHERE account_id = :account_id AND email_id = :email_id"));

        for (const auto& emailId : emailIds)
        {
            query.bindValue(QStringLiteral(":account_id"),
                            QString::fromStdString(std::string{accountId}));
            query.bindValue(QStringLiteral(":email_id"), QString::fromStdString(emailId));
            if (!query.exec())
            {
                return makeQueryError(QStringLiteral("Read existing email ids"), query);
            }

            if (query.next())
            {
                existing.push_back(emailId);
            }

            query.finish();
        }

        return existing;
    }

    std::variant<std::vector<std::string>, DatabaseError>
    EmailRepository::listMailboxEmailIds(const std::string_view accountId,
                                         const std::string_view mailboxId) const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT em.email_id "
            "FROM email_mailboxes em "
            "WHERE em.account_id = :account_id AND em.mailbox_id = :mailbox_id "
            "ORDER BY em.email_id"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":mailbox_id"),
                        QString::fromStdString(std::string{mailboxId}));
        if (!query.exec())
        {
            return makeQueryError(QStringLiteral("Read mailbox email ids"), query);
        }

        std::vector<std::string> emailIds;
        while (query.next())
        {
            emailIds.push_back(query.value(0).toString().toStdString());
        }

        return emailIds;
    }

} // namespace javelin::jmap::cache
