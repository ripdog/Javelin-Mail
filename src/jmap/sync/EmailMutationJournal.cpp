#include "jmap/sync/EmailMutationJournal.h"

#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailboxWindowRepository.h"
#include "jmap/cache/SearchWindowRepository.h"

#include <glaze/glaze.hpp>

#include <algorithm>

namespace
{

    struct RawEmailPatchMutation
    {
        std::string emailId;
        std::vector<std::string> addMailboxIds;
        std::vector<std::string> removeMailboxIds;
        std::vector<std::string> addKeywords;
        std::vector<std::string> removeKeywords;
        bool destroy = false;
        std::optional<std::vector<std::string>> baseMailboxIds;
        std::optional<std::vector<std::string>> baseKeywords;
    };

} // namespace

template <> struct glz::meta<RawEmailPatchMutation>
{
    using T = RawEmailPatchMutation;

    static constexpr auto value =
        glz::object("emailId", &T::emailId, "addMailboxIds", &T::addMailboxIds, "removeMailboxIds",
                    &T::removeMailboxIds, "addKeywords", &T::addKeywords, "removeKeywords",
                    &T::removeKeywords, "destroy", &T::destroy, "baseMailboxIds",
                    &T::baseMailboxIds, "baseKeywords", &T::baseKeywords);
};

namespace javelin::jmap::sync
{

    namespace
    {

        [[nodiscard]] std::optional<RawEmailPatchMutation> parsePayload(const std::string_view json)
        {
            RawEmailPatchMutation raw;
            if (glz::read<glz::opts{.error_on_unknown_keys = false}>(raw, json))
            {
                return std::nullopt;
            }
            return raw;
        }

        [[nodiscard]] std::variant<MutationRecord, javelin::jmap::cache::DatabaseError>
        genericRecord(const EmailMutationRecord& record)
        {
            std::string payload;
            if (glz::write_json(
                    RawEmailPatchMutation{
                        .emailId = record.patch.emailId,
                        .addMailboxIds = record.patch.addMailboxIds,
                        .removeMailboxIds = record.patch.removeMailboxIds,
                        .addKeywords = record.patch.addKeywords,
                        .removeKeywords = record.patch.removeKeywords,
                        .destroy = record.patch.destroy,
                        .baseMailboxIds = record.baseMailboxIds,
                        .baseKeywords = record.baseKeywords,
                    },
                    payload))
            {
                return javelin::jmap::cache::DatabaseError{
                    .code = javelin::jmap::cache::DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("Unable to serialize an Email mutation."),
                };
            }
            return MutationRecord{
                .mutationId = record.mutationId,
                .operationGroupId = record.operationGroupId,
                .domain = {.accountId = record.accountId, .dataType = "Email"},
                .objectId = record.patch.emailId,
                .mutationKind = "email_patch",
                .status = record.status,
                .payloadJson = std::move(payload),
                .baseState = record.baseState,
                .acceptedState = record.acceptedState,
                .errorJson = record.errorJson,
            };
        }

        [[nodiscard]] std::variant<std::vector<EmailMutationRecord>,
                                   javelin::jmap::cache::DatabaseError>
        typedRecords(
            std::variant<std::vector<MutationRecord>, javelin::jmap::cache::DatabaseError> result)
        {
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
            {
                return *error;
            }

            std::vector<EmailMutationRecord> typed;
            for (auto& record : std::get<std::vector<MutationRecord>>(result))
            {
                if (record.mutationKind != "email_patch")
                    continue;
                auto raw = parsePayload(record.payloadJson);
                if (!raw.has_value() || raw->emailId != record.objectId)
                {
                    return javelin::jmap::cache::DatabaseError{
                        .code = javelin::jmap::cache::DatabaseErrorCode::QueryFailed,
                        .message = QStringLiteral("Invalid Email mutation journal payload."),
                    };
                }
                typed.push_back(EmailMutationRecord{
                    .mutationId = std::move(record.mutationId),
                    .operationGroupId = std::move(record.operationGroupId),
                    .accountId = std::move(record.domain.accountId),
                    .status = record.status,
                    .patch =
                        {
                            .emailId = std::move(raw->emailId),
                            .addMailboxIds = std::move(raw->addMailboxIds),
                            .removeMailboxIds = std::move(raw->removeMailboxIds),
                            .addKeywords = std::move(raw->addKeywords),
                            .removeKeywords = std::move(raw->removeKeywords),
                            .destroy = raw->destroy,
                        },
                    .baseMailboxIds = std::move(raw->baseMailboxIds),
                    .baseKeywords = std::move(raw->baseKeywords),
                    .baseState = std::move(record.baseState),
                    .acceptedState = std::move(record.acceptedState),
                    .errorJson = std::move(record.errorJson),
                });
            }
            return typed;
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
            std::erase_if(values, [&removed](const std::string& value)
                          { return std::ranges::find(removed, value) != removed.end(); });
        }

    } // namespace

