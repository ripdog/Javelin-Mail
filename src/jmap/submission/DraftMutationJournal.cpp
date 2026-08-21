#include "jmap/submission/DraftMutationJournal.h"

#include "jmap/cache/ComposeSessionRepository.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailboxWindowRepository.h"
#include "jmap/cache/SearchWindowRepository.h"
#include "jmap/domain/MailEntityParsers.h"
#include "jmap/sync/ConsistencyDomain.h"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <array>

namespace
{
    struct RawDraftMutation
    {
        std::string kind;
        std::string composeSessionId;
        std::optional<std::string> baseDraftEmailId;
        std::optional<glz::raw_json> baseEmail;
        std::optional<glz::raw_json> projectedEmail;
    };
} // namespace

template <> struct glz::meta<RawDraftMutation>
{
    using T = RawDraftMutation;
    static constexpr auto value = glz::object(
        "kind", &T::kind, "composeSessionId", &T::composeSessionId, "baseDraftEmailId",
        &T::baseDraftEmailId, "baseEmail", &T::baseEmail, "projectedEmail", &T::projectedEmail);
};

namespace javelin::jmap::submission
{
    namespace
    {
        [[nodiscard]] std::variant<sync::MutationRecord, cache::DatabaseError>
        record(std::string mutationId, const DraftMutationGroup& group, std::string objectId,
               std::string kind, const std::optional<javelin::jmap::domain::Email>& base,
               const std::optional<javelin::jmap::domain::Email>& projected)
        {
            std::optional<std::string> baseJson;
            std::optional<std::string> projectedJson;
            if (base.has_value())
                baseJson = javelin::jmap::domain::serializeEmail(*base);
            if (projected.has_value())
                projectedJson = javelin::jmap::domain::serializeEmail(*projected);
            if ((base.has_value() && !baseJson.has_value()) ||
                (projected.has_value() && !projectedJson.has_value()))
                return cache::DatabaseError{
                    .code = cache::DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("Unable to serialize a draft mutation."),
                };
            std::string payload;
            if (glz::write_json(
                    RawDraftMutation{
                        .kind = kind,
                        .composeSessionId = group.baseSnapshot.composeSessionId,
                        .baseDraftEmailId = group.baseSnapshot.draftEmailId,
                        .baseEmail = baseJson.has_value()
                                         ? std::optional<glz::raw_json>{glz::raw_json{*baseJson}}
                                         : std::nullopt,
                        .projectedEmail =
                            projectedJson.has_value()
                                ? std::optional<glz::raw_json>{glz::raw_json{*projectedJson}}
                                : std::nullopt,
                    },
                    payload))
                return cache::DatabaseError{
                    .code = cache::DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("Unable to serialize a draft journal record."),
                };
            return sync::MutationRecord{
                .mutationId = std::move(mutationId),
                .operationGroupId = group.operationGroupId,
                .domain = {.accountId = group.accountId, .dataType = "Email"},
                .objectId = std::move(objectId),
                .mutationKind = std::move(kind),
                .status = sync::MutationStatus::Pending,
                .payloadJson = std::move(payload),
                .baseState = std::nullopt,
                .acceptedState = std::nullopt,
                .errorJson = std::nullopt,
            };
        }

        [[nodiscard]] std::optional<cache::DatabaseError>
        advanceEmail(sync::MutationProjectionTransaction& transaction, const std::string& accountId)
        {
            const std::array domains{sync::ConsistencyDomain{
                .accountId = accountId,
                .dataType = "Email",
            }};
            return transaction.advance(domains);
        }

        [[nodiscard]] std::vector<std::string> affectedMailboxIds(const DraftMutationGroup& group)
        {
            auto mailboxIds = group.projectedEmail.mailboxIds;
            if (group.baseEmail.has_value())
            {
                mailboxIds.insert(mailboxIds.end(), group.baseEmail->mailboxIds.begin(),
                                  group.baseEmail->mailboxIds.end());
            }
            std::ranges::sort(mailboxIds);
            mailboxIds.erase(std::ranges::unique(mailboxIds).begin(), mailboxIds.end());
            return mailboxIds;
        }

        [[nodiscard]] std::optional<cache::DatabaseError>
        projectQueryWindows(cache::DatabaseConnection& connection,
                            sync::MutationProjectionTransaction& transaction,
                            const DraftMutationGroup& group)
        {
            cache::MailboxWindowRepository mailboxWindows{connection};
            for (const auto& mailboxId : affectedMailboxIds(group))
            {
                if (const auto error = mailboxWindows.invalidateMailbox(
                        transaction.cacheTransaction(), group.accountId, mailboxId,
                        cache::QueryWindowCoverage::LocallyProjected))
                {
                    return error;
                }
            }
            cache::SearchWindowRepository searchWindows{connection};
            return searchWindows.projectAccount(transaction.cacheTransaction(), group.accountId);
        }
    } // namespace

