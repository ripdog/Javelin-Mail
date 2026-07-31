#include "app/undo/MailHistoryExecutor.h"

#include "app/MailApplicationService.h"
#include "jmap/OperationError.h"

#include <QUuid>

#include <algorithm>
#include <ranges>
#include <unordered_map>
#include <utility>

namespace javelin::app::undo
{
    namespace
    {
        [[nodiscard]] bool contains(const std::vector<std::string>& values,
                                    const std::string& value)
        {
            return std::ranges::find(values, value) != values.end();
        }

        [[nodiscard]] bool patchIsApplied(const javelin::jmap::domain::Email& email,
                                          const ExactMailPatch& patch)
        {
            return std::ranges::all_of(patch.addMailboxIds, [&](const auto& id)
                                       { return contains(email.mailboxIds, id); }) &&
                   std::ranges::none_of(patch.removeMailboxIds, [&](const auto& id)
                                        { return contains(email.mailboxIds, id); }) &&
                   std::ranges::all_of(patch.addKeywords, [&](const auto& id)
                                       { return contains(email.keywords, id); }) &&
                   std::ranges::none_of(patch.removeKeywords, [&](const auto& id)
                                        { return contains(email.keywords, id); });
        }

        [[nodiscard]] javelin::jmap::EmailMailboxMutation
        mutationFrom(const MailPatchItemHistory& item, const ExactMailPatch& patch,
                     const std::string& operationGroupId, const std::string& state,
                     const javelin::jmap::domain::Email& authoritativeEmail)
        {
            return {
                .emailId = item.emailId,
                .addMailboxIds = patch.addMailboxIds,
                .removeMailboxIds = patch.removeMailboxIds,
                .addKeywords = patch.addKeywords,
                .removeKeywords = patch.removeKeywords,
                .operationGroupId = operationGroupId,
                .ifInState = state,
                .authoritativeMailboxIds = authoritativeEmail.mailboxIds,
                .authoritativeKeywords = authoritativeEmail.keywords,
            };
        }

        [[nodiscard]] HistoryExecutionOutcome outcomeFor(const javelin::jmap::OperationError& error)
        {
            using enum javelin::jmap::OperationErrorCode;
            if (error.code == Conflict || error.code == PreconditionFailed ||
                error.code == NotFound)
                return HistoryExecutionOutcome::Conflict;
            if (error.code == AuthenticationRequired || error.code == PermissionDenied ||
                error.code == InvalidRequest || error.code == InvalidUserInput ||
                error.code == UnsupportedCapability)
                return HistoryExecutionOutcome::DefinitiveFailure;
            return HistoryExecutionOutcome::Unknown;
        }

        [[nodiscard]] HistoryExecutionResult
        failure(const HistoryExecutionOutcome outcome, QString summary,
                std::vector<HistoryObjectFailure> objectFailures = {})
        {
            return {
                .outcome = outcome,
                .updatedPayload = std::nullopt,
                .refreshScope = {},
                .summary = std::move(summary),
                .objectFailures = std::move(objectFailures),
                .mayRemoveFromHistory = outcome == HistoryExecutionOutcome::Conflict ||
                                        outcome == HistoryExecutionOutcome::DefinitiveFailure,
            };
        }
    } // namespace

    MailHistoryExecutor::MailHistoryExecutor(MailHistoryPort& mailService)
        : m_mailService(mailService)
    {
    }

