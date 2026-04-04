#include "jmap/sync/PendingActions.h"

#include <QSqlError>
#include <QSqlQuery>

#include <glaze/glaze.hpp>

#include <algorithm>

namespace
{

    struct RawPendingEmailPatch
    {
        std::string emailId;
        std::vector<std::string> addMailboxIds;
        std::vector<std::string> removeMailboxIds;
        std::vector<std::string> addKeywords;
        std::vector<std::string> removeKeywords;
    };

} // namespace

template <> struct glz::meta<RawPendingEmailPatch>
{
    using T = RawPendingEmailPatch;

    static constexpr auto value = glz::object(
        "emailId", &T::emailId, "addMailboxIds", &T::addMailboxIds, "removeMailboxIds",
        &T::removeMailboxIds, "addKeywords", &T::addKeywords, "removeKeywords", &T::removeKeywords);
};

namespace javelin::jmap::sync
{

    namespace
    {

        [[nodiscard]] javelin::jmap::cache::DatabaseError makeQueryError(const QString& operation,
                                                                         const QSqlQuery& query)
        {
            return javelin::jmap::cache::DatabaseError{
                .code = javelin::jmap::cache::DatabaseErrorCode::QueryFailed,
                .message = operation + ": " + query.lastError().text(),
            };
        }

        [[nodiscard]] std::optional<std::string> serializePayload(const PendingEmailPatch& patch)
        {
            std::string buffer;
            const auto writeError = glz::write_json(
                RawPendingEmailPatch{
                    .emailId = patch.emailId,
                    .addMailboxIds = patch.addMailboxIds,
                    .removeMailboxIds = patch.removeMailboxIds,
                    .addKeywords = patch.addKeywords,
                    .removeKeywords = patch.removeKeywords,
                },
                buffer);
            if (writeError)
            {
                return std::nullopt;
            }

            return buffer;
        }

        [[nodiscard]] std::optional<PendingEmailPatch> parsePayload(const QString& json)
        {
            std::string buffer = json.toStdString();
            RawPendingEmailPatch raw;
            const auto readError =
                glz::read<glz::opts{.error_on_unknown_keys = false}>(raw, buffer);
            if (readError)
            {
                return std::nullopt;
            }

            return PendingEmailPatch{
                .emailId = std::move(raw.emailId),
                .addMailboxIds = std::move(raw.addMailboxIds),
                .removeMailboxIds = std::move(raw.removeMailboxIds),
                .addKeywords = std::move(raw.addKeywords),
                .removeKeywords = std::move(raw.removeKeywords),
            };
        }

        void applyAdd(std::vector<std::string>& values, const std::vector<std::string>& added)
        {
            for (const auto& value : added)
            {
                if (std::find(values.cbegin(), values.cend(), value) == values.cend())
                {
                    values.push_back(value);
                }
            }
        }

        void applyRemove(std::vector<std::string>& values, const std::vector<std::string>& removed)
        {
            values.erase(std::remove_if(values.begin(), values.end(),
                                        [&removed](const std::string& value)
                                        {
                                            return std::find(removed.cbegin(), removed.cend(),
                                                             value) != removed.cend();
                                        }),
                         values.end());
        }

    } // namespace

