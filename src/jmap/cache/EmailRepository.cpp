#include "jmap/cache/EmailRepository.h"

#include <QSqlError>
#include <QSqlQuery>

#include <array>

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

        std::optional<DatabaseError>
        insertAddresses(QSqlDatabase& database, std::string_view accountId,
                        std::string_view emailId, std::string_view fieldName,
                        const std::vector<javelin::jmap::domain::EmailAddress>& addresses)
        {
            QSqlQuery query{database};
            query.prepare(
                "INSERT INTO email_addresses ("
                "account_id, email_id, field_name, position, display_name, address"
                ") VALUES ("
                ":account_id, :email_id, :field_name, :position, :display_name, :address)");

            int position = 0;
            for (const auto& address : addresses)
            {
                query.bindValue(":account_id", QString::fromStdString(std::string{accountId}));
                query.bindValue(":email_id", QString::fromStdString(std::string{emailId}));
                query.bindValue(":field_name", QString::fromStdString(std::string{fieldName}));
                query.bindValue(":position", position);
                query.bindValue(":display_name",
                                address.name.has_value()
                                    ? QVariant{QString::fromStdString(*address.name)}
                                    : QVariant{});
                query.bindValue(":address", QString::fromStdString(address.email));
                if (!query.exec())
                {
                    return makeQueryError("Insert email address", query);
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
            query.prepare("SELECT display_name, address "
                          "FROM email_addresses "
                          "WHERE account_id = :account_id AND email_id = :email_id AND field_name "
                          "= :field_name "
                          "ORDER BY position");
            query.bindValue(":account_id", QString::fromStdString(std::string{accountId}));
            query.bindValue(":email_id", QString::fromStdString(std::string{emailId}));
            query.bindValue(":field_name", QString::fromStdString(std::string{fieldName}));
            if (!query.exec())
            {
                error = makeQueryError("Read email addresses", query);
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
                .message = "Begin email replacement transaction: " + database.lastError().text(),
            };
        }

        for (const QString& table :
             {QStringLiteral("email_addresses"), QStringLiteral("email_keywords"),
              QStringLiteral("email_mailboxes"), QStringLiteral("emails")})
        {
            QSqlQuery deleteQuery{database};
            deleteQuery.prepare(
                QStringLiteral("DELETE FROM %1 WHERE account_id = :account_id").arg(table));
            deleteQuery.bindValue(":account_id", QString::fromStdString(std::string{accountId}));
            if (!deleteQuery.exec())
            {
                database.rollback();
                return makeQueryError("Delete cached email rows", deleteQuery);
            }
        }

        QSqlQuery emailQuery{database};
        emailQuery.prepare(
            "INSERT INTO emails ("
            "account_id, email_id, thread_id, blob_id, received_at, sent_at, subject, preview, "
            "mailbox_ids_json, keywords_json, has_attachment, size, state"
            ") VALUES ("
            ":account_id, :email_id, :thread_id, :blob_id, :received_at, :sent_at, :subject, "
            ":preview, "
            ":mailbox_ids_json, :keywords_json, :has_attachment, :size, :state)");

        QSqlQuery mailboxQuery{database};
        mailboxQuery.prepare("INSERT INTO email_mailboxes (account_id, email_id, mailbox_id) "
                             "VALUES (:account_id, :email_id, :mailbox_id)");

        QSqlQuery keywordQuery{database};
        keywordQuery.prepare("INSERT INTO email_keywords (account_id, email_id, keyword) "
                             "VALUES (:account_id, :email_id, :keyword)");

        for (const auto& email : emails)
        {
            emailQuery.bindValue(":account_id", QString::fromStdString(std::string{accountId}));
            emailQuery.bindValue(":email_id", QString::fromStdString(email.id));
            emailQuery.bindValue(":thread_id", QString::fromStdString(email.threadId));
            emailQuery.bindValue(":blob_id", QString::fromStdString(email.blobId));
            emailQuery.bindValue(":received_at", QString::fromStdString(email.receivedAt));
            emailQuery.bindValue(":sent_at", email.sentAt.has_value()
                                                 ? QVariant{QString::fromStdString(*email.sentAt)}
                                                 : QVariant{});
            emailQuery.bindValue(":subject", email.subject.has_value()
                                                 ? QVariant{QString::fromStdString(*email.subject)}
                                                 : QVariant{});
            emailQuery.bindValue(":preview", email.preview.has_value()
                                                 ? QVariant{QString::fromStdString(*email.preview)}
                                                 : QVariant{});
            emailQuery.bindValue(":mailbox_ids_json", QStringLiteral("[]"));
            emailQuery.bindValue(":keywords_json", QStringLiteral("{}"));
            emailQuery.bindValue(":has_attachment", email.hasAttachment ? 1 : 0);
            emailQuery.bindValue(":size", static_cast<qulonglong>(email.size));
            emailQuery.bindValue(":state", QVariant{});
            if (!emailQuery.exec())
            {
                database.rollback();
                return makeQueryError("Insert email", emailQuery);
            }

            for (const auto& mailboxId : email.mailboxIds)
            {
                mailboxQuery.bindValue(":account_id",
                                       QString::fromStdString(std::string{accountId}));
                mailboxQuery.bindValue(":email_id", QString::fromStdString(email.id));
                mailboxQuery.bindValue(":mailbox_id", QString::fromStdString(mailboxId));
                if (!mailboxQuery.exec())
                {
                    database.rollback();
                    return makeQueryError("Insert email mailbox", mailboxQuery);
                }
            }

            for (const auto& keyword : email.keywords)
            {
                keywordQuery.bindValue(":account_id",
                                       QString::fromStdString(std::string{accountId}));
                keywordQuery.bindValue(":email_id", QString::fromStdString(email.id));
                keywordQuery.bindValue(":keyword", QString::fromStdString(keyword));
                if (!keywordQuery.exec())
                {
                    database.rollback();
                    return makeQueryError("Insert email keyword", keywordQuery);
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
                .message = "Commit email replacement transaction: " + database.lastError().text(),
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
        emailQuery.prepare("SELECT blob_id, thread_id, size, received_at, sent_at, has_attachment, "
                           "subject, preview "
                           "FROM emails WHERE account_id = :account_id AND email_id = :email_id");
        emailQuery.bindValue(":account_id", QString::fromStdString(std::string{accountId}));
        emailQuery.bindValue(":email_id", QString::fromStdString(std::string{emailId}));
        if (!emailQuery.exec())
        {
            return makeQueryError("Read email", emailQuery);
        }

        if (!emailQuery.next())
        {
            return std::optional<javelin::jmap::domain::Email>{std::nullopt};
        }

        QSqlQuery mailboxQuery{database};
        mailboxQuery.prepare(
            "SELECT mailbox_id FROM email_mailboxes "
            "WHERE account_id = :account_id AND email_id = :email_id ORDER BY mailbox_id");
        mailboxQuery.bindValue(":account_id", QString::fromStdString(std::string{accountId}));
        mailboxQuery.bindValue(":email_id", QString::fromStdString(std::string{emailId}));
        if (!mailboxQuery.exec())
        {
            return makeQueryError("Read email mailboxes", mailboxQuery);
        }

        std::vector<std::string> mailboxIds;
        while (mailboxQuery.next())
        {
            mailboxIds.push_back(mailboxQuery.value(0).toString().toStdString());
        }

        QSqlQuery keywordQuery{database};
        keywordQuery.prepare(
            "SELECT keyword FROM email_keywords "
            "WHERE account_id = :account_id AND email_id = :email_id ORDER BY keyword");
        keywordQuery.bindValue(":account_id", QString::fromStdString(std::string{accountId}));
        keywordQuery.bindValue(":email_id", QString::fromStdString(std::string{emailId}));
        if (!keywordQuery.exec())
        {
            return makeQueryError("Read email keywords", keywordQuery);
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
            .hasAttachment = emailQuery.value(5).toInt() != 0,
            .subject = emailQuery.value(6).isNull()
                           ? std::nullopt
                           : std::optional{emailQuery.value(6).toString().toStdString()},
            .from = std::move(from),
            .to = std::move(to),
            .cc = std::move(cc),
            .bcc = std::move(bcc),
            .replyTo = std::move(replyTo),
            .preview = emailQuery.value(7).isNull()
                           ? std::nullopt
                           : std::optional{emailQuery.value(7).toString().toStdString()},
        }};
    }

} // namespace javelin::jmap::cache
