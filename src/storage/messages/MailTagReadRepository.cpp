#include "jmap/cache/MailTagReadRepository.h"

#include "jmap/domain/MailKeywords.h"

#include <glaze/glaze.hpp>

#include <QSqlError>
#include <QSqlQuery>

#include <unordered_map>

namespace javelin::jmap::cache
{
    namespace
    {
        [[nodiscard]] DatabaseError makeQueryError(const QString& operation, const QSqlQuery& query)
        {
            return databaseError(operation, query.lastError());
        }
    } // namespace

    MailTagReadRepository::MailTagReadRepository(DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    MailTagReadRepository::MailTagReadRepository(ReadOnlyDatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    MailTagReadRepository::MailTagReadRepository(DatabaseReadView connection)
        : m_connection(connection)
    {
    }

    std::variant<std::vector<std::string>, DatabaseError>
    MailTagReadRepository::listUserKeywords(const std::string_view accountId,
                                            const std::string_view mailboxId) const
    {
        if (const auto error = m_connection.validate())
            return *error;

        QSqlQuery query{m_connection.database()};
        if (mailboxId.empty())
        {
            query.prepare(QStringLiteral(
                "SELECT DISTINCT k.keyword FROM email_keywords k "
                "WHERE k.account_id=:account_id AND NOT EXISTS(SELECT 1 FROM background_jobs j "
                "WHERE j.account_id=k.account_id AND j.kind='tag_deletion' "
                "AND j.status NOT IN ('failed','complete') "
                "AND json_extract(j.checkpoint_json,'$.keyword')=k.keyword COLLATE NOCASE) "
                "ORDER BY k.keyword COLLATE NOCASE"));
        }
        else
        {
            query.prepare(QStringLiteral(
                "SELECT DISTINCT k.keyword FROM email_keywords k "
                "JOIN email_mailboxes em ON em.account_id=k.account_id AND "
                "em.email_id=k.email_id "
                "WHERE k.account_id=:account_id AND em.mailbox_id=:mailbox_id "
                "AND NOT EXISTS(SELECT 1 FROM background_jobs j "
                "WHERE j.account_id=k.account_id AND j.kind='tag_deletion' "
                "AND j.status NOT IN ('failed','complete') "
                "AND json_extract(j.checkpoint_json,'$.keyword')=k.keyword COLLATE NOCASE) "
                "ORDER BY k.keyword COLLATE NOCASE"));
            query.bindValue(QStringLiteral(":mailbox_id"),
                            QString::fromStdString(std::string{mailboxId}));
        }
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        if (!query.exec())
            return makeQueryError(QStringLiteral("List message user keywords"), query);

        std::vector<std::string> keywords;
        while (query.next())
        {
            auto keyword = query.value(0).toString().toStdString();
            if (!javelin::jmap::domain::hasStandardKeywordSemantics(keyword))
                keywords.push_back(std::move(keyword));
        }
        return keywords;
    }

    std::variant<std::vector<std::string>, DatabaseError>
    MailTagReadRepository::listTagKeywords(const std::string_view accountId) const
    {
        if (const auto error = m_connection.validate())
            return *error;

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT d.keyword FROM mail_tag_definitions d "
            "WHERE d.account_id=:account_id AND NOT EXISTS(SELECT 1 FROM background_jobs j "
            "WHERE j.account_id=d.account_id AND j.kind='tag_deletion' "
            "AND j.status NOT IN ('failed','complete') "
            "AND json_extract(j.checkpoint_json,'$.keyword')=d.keyword COLLATE NOCASE) "
            "ORDER BY d.sort_order,d.display_name COLLATE NOCASE,d.keyword"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        if (!query.exec())
            return makeQueryError(QStringLiteral("List mail tag keywords"), query);

        std::vector<std::string> keywords;
        while (query.next())
            keywords.push_back(query.value(0).toString().toStdString());
        return keywords;
    }

    std::variant<std::vector<EmailKeywordMembership>, DatabaseError>
    MailTagReadRepository::listEmailKeywordMemberships(
        const std::string_view accountId, const std::vector<std::string>& emailIds) const
    {
        if (const auto error = m_connection.validate())
            return *error;
        if (emailIds.empty())
            return std::vector<EmailKeywordMembership>{};

        std::string idsJson;
        if (const auto error = glz::write_json(emailIds, idsJson))
        {
            Q_UNUSED(error);
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Serialize email ids for keyword lookup failed."),
            };
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(
            QStringLiteral("WITH selected(email_id) AS (SELECT value FROM json_each(:ids)) "
                           "SELECT s.email_id,k.keyword FROM selected s LEFT JOIN email_keywords k "
                           "ON k.account_id=:account_id AND k.email_id=s.email_id "
                           "ORDER BY s.email_id,k.keyword COLLATE NOCASE"));
        query.bindValue(QStringLiteral(":ids"), QString::fromStdString(idsJson));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        if (!query.exec())
            return makeQueryError(QStringLiteral("List email keyword memberships"), query);

        std::unordered_map<std::string, std::vector<std::string>> byEmail;
        byEmail.reserve(emailIds.size());
        while (query.next())
        {
            auto& keywords = byEmail[query.value(0).toString().toStdString()];
            if (!query.value(1).isNull())
                keywords.push_back(query.value(1).toString().toStdString());
        }

        std::vector<EmailKeywordMembership> memberships;
        memberships.reserve(emailIds.size());
        for (const auto& emailId : emailIds)
        {
            auto found = byEmail.find(emailId);
            memberships.push_back(EmailKeywordMembership{
                .emailId = emailId,
                .keywords =
                    found == byEmail.end() ? std::vector<std::string>{} : std::move(found->second),
            });
        }
        return memberships;
    }

    std::variant<std::vector<TagDefinition>, DatabaseError>
    MailTagReadRepository::listTagDefinitions(const std::string_view accountId) const
    {
        if (const auto error = m_connection.validate())
            return *error;

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT d.keyword,d.display_name,d.color,d.sort_order FROM mail_tag_definitions d "
            "WHERE d.account_id=:account_id AND NOT EXISTS(SELECT 1 FROM background_jobs j "
            "WHERE j.account_id=d.account_id AND j.kind='tag_deletion' "
            "AND j.status NOT IN ('failed','complete') "
            "AND json_extract(j.checkpoint_json,'$.keyword')=d.keyword COLLATE NOCASE) "
            "ORDER BY d.sort_order,d.display_name COLLATE NOCASE,d.keyword"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        if (!query.exec())
            return makeQueryError(QStringLiteral("List mail tag definitions"), query);

        std::vector<TagDefinition> definitions;
        while (query.next())
        {
            definitions.push_back(TagDefinition{
                .accountId = std::string{accountId},
                .keyword = query.value(0).toString().toStdString(),
                .displayName = query.value(1).toString(),
                .color = query.value(2).toString(),
                .sortOrder = query.value(3).toInt(),
            });
        }
        return definitions;
    }

} // namespace javelin::jmap::cache