    QCoro::Task<HistoryExecutionResult>
    MailHistoryExecutor::execute(HistoryEntry entry, const HistoryExecutionDirection direction)
    {
        auto* history = std::get_if<MailPatchHistory>(&entry.payload);
        if (history == nullptr || history->items.empty())
        {
            co_return failure(HistoryExecutionOutcome::DefinitiveFailure,
                              QStringLiteral("The mail history payload is incomplete."));
        }
        if (direction == HistoryExecutionDirection::Recover)
        {
            co_return failure(
                HistoryExecutionOutcome::Unknown,
                QStringLiteral("The previous mail history request requires reconciliation."));
        }

        const auto& accountId = history->items.front().accountId;
        if (std::ranges::any_of(history->items,
                                [&](const auto& item) { return item.accountId != accountId; }))
        {
            co_return failure(
                HistoryExecutionOutcome::DefinitiveFailure,
                QStringLiteral("A mail history entry spans multiple accounts unexpectedly."));
        }

        std::vector<std::string> emailIds;
        emailIds.reserve(history->items.size());
        for (const auto& item : history->items)
            emailIds.push_back(item.emailId);

        auto authoritativeResult = m_mailService.getEffectiveEmails(accountId, emailIds);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&authoritativeResult))
        {
            co_return failure(outcomeFor(*error), error->message);
        }
        const auto& authoritative =
            std::get<javelin::jmap::AuthoritativeEmails>(authoritativeResult);
        std::unordered_map<std::string, const javelin::jmap::domain::Email*> emails;
        for (const auto& email : authoritative.emails)
            emails.emplace(email.id, &email);

        std::vector<HistoryObjectFailure> conflicts;
        for (const auto& item : history->items)
        {
            const auto found = emails.find(item.emailId);
            const auto displayName = QString::fromStdString(item.subject.value_or(item.emailId));
            if (found == emails.end())
            {
                conflicts.push_back({
                    .objectId = QString::fromStdString(item.emailId),
                    .summary =
                        displayName + QStringLiteral(" is no longer available on the server."),
                });
                continue;
            }
            const auto& expected =
                direction == HistoryExecutionDirection::Undo ? item.forward : item.inverse;
            if (!patchIsApplied(*found->second, expected))
            {
                conflicts.push_back({
                    .objectId = QString::fromStdString(item.emailId),
                    .summary =
                        displayName +
                        QStringLiteral(" no longer has the expected mailbox or keyword state."),
                });
            }
        }
        if (!conflicts.empty())
        {
            auto result = failure(HistoryExecutionOutcome::Conflict,
                                  QStringLiteral("The mail operation changed on another client."),
                                  std::move(conflicts));
            result.refreshScope = {
                .accountIds = {QString::fromStdString(accountId)},
                .objectTypes = {QStringLiteral("Email")},
                .views = {QStringLiteral("mail")},
            };
            co_return result;
        }

        const auto operationGroupId =
            QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        std::vector<javelin::jmap::EmailMailboxMutation> mutations;
        mutations.reserve(history->items.size());
        for (const auto& item : history->items)
        {
            const auto& patch =
                direction == HistoryExecutionDirection::Undo ? item.inverse : item.forward;
            const auto authoritativeEmail = emails.find(item.emailId);
            mutations.push_back(mutationFrom(item, patch, operationGroupId, authoritative.state,
                                             *authoritativeEmail->second));
        }
        auto queuedResult = m_mailService.queueExactEmailMutations(accountId, std::move(mutations));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&queuedResult))
            co_return failure(outcomeFor(*error), error->message);
        const auto& queued =
            std::get<std::vector<javelin::jmap::QueuedEmailMutation>>(queuedResult);
        for (std::size_t index = 0; index < history->items.size(); ++index)
            history->items[index].mutationId = queued[index].mutationId;

        auto submitResult =
            co_await m_mailService.submitPendingEmailMutations(accountId, operationGroupId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&submitResult))
        {
            auto result = failure(outcomeFor(*error), error->message);
            result.updatedPayload = *history;
            co_return result;
        }
        const auto& submitted = std::get<javelin::jmap::SubmittedEmailMutations>(submitResult);
        if (submitted.updatedEmailCount == submitted.attemptedEmailCount)
        {
            co_return HistoryExecutionResult{
                .outcome = HistoryExecutionOutcome::Success,
                .updatedPayload = *history,
                .refreshScope =
                    {
                        .accountIds = {QString::fromStdString(accountId)},
                        .objectTypes = {QStringLiteral("Email")},
                        .views = {QStringLiteral("mail")},
                    },
                .summary = {},
                .objectFailures = {},
                .mayRemoveFromHistory = false,
            };
        }
        if (submitted.updatedEmailCount > 0)
        {
            std::vector<std::string> acceptedEmailIds;
            for (const auto& item : submitted.items)
            {
                if (item.accepted)
                    acceptedEmailIds.push_back(item.emailId);
            }
            auto compensationBase =
                co_await m_mailService.getAuthoritativeEmails(accountId, acceptedEmailIds);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&compensationBase))
            {
                auto result = failure(HistoryExecutionOutcome::PartialFailure, error->message);
                result.updatedPayload = *history;
                co_return result;
            }

            const auto& current = std::get<javelin::jmap::AuthoritativeEmails>(compensationBase);
            std::unordered_map<std::string, const javelin::jmap::domain::Email*> currentEmails;
            for (const auto& email : current.emails)
                currentEmails.emplace(email.id, &email);

            const auto compensationGroupId =
                QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
            std::vector<javelin::jmap::EmailMailboxMutation> compensationMutations;
            compensationMutations.reserve(acceptedEmailIds.size());
            for (const auto& acceptedId : acceptedEmailIds)
            {
                const auto historyItem =
                    std::ranges::find(history->items, acceptedId, &MailPatchItemHistory::emailId);
                const auto currentEmail = currentEmails.find(acceptedId);
                if (historyItem == history->items.end() || currentEmail == currentEmails.end())
                {
                    auto result = failure(
                        HistoryExecutionOutcome::PartialFailure,
                        QStringLiteral(
                            "The partially changed mail operation could not be compensated."));
                    result.updatedPayload = *history;
                    co_return result;
                }
                const auto& attemptedPatch = direction == HistoryExecutionDirection::Undo
                                                 ? historyItem->inverse
                                                 : historyItem->forward;
                if (!patchIsApplied(*currentEmail->second, attemptedPatch))
                {
                    auto result = failure(
                        HistoryExecutionOutcome::PartialFailure,
                        QStringLiteral(
                            "A partially changed message no longer matches the server result."));
                    result.updatedPayload = *history;
                    co_return result;
                }
                const auto& compensationPatch = direction == HistoryExecutionDirection::Undo
                                                    ? historyItem->forward
                                                    : historyItem->inverse;
                compensationMutations.push_back(mutationFrom(*historyItem, compensationPatch,
                                                             compensationGroupId, current.state,
                                                             *currentEmail->second));
            }
            const auto queuedCompensation =
                m_mailService.queueExactEmailMutations(accountId, std::move(compensationMutations));
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&queuedCompensation))
            {
                auto result = failure(HistoryExecutionOutcome::PartialFailure, error->message);
                result.updatedPayload = *history;
                co_return result;
            }

            const auto compensated =
                co_await m_mailService.submitPendingEmailMutations(accountId, compensationGroupId);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&compensated))
            {
                auto result = failure(HistoryExecutionOutcome::PartialFailure, error->message);
                result.updatedPayload = *history;
                co_return result;
            }
            const auto& compensationSummary =
                std::get<javelin::jmap::SubmittedEmailMutations>(compensated);
            if (compensationSummary.updatedEmailCount != acceptedEmailIds.size())
            {
                auto result = failure(
                    HistoryExecutionOutcome::PartialFailure,
                    QStringLiteral("The server rejected part of the compensation request."));
                result.updatedPayload = *history;
                co_return result;
            }

            auto result = failure(
                HistoryExecutionOutcome::DefinitiveFailure,
                QStringLiteral(
                    "The server accepted only part of the mail operation; the accepted changes "
                    "were restored."));
            result.updatedPayload = *history;
            result.mayRemoveFromHistory = false;
            result.refreshScope = {
                .accountIds = {QString::fromStdString(accountId)},
                .objectTypes = {QStringLiteral("Email")},
                .views = {QStringLiteral("mail")},
            };
            co_return result;
        }
        auto result = failure(HistoryExecutionOutcome::DefinitiveFailure,
                              QStringLiteral("The server rejected the mail operation."));
        result.updatedPayload = *history;
        co_return result;
    }

} // namespace javelin::app::undo