    EmailMutationJournal::EmailMutationJournal(javelin::jmap::cache::DatabaseConnection& connection)
        : m_connection(connection), m_repository(connection)
    {
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    EmailMutationJournal::put(const EmailMutationRecord& record)
    {
        const auto generic = genericRecord(record);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&generic))
        {
            return *error;
        }
        return m_repository.put(std::get<MutationRecord>(generic));
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    EmailMutationJournal::queue(const EmailMutationRecord& record,
                                const javelin::jmap::domain::Email& projectedEmail)
    {
        return queueGroup(record, projectedEmail, {});
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    EmailMutationJournal::queueGroup(const EmailMutationRecord& record,
                                     const javelin::jmap::domain::Email& projectedEmail,
                                     const std::span<const MutationRecord> companionRecords)
    {
        auto transactionResult = MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Begin Email mutation projection"));
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
        {
            return *error;
        }
        auto transaction = std::get<MutationProjectionTransaction>(std::move(transactionResult));
        const auto generic = genericRecord(record);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&generic))
        {
            return *error;
        }
        if (const auto error = transaction.append(std::get<MutationRecord>(generic)))
        {
            return error;
        }
        for (const auto& companion : companionRecords)
        {
            if (const auto error = transaction.append(companion))
            {
                return error;
            }
        }
        javelin::jmap::cache::EmailRepository emails{m_connection};
        if (const auto error = emails.upsertMany(transaction.cacheTransaction(), record.accountId,
                                                 {projectedEmail}))
        {
            return error;
        }
        std::vector<std::string> affectedMailboxIds = record.patch.addMailboxIds;
        affectedMailboxIds.insert(affectedMailboxIds.end(), record.patch.removeMailboxIds.begin(),
                                  record.patch.removeMailboxIds.end());
        if (record.patch.destroy && record.baseMailboxIds.has_value())
        {
            affectedMailboxIds.insert(affectedMailboxIds.end(), record.baseMailboxIds->begin(),
                                      record.baseMailboxIds->end());
        }
        std::ranges::sort(affectedMailboxIds);
        const auto uniqueEnd = std::ranges::unique(affectedMailboxIds).begin();
        affectedMailboxIds.erase(uniqueEnd, affectedMailboxIds.end());
        javelin::jmap::cache::MailboxWindowRepository windows{m_connection};
        for (const auto& mailboxId : affectedMailboxIds)
        {
            if (const auto error = windows.invalidateMailbox(
                    transaction.cacheTransaction(), record.accountId, mailboxId,
                    javelin::jmap::cache::QueryWindowCoverage::LocallyProjected))
                return error;
        }
        javelin::jmap::cache::SearchWindowRepository searchWindows{m_connection};
        if (const auto error =
                searchWindows.projectAccount(transaction.cacheTransaction(), record.accountId))
            return error;
        return transaction.commit();
    }

    std::variant<std::vector<EmailMutationRecord>, javelin::jmap::cache::DatabaseError>
    EmailMutationJournal::listForEmail(const std::string_view accountId,
                                       const std::string_view emailId) const
    {
        return typedRecords(m_repository.listForObject(
            {.accountId = std::string{accountId}, .dataType = "Email"}, emailId));
    }

    std::variant<std::vector<EmailMutationRecord>, javelin::jmap::cache::DatabaseError>
    EmailMutationJournal::listForOperationGroup(const std::string_view accountId,
                                                const std::string_view operationGroupId) const
    {
        return typedRecords(m_repository.listForOperationGroup(
            {.accountId = std::string{accountId}, .dataType = "Email"}, operationGroupId));
    }

    std::variant<std::vector<EmailMutationRecord>, javelin::jmap::cache::DatabaseError>
    EmailMutationJournal::listByStatus(const std::string_view accountId,
                                       const MutationStatus status, const std::size_t limit) const
    {
        return typedRecords(m_repository.listByStatus(
            {.accountId = std::string{accountId}, .dataType = "Email"}, status, limit));
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    EmailMutationJournal::transition(const std::string_view mutationId, const MutationStatus status,
                                     const std::optional<std::string_view> acceptedState,
                                     const std::optional<std::string_view> errorJson)
    {
        return m_repository.transition(mutationId, status, acceptedState, errorJson);
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    EmailMutationJournal::remove(const std::string_view mutationId)
    {
        return m_repository.remove(mutationId);
    }

    javelin::jmap::domain::Email
    projectEmailMutations(const javelin::jmap::domain::Email& confirmedEmail,
                          const std::vector<EmailMutationRecord>& mutations)
    {
        auto projected = confirmedEmail;
        for (const auto& mutation : mutations)
        {
            if (!projectsOptimistically(mutation.status) || mutation.patch.emailId != projected.id)
            {
                continue;
            }
            applyAdd(projected.mailboxIds, mutation.patch.addMailboxIds);
            applyRemove(projected.mailboxIds, mutation.patch.removeMailboxIds);
            applyAdd(projected.keywords, mutation.patch.addKeywords);
            applyRemove(projected.keywords, mutation.patch.removeKeywords);
        }
        std::ranges::sort(projected.mailboxIds);
        std::ranges::sort(projected.keywords);
        return projected;
    }

} // namespace javelin::jmap::sync