    DraftMutationJournal::DraftMutationJournal(cache::DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    std::optional<cache::DatabaseError> DraftMutationJournal::queue(const DraftMutationGroup& group)
    {
        auto transactionResult = sync::MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Project draft replacement"));
        if (const auto* error = std::get_if<cache::DatabaseError>(&transactionResult))
            return *error;
        auto transaction =
            std::get<sync::MutationProjectionTransaction>(std::move(transactionResult));
        const auto create = record(group.createMutationId, group, group.temporaryEmailId,
                                   "email_draft_create", std::nullopt, group.projectedEmail);
        if (const auto* error = std::get_if<cache::DatabaseError>(&create))
            return *error;
        if (const auto error = transaction.append(std::get<sync::MutationRecord>(create)))
            return error;
        if (group.destroyMutationId.has_value() && group.replacedEmailId.has_value())
        {
            const auto destroy = record(*group.destroyMutationId, group, *group.replacedEmailId,
                                        "email_draft_destroy", group.baseEmail, std::nullopt);
            if (const auto* error = std::get_if<cache::DatabaseError>(&destroy))
                return *error;
            if (const auto error = transaction.append(std::get<sync::MutationRecord>(destroy)))
                return error;
        }
        cache::EmailRepository emails{m_connection};
        if (const auto error = emails.upsertMany(transaction.cacheTransaction(), group.accountId,
                                                 {group.projectedEmail}))
            return error;
        if (group.replacedEmailId.has_value())
        {
            const std::array ids{*group.replacedEmailId};
            if (const auto error =
                    emails.removeMany(transaction.cacheTransaction(), group.accountId, ids))
                return error;
        }
        if (const auto error = projectQueryWindows(m_connection, transaction, group))
            return error;
        auto projectedSnapshot = group.baseSnapshot;
        projectedSnapshot.draftEmailId = group.temporaryEmailId;
        cache::ComposeSessionRepository composeSessions{m_connection};
        if (const auto error =
                composeSessions.upsert(transaction.cacheTransaction(), projectedSnapshot))
            return error;
        return transaction.commit();
    }

    std::optional<cache::DatabaseError>
    DraftMutationJournal::transition(const DraftMutationGroup& group,
                                     const sync::MutationStatus status)
    {
        auto transactionResult = sync::MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Transition draft replacement"));
        if (const auto* error = std::get_if<cache::DatabaseError>(&transactionResult))
            return *error;
        auto transaction =
            std::get<sync::MutationProjectionTransaction>(std::move(transactionResult));
        if (const auto error = transaction.transition(group.createMutationId, status))
            return error;
        if (group.destroyMutationId.has_value())
        {
            if (const auto error = transaction.transition(*group.destroyMutationId, status))
                return error;
        }
        return transaction.commit();
    }

    std::optional<cache::DatabaseError>
    DraftMutationJournal::transitionDestruction(const DraftMutationGroup& group,
                                                const sync::MutationStatus status)
    {
        if (!group.destroyMutationId.has_value())
            return std::nullopt;
        sync::MutationJournalRepository journal{m_connection};
        return journal.transition(*group.destroyMutationId, status);
    }

    std::optional<cache::DatabaseError>
    DraftMutationJournal::rejectCreation(const DraftMutationGroup& group,
                                         const std::optional<std::string_view> errorJson)
    {
        auto transactionResult = sync::MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Reject draft creation"));
        if (const auto* error = std::get_if<cache::DatabaseError>(&transactionResult))
            return *error;
        auto transaction =
            std::get<sync::MutationProjectionTransaction>(std::move(transactionResult));
        if (const auto error = transaction.transition(
                group.createMutationId, sync::MutationStatus::Rejected, std::nullopt, errorJson))
            return error;
        if (group.destroyMutationId.has_value())
        {
            if (const auto error =
                    transaction.transition(*group.destroyMutationId, sync::MutationStatus::Rejected,
                                           std::nullopt, errorJson))
                return error;
        }
        cache::EmailRepository emails{m_connection};
        const std::array temporaryIds{group.temporaryEmailId};
        if (const auto error =
                emails.removeMany(transaction.cacheTransaction(), group.accountId, temporaryIds))
            return error;
        if (group.baseEmail.has_value())
        {
            if (const auto error = emails.upsertMany(transaction.cacheTransaction(),
                                                     group.accountId, {*group.baseEmail}))
                return error;
        }
        if (const auto error = projectQueryWindows(m_connection, transaction, group))
            return error;
        cache::ComposeSessionRepository composeSessions{m_connection};
        if (const auto error =
                composeSessions.upsert(transaction.cacheTransaction(), group.baseSnapshot))
            return error;
        if (const auto error = transaction.retireTerminal())
            return error;
        return transaction.commit();
    }

    std::optional<cache::DatabaseError> DraftMutationJournal::acceptCreation(
        const DraftMutationGroup& group, const javelin::jmap::domain::Email& acceptedEmail,
        const DraftSnapshot& snapshot, const std::string_view acceptedState)
    {
        auto transactionResult = sync::MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Accept draft creation"));
        if (const auto* error = std::get_if<cache::DatabaseError>(&transactionResult))
            return *error;
        auto transaction =
            std::get<sync::MutationProjectionTransaction>(std::move(transactionResult));
        if (const auto error = transaction.transition(
                group.createMutationId, sync::MutationStatus::Accepted, acceptedState))
            return error;
        if (const auto error = advanceEmail(transaction, group.accountId))
            return error;
        cache::EmailRepository emails{m_connection};
        const std::array temporaryIds{group.temporaryEmailId};
        if (const auto error =
                emails.removeMany(transaction.cacheTransaction(), group.accountId, temporaryIds))
            return error;
        if (const auto error =
                emails.upsertMany(transaction.cacheTransaction(), group.accountId, {acceptedEmail}))
            return error;
        if (const auto error = projectQueryWindows(m_connection, transaction, group))
            return error;
        cache::ComposeSessionRepository composeSessions{m_connection};
        if (const auto error = composeSessions.upsert(transaction.cacheTransaction(), snapshot))
            return error;
        if (const auto error = transaction.retireTerminal())
            return error;
        return transaction.commit();
    }

    std::optional<cache::DatabaseError>
    DraftMutationJournal::acceptDestruction(const DraftMutationGroup& group,
                                            const std::string_view acceptedState)
    {
        if (!group.destroyMutationId.has_value())
            return std::nullopt;
        auto transactionResult = sync::MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Accept replaced draft destruction"));
        if (const auto* error = std::get_if<cache::DatabaseError>(&transactionResult))
            return *error;
        auto transaction =
            std::get<sync::MutationProjectionTransaction>(std::move(transactionResult));
        if (const auto error = transaction.transition(
                *group.destroyMutationId, sync::MutationStatus::Accepted, acceptedState))
            return error;
        if (const auto error = advanceEmail(transaction, group.accountId))
            return error;
        if (const auto error = transaction.retireTerminal())
            return error;
        return transaction.commit();
    }

    std::optional<cache::DatabaseError>
    DraftMutationJournal::rejectDestruction(const DraftMutationGroup& group,
                                            const std::optional<std::string_view> acceptedState,
                                            const std::optional<std::string_view> errorJson)
    {
        if (!group.destroyMutationId.has_value())
            return std::nullopt;
        auto transactionResult = sync::MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Reject replaced draft destruction"));
        if (const auto* error = std::get_if<cache::DatabaseError>(&transactionResult))
            return *error;
        auto transaction =
            std::get<sync::MutationProjectionTransaction>(std::move(transactionResult));
        if (const auto error = transaction.transition(
                *group.destroyMutationId, sync::MutationStatus::Rejected, acceptedState, errorJson))
            return error;
        if (group.baseEmail.has_value())
        {
            cache::EmailRepository emails{m_connection};
            if (const auto error = emails.upsertMany(transaction.cacheTransaction(),
                                                     group.accountId, {*group.baseEmail}))
                return error;
        }
        if (const auto error = projectQueryWindows(m_connection, transaction, group))
            return error;
        if (const auto error = transaction.retireTerminal())
            return error;
        return transaction.commit();
    }

    std::variant<bool, cache::DatabaseError>
    DraftMutationJournal::hasActiveForCompose(const std::string_view accountId,
                                              const std::string_view composeSessionId) const
    {
        sync::MutationJournalRepository journal{m_connection};
        const auto active =
            journal.listActive({.accountId = std::string{accountId}, .dataType = "Email"});
        if (const auto* error = std::get_if<cache::DatabaseError>(&active))
            return *error;
        for (const auto& mutation : std::get<std::vector<sync::MutationRecord>>(active))
        {
            if (mutation.mutationKind != "email_draft_create" &&
                mutation.mutationKind != "email_draft_destroy")
                continue;
            RawDraftMutation payload;
            if (glz::read_json(payload, mutation.payloadJson))
                return cache::DatabaseError{
                    .code = cache::DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("Unable to parse an active draft mutation."),
                };
            if (payload.composeSessionId == composeSessionId)
                return true;
        }
        return false;
    }
} // namespace javelin::jmap::submission
