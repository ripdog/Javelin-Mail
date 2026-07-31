#include "jmap/cache/EmailReadRepository.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>

namespace javelin::jmap::cache
{
    namespace
    {
        [[nodiscard]] DatabaseError makeQueryError(const QString& operation, const QSqlQuery& query)
        {
            return {.code = DatabaseErrorCode::QueryFailed,
                    .message = operation + QStringLiteral(": ") + query.lastError().text()};
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
                    values.push_back(stringValue.toStdString());
            }
            return values;
        }

        [[nodiscard]] std::vector<javelin::jmap::domain::EmailAddress>
        loadAddresses(const QSqlDatabase& database, std::string_view accountId,
                      std::string_view emailId, std::string_view fieldName,
                      std::optional<DatabaseError>& error)
        {
            QSqlQuery query{database};
            query.prepare(QStringLiteral(
                "SELECT display_name,address FROM email_addresses WHERE account_id=:account_id "
                "AND email_id=:email_id AND field_name=:field_name ORDER BY position"));
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
                addresses.push_back({
                    .name = query.value(0).isNull()
                                ? std::nullopt
                                : std::optional{query.value(0).toString().toStdString()},
                    .email = query.value(1).toString().toStdString(),
                });
            }
            return addresses;
        }
    } // namespace

    EmailReadRepository::EmailReadRepository(const DatabaseReadView& connection)
        : m_connection(connection)
    {
    }

    std::variant<std::optional<javelin::jmap::domain::Email>, DatabaseError>
    EmailReadRepository::find(const std::string_view accountId,
                              const std::string_view emailId) const
    {
        if (const auto error = m_connection.validate())
            return *error;

        const auto& database = m_connection.database();
        QSqlQuery emailQuery{database};
        emailQuery.prepare(QStringLiteral(
            "SELECT blob_id,thread_id,size,received_at,sent_at,message_id_json,in_reply_to_json,"
            "references_json,has_attachment,subject,preview FROM emails WHERE account_id=:account "
            "AND email_id=:email"));
        emailQuery.bindValue(QStringLiteral(":account"),
                             QString::fromStdString(std::string{accountId}));
        emailQuery.bindValue(QStringLiteral(":email"),
                             QString::fromStdString(std::string{emailId}));
        if (!emailQuery.exec())
            return makeQueryError(QStringLiteral("Read email"), emailQuery);
        if (!emailQuery.next())
            return std::optional<javelin::jmap::domain::Email>{std::nullopt};

        QSqlQuery mailboxQuery{database};
        mailboxQuery.prepare(
            QStringLiteral("SELECT mailbox_id FROM email_mailboxes WHERE account_id=:account AND "
                           "email_id=:email ORDER BY mailbox_id"));
        mailboxQuery.bindValue(QStringLiteral(":account"),
                               QString::fromStdString(std::string{accountId}));
        mailboxQuery.bindValue(QStringLiteral(":email"),
                               QString::fromStdString(std::string{emailId}));
        if (!mailboxQuery.exec())
            return makeQueryError(QStringLiteral("Read email mailboxes"), mailboxQuery);
        std::vector<std::string> mailboxIds;
        while (mailboxQuery.next())
            mailboxIds.push_back(mailboxQuery.value(0).toString().toStdString());

        QSqlQuery keywordQuery{database};
        keywordQuery.prepare(QStringLiteral(
            "SELECT keyword FROM email_keywords WHERE account_id=:account AND email_id=:email "
            "ORDER BY keyword"));
        keywordQuery.bindValue(QStringLiteral(":account"),
                               QString::fromStdString(std::string{accountId}));
        keywordQuery.bindValue(QStringLiteral(":email"),
                               QString::fromStdString(std::string{emailId}));
        if (!keywordQuery.exec())
            return makeQueryError(QStringLiteral("Read email keywords"), keywordQuery);
        std::vector<std::string> keywords;
        while (keywordQuery.next())
            keywords.push_back(keywordQuery.value(0).toString().toStdString());

        std::optional<DatabaseError> addressError;
        auto from = loadAddresses(database, accountId, emailId, "from", addressError);
        if (addressError)
            return *addressError;
        auto to = loadAddresses(database, accountId, emailId, "to", addressError);
        if (addressError)
            return *addressError;
        auto cc = loadAddresses(database, accountId, emailId, "cc", addressError);
        if (addressError)
            return *addressError;
        auto bcc = loadAddresses(database, accountId, emailId, "bcc", addressError);
        if (addressError)
            return *addressError;
        auto replyTo = loadAddresses(database, accountId, emailId, "replyTo", addressError);
        if (addressError)
            return *addressError;

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
} // namespace javelin::jmap::cache
