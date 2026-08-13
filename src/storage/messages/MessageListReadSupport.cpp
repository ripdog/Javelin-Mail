#include "jmap/cache/MessageListReadSupport.h"

#include <glaze/glaze.hpp>

#include <QSqlError>
#include <QSqlQuery>

#include <unordered_map>

namespace javelin::jmap::cache::detail
{
    namespace
    {
        [[nodiscard]] DatabaseError makeQueryError(const QString& operation, const QSqlQuery& query)
        {
            return databaseError(operation, query.lastError());
        }

        [[nodiscard]] std::optional<DatabaseError>
        attachMessageBodyPreviews(const DatabaseReadView& connection,
                                  const std::string_view accountId,
                                  std::vector<MessageListItem>& items)
        {
            if (items.empty())
                return std::nullopt;

            std::vector<std::string> emailIds;
            emailIds.reserve(items.size());
            std::unordered_map<std::string, MessageListItem*> itemsById;
            itemsById.reserve(items.size());
            for (auto& item : items)
            {
                item.bodyPreview.reset();
                emailIds.push_back(item.emailId);
                itemsById.emplace(item.emailId, &item);
            }

            std::string emailIdsJson;
            if (const auto error = glz::write_json(emailIds, emailIdsJson))
            {
                Q_UNUSED(error);
                return DatabaseError{
                    .code = DatabaseErrorCode::QueryFailed,
                    .message =
                        QStringLiteral("Serialize message ids for body preview lookup failed."),
                };
            }

            QSqlQuery query{connection.database()};
            query.prepare(QStringLiteral(
                "WITH requested AS MATERIALIZED (SELECT value AS email_id FROM "
                "json_each(:email_ids)) "
                "SELECT r.email_id,r.body_preview FROM requested q "
                "JOIN mail_vault_email_refs r ON r.account_id=:account_id AND "
                "r.email_id=q.email_id "
                "WHERE r.indexed_hash=r.content_hash AND r.body_preview IS NOT NULL"));
            query.bindValue(QStringLiteral(":account_id"),
                            QString::fromStdString(std::string{accountId}));
            query.bindValue(QStringLiteral(":email_ids"), QString::fromStdString(emailIdsJson));
            if (!query.exec())
                return makeQueryError(QStringLiteral("Load message body previews"), query);

            while (query.next())
            {
                const auto found = itemsById.find(query.value(0).toString().toStdString());
                if (found != itemsById.end())
                    found->second->bodyPreview = query.value(1).toString().toStdString();
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<DatabaseError>
        attachMessageTags(const DatabaseReadView& connection, const std::string_view accountId,
                          std::vector<MessageListItem>& items)
        {
            if (items.empty())
                return std::nullopt;

            std::vector<std::string> emailIds;
            emailIds.reserve(items.size());
            std::unordered_map<std::string, MessageListItem*> itemsById;
            itemsById.reserve(items.size());
            for (auto& item : items)
            {
                item.tags.clear();
                emailIds.push_back(item.emailId);
                itemsById.emplace(item.emailId, &item);
            }

            std::string emailIdsJson;
            if (const auto error = glz::write_json(emailIds, emailIdsJson))
            {
                Q_UNUSED(error);
                return DatabaseError{
                    .code = DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("Serialize message ids for tag lookup failed."),
                };
            }

            QSqlQuery query{connection.database()};
            query.prepare(QStringLiteral(
                "WITH requested AS MATERIALIZED ("
                " SELECT value AS email_id,CAST(key AS INTEGER) AS sort_index "
                " FROM json_each(:email_ids)) "
                "SELECT r.email_id,d.keyword,d.display_name,d.color FROM requested r "
                "JOIN email_keywords k ON k.account_id=:account_id AND k.email_id=r.email_id "
                "JOIN mail_tag_definitions d ON d.account_id=k.account_id "
                " AND d.keyword=k.keyword COLLATE NOCASE "
                "WHERE NOT EXISTS(SELECT 1 FROM background_jobs j "
                " WHERE j.account_id=d.account_id AND j.kind='tag_deletion' "
                " AND j.status NOT IN ('failed','complete') "
                " AND json_extract(j.checkpoint_json,'$.keyword')=d.keyword COLLATE NOCASE) "
                "ORDER BY r.sort_index,d.sort_order,d.display_name COLLATE NOCASE,d.keyword"));
            query.bindValue(QStringLiteral(":account_id"),
                            QString::fromStdString(std::string{accountId}));
            query.bindValue(QStringLiteral(":email_ids"), QString::fromStdString(emailIdsJson));
            if (!query.exec())
                return makeQueryError(QStringLiteral("Load message tags"), query);

            while (query.next())
            {
                const auto found = itemsById.find(query.value(0).toString().toStdString());
                if (found == itemsById.end())
                    continue;
                found->second->tags.push_back(MessageListTag{
                    .keyword = query.value(1).toString().toStdString(),
                    .displayName = query.value(2).toString(),
                    .color = query.value(3).toString(),
                });
            }
            return std::nullopt;
        }
    } // namespace

    std::optional<DatabaseError> attachMessageListMetadata(const DatabaseReadView& connection,
                                                           const std::string_view accountId,
                                                           std::vector<MessageListItem>& items)
    {
        if (const auto error = attachMessageBodyPreviews(connection, accountId, items))
            return error;
        return attachMessageTags(connection, accountId, items);
    }

} // namespace javelin::jmap::cache::detail