    PendingActionRepository::PendingActionRepository(
        javelin::jmap::cache::DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    PendingActionRepository::put(const PendingActionRecord& record)
    {
        if (const auto error = m_connection.validate())
        {
            return error;
        }

        const auto payload = serializePayload(record.emailPatch);
        if (!payload.has_value())
        {
            return javelin::jmap::cache::DatabaseError{
                .code = javelin::jmap::cache::DatabaseErrorCode::QueryFailed,
                .message = "Serialize pending action payload failed",
            };
        }

        QSqlQuery query{m_connection.database()};
        query.prepare("INSERT INTO pending_actions ("
                      "pending_action_id, account_id, action_type, status, payload_json"
                      ") VALUES ("
                      ":pending_action_id, :account_id, :action_type, :status, :payload_json"
                      ") ON CONFLICT(pending_action_id) DO UPDATE SET "
                      "account_id = excluded.account_id, "
                      "action_type = excluded.action_type, "
                      "status = excluded.status, "
                      "payload_json = excluded.payload_json, "
                      "updated_at = CURRENT_TIMESTAMP");
        query.bindValue(":pending_action_id", QString::fromStdString(record.pendingActionId));
        query.bindValue(":account_id", QString::fromStdString(record.accountId));
        query.bindValue(":action_type", "email_patch");
        query.bindValue(":status", QString::fromStdString(std::string{toString(record.status)}));
        query.bindValue(":payload_json", QString::fromStdString(*payload));
        if (!query.exec())
        {
            return makeQueryError("Upsert pending action", query);
        }

        return std::nullopt;
    }

    std::variant<std::vector<PendingActionRecord>, javelin::jmap::cache::DatabaseError>
    PendingActionRepository::listForEmail(const std::string_view accountId,
                                          const std::string_view emailId) const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare("SELECT pending_action_id, account_id, status, payload_json "
                      "FROM pending_actions "
                      "WHERE account_id = :account_id AND action_type = 'email_patch' "
                      "ORDER BY created_at, pending_action_id");
        query.bindValue(":account_id", QString::fromStdString(std::string{accountId}));
        if (!query.exec())
        {
            return makeQueryError("Read pending actions", query);
        }

        std::vector<PendingActionRecord> records;
        while (query.next())
        {
            const auto patch = parsePayload(query.value(3).toString());
            if (!patch.has_value() || patch->emailId != emailId)
            {
                continue;
            }

            const auto status =
                pendingActionStatusFromString(query.value(2).toString().toStdString());
            if (!status.has_value())
            {
                continue;
            }

            records.push_back(PendingActionRecord{
                .pendingActionId = query.value(0).toString().toStdString(),
                .accountId = query.value(1).toString().toStdString(),
                .status = *status,
                .emailPatch = *patch,
            });
        }

        return records;
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    PendingActionRepository::remove(const std::string_view pendingActionId)
    {
        if (const auto error = m_connection.validate())
        {
            return error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare("DELETE FROM pending_actions WHERE pending_action_id = :pending_action_id");
        query.bindValue(":pending_action_id", QString::fromStdString(std::string{pendingActionId}));
        if (!query.exec())
        {
            return makeQueryError("Delete pending action", query);
        }

        return std::nullopt;
    }

    std::string_view toString(const PendingActionStatus status)
    {
        switch (status)
        {
        case PendingActionStatus::Pending:
            return "pending";
        case PendingActionStatus::InFlight:
            return "in_flight";
        case PendingActionStatus::Failed:
            return "failed";
        }

        return "pending";
    }

    std::optional<PendingActionStatus> pendingActionStatusFromString(const std::string_view value)
    {
        if (value == "pending")
        {
            return PendingActionStatus::Pending;
        }
        if (value == "in_flight")
        {
            return PendingActionStatus::InFlight;
        }
        if (value == "failed")
        {
            return PendingActionStatus::Failed;
        }

        return std::nullopt;
    }

    javelin::jmap::domain::Email
    mergePendingEmailPatch(const javelin::jmap::domain::Email& email,
                           const std::vector<PendingActionRecord>& pendingActions)
    {
        auto merged = email;
        for (const auto& action : pendingActions)
        {
            if (action.emailPatch.emailId != merged.id)
            {
                continue;
            }

            applyAdd(merged.mailboxIds, action.emailPatch.addMailboxIds);
            applyRemove(merged.mailboxIds, action.emailPatch.removeMailboxIds);
            applyAdd(merged.keywords, action.emailPatch.addKeywords);
            applyRemove(merged.keywords, action.emailPatch.removeKeywords);
        }

        std::sort(merged.mailboxIds.begin(), merged.mailboxIds.end());
        merged.mailboxIds.erase(std::unique(merged.mailboxIds.begin(), merged.mailboxIds.end()),
                                merged.mailboxIds.end());
        std::sort(merged.keywords.begin(), merged.keywords.end());
        merged.keywords.erase(std::unique(merged.keywords.begin(), merged.keywords.end()),
                              merged.keywords.end());
        return merged;
    }

} // namespace javelin::jmap::sync
