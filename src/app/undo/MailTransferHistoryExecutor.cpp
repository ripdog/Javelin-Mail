#include "app/undo/MailTransferHistoryExecutor.h"

#include <KLocalizedString>

#include <QUuid>

#include <algorithm>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace javelin::app::undo
{
    namespace
    {
        [[nodiscard]] HistoryExecutionOutcome outcomeFor(const javelin::jmap::OperationError& error)
        {
            if (error.protocolType == std::optional<std::string>{"ambiguousOutcome"})
                return HistoryExecutionOutcome::Unknown;
            if (error.protocolType == std::optional<std::string>{"partialOutcome"})
                return HistoryExecutionOutcome::PartialFailure;
            using enum javelin::jmap::OperationErrorCode;
            if (error.code == Conflict || error.code == PreconditionFailed || error.code == NotFound)
                return HistoryExecutionOutcome::Conflict;
            if (javelin::jmap::isTransientError(error) ||
                javelin::jmap::isAuthenticationError(error))
                return HistoryExecutionOutcome::Unknown;
            return HistoryExecutionOutcome::DefinitiveFailure;
        }

        [[nodiscard]] HistoryExecutionResult
        failure(const HistoryExecutionOutcome outcome, QString message,
                std::optional<MailTransferHistory> history = std::nullopt,
                std::vector<HistoryObjectFailure> objectFailures = {})
        {
            HistoryExecutionResult result{
                .outcome = outcome,
                .updatedPayload = std::nullopt,
                .refreshScope = {},
                .summary = std::move(message),
                .objectFailures = std::move(objectFailures),
                .mayRemoveFromHistory = outcome == HistoryExecutionOutcome::Conflict ||
                                        outcome == HistoryExecutionOutcome::DefinitiveFailure,
            };
            if (history.has_value())
                result.updatedPayload = std::move(*history);
            return result;
        }

        [[nodiscard]] HistoryExecutionResult success(MailTransferHistory history)
        {
            const QString sourceAccountId = QString::fromStdString(history.sourceAccountId);
            const QString destinationAccountId = QString::fromStdString(history.destinationAccountId);
            return {
                .outcome = HistoryExecutionOutcome::Success,
                .updatedPayload = std::move(history),
                .refreshScope =
                    {
                        .accountIds = {sourceAccountId, destinationAccountId},
                        .objectTypes = {QStringLiteral("Email")},
                        .views = {QStringLiteral("mail")},
                    },
                .summary = {},
                .objectFailures = {},
                .mayRemoveFromHistory = false,
            };
        }

        [[nodiscard]] std::vector<std::string> normalized(std::vector<std::string> values)
        {
            std::ranges::sort(values);
            values.erase(std::unique(values.begin(), values.end()), values.end());
            return values;
        }

        [[nodiscard]] bool sameStrings(const std::vector<std::string>& left,
                                       const std::vector<std::string>& right)
        {
            return normalized(left) == normalized(right);
        }

        [[nodiscard]] bool containsAll(const std::vector<std::string>& values,
                                       const std::vector<std::string>& required)
        {
            return std::ranges::all_of(required, [&](const auto& value)
                                       { return std::ranges::contains(values, value); });
        }

        void snapshotSource(MailTransferItemHistory& item,
                            const javelin::jmap::domain::Email& source)
        {
            item.originalSourceMailboxIds = source.mailboxIds;
            item.sourceKeywords = source.keywords;
            item.sourceMessageIds = source.messageId;
            item.sourceReceivedAt = source.receivedAt.empty()
                                        ? std::nullopt
                                        : std::optional<std::string>{source.receivedAt};
            item.sourceSize = source.size;
        }

        [[nodiscard]] std::vector<std::string>
        withAdded(std::vector<std::string> values, const std::string& addition)
        {
            if (!std::ranges::contains(values, addition))
                values.push_back(addition);
            return normalized(std::move(values));
        }

        [[nodiscard]] bool removesAll(const std::vector<std::string>& current,
                                      const std::vector<std::string>& removals)
        {
            return !current.empty() && containsAll(removals, current);
        }

        [[nodiscard]] std::vector<std::string>
        missingFrom(const std::vector<std::string>& values, const std::vector<std::string>& desired)
        {
            std::vector<std::string> missing;
            for (const auto& value : desired)
            {
                if (!std::ranges::contains(values, value))
                    missing.push_back(value);
            }
            return normalized(std::move(missing));
        }

        [[nodiscard]] javelin::jmap::EmailMailboxMutation
        mutation(std::string emailId, std::vector<std::string> addMailboxIds,
                 std::vector<std::string> removeMailboxIds, bool destroy,
                 const javelin::jmap::AuthoritativeEmails& authoritative,
                 const javelin::jmap::domain::Email& email)
        {
            return {
                .emailId = std::move(emailId),
                .addMailboxIds = std::move(addMailboxIds),
                .removeMailboxIds = std::move(removeMailboxIds),
                .addKeywords = {},
                .removeKeywords = {},
                .operationGroupId =
                    QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),
                .ifInState = authoritative.state.empty()
                                 ? std::nullopt
                                 : std::optional<std::string>{authoritative.state},
                .authoritativeMailboxIds = email.mailboxIds,
                .authoritativeKeywords = email.keywords,
                .destroy = destroy,
            };
        }

        [[nodiscard]] const javelin::jmap::domain::Email*
        oneEmail(const javelin::jmap::AuthoritativeEmails& authoritative, std::string_view emailId)
        {
            const auto found = std::ranges::find(authoritative.emails, emailId,
                                                 &javelin::jmap::domain::Email::id);
            return found == authoritative.emails.end() ? nullptr : &*found;
        }

        [[nodiscard]] bool acceptedExactlyOne(const javelin::jmap::SubmittedEmailMutations& submitted)
        {
            return submitted.attemptedEmailCount == 1 && submitted.updatedEmailCount == 1 &&
                   submitted.failedEmailCount == 0 && submitted.items.size() == 1 &&
                   submitted.items.front().accepted;
        }

        [[nodiscard]] HistoryExecutionResult
        errorResult(const javelin::jmap::OperationError& error, MailTransferHistory history,
                    const bool alreadyChanged)
        {
            auto outcome = outcomeFor(error);
            if (alreadyChanged && outcome != HistoryExecutionOutcome::Unknown)
                outcome = HistoryExecutionOutcome::PartialFailure;
            auto result = failure(outcome, error.message, std::move(history));
            result.mayRemoveFromHistory = false;
            return result;
        }

        [[nodiscard]] HistoryExecutionResult
        rejection(QString message, MailTransferHistory history, const bool alreadyChanged)
        {
            auto result = failure(alreadyChanged ? HistoryExecutionOutcome::PartialFailure
                                                 : HistoryExecutionOutcome::DefinitiveFailure,
                                  std::move(message), std::move(history));
            result.mayRemoveFromHistory = false;
            return result;
        }
    } // namespace

    MailTransferHistoryExecutor::MailTransferHistoryExecutor(MailTransferHistoryPort& mail)
        : m_mail(mail)
    {
    }

    QCoro::Task<HistoryExecutionResult>
    MailTransferHistoryExecutor::execute(HistoryEntry entry,
                                         const HistoryExecutionDirection direction)
    {
        auto* history = std::get_if<MailTransferHistory>(&entry.payload);
        if (history == nullptr || history->items.empty() || history->sourceAccountId.empty() ||
            history->destinationAccountId.empty() || history->destinationMailboxId.empty())
        {
            co_return failure(HistoryExecutionOutcome::DefinitiveFailure,
                              i18n("The mail transfer history payload is incomplete."));
        }
        if (direction == HistoryExecutionDirection::Recover)
        {
            co_return failure(HistoryExecutionOutcome::Unknown,
                              i18n("The previous mail transfer history request requires "
                                   "authoritative reconciliation."),
                              *history);
        }
        if (direction == HistoryExecutionDirection::Redo)
        {
            bool changed = false;
            for (auto& item : history->items)
            {
                if (!item.currentSourceEmailId.has_value())
                {
                    co_return failure(
                        changed ? HistoryExecutionOutcome::PartialFailure
                                : HistoryExecutionOutcome::Conflict,
                        i18n("The source message is no longer available to repeat this transfer."),
                        *history);
                }
                const std::string sourceEmailId = *item.currentSourceEmailId;
                auto sourceResult = co_await m_mail.getAuthoritativeEmails(
                    history->sourceAccountId, {sourceEmailId});
                if (const auto* error =
                        std::get_if<javelin::jmap::OperationError>(&sourceResult))
                    co_return errorResult(*error, *history, changed);
                const auto& source =
                    std::get<javelin::jmap::AuthoritativeEmails>(sourceResult);
                const auto* currentSource = oneEmail(source, sourceEmailId);
                if (currentSource == nullptr)
                {
                    co_return failure(
                        changed ? HistoryExecutionOutcome::PartialFailure
                                : HistoryExecutionOutcome::Conflict,
                        i18n("The source message is no longer available to repeat this transfer."),
                        *history);
                }
                if (history->operation == MailTransferHistoryOperation::Move &&
                    (item.sourceRemovedMailboxIds.empty() ||
                     !containsAll(currentSource->mailboxIds, item.sourceRemovedMailboxIds)))
                {
                    co_return failure(
                        changed ? HistoryExecutionOutcome::PartialFailure
                                : HistoryExecutionOutcome::Conflict,
                        i18n("The source message no longer has the mailbox state required to repeat "
                             "this move."),
                        *history);
                }

                const javelin::jmap::domain::Email* currentDestination = nullptr;
                std::optional<javelin::jmap::AuthoritativeEmails> destination;
                if (item.currentDestinationEmailId.has_value())
                {
                    auto destinationResult = co_await m_mail.getAuthoritativeEmails(
                        history->destinationAccountId, {*item.currentDestinationEmailId});
                    if (const auto* error =
                            std::get_if<javelin::jmap::OperationError>(&destinationResult))
                        co_return errorResult(*error, *history, changed);
                    destination =
                        std::get<javelin::jmap::AuthoritativeEmails>(std::move(destinationResult));
                    currentDestination = oneEmail(*destination, *item.currentDestinationEmailId);
                    if (currentDestination == nullptr)
                        item.currentDestinationEmailId = std::nullopt;
                }

                if (!item.currentDestinationEmailId.has_value())
                {
                    auto redone = co_await m_mail.redoMissingDestination(
                        entry.entryId, history->operation, history->sourceAccountId,
                        history->destinationAccountId, history->destinationMailboxId, item);
                    if (const auto* error =
                            std::get_if<javelin::jmap::OperationError>(&redone))
                        co_return errorResult(*error, *history, changed);
                    item = std::get<MailTransferItemHistory>(std::move(redone));
                    changed = true;
                    continue;
                }

                if (currentDestination == nullptr || !destination.has_value())
                {
                    co_return failure(
                        changed ? HistoryExecutionOutcome::PartialFailure
                                : HistoryExecutionOutcome::Conflict,
                        i18n("The destination message could not be resolved for Redo."), *history);
                }

                const auto priorDestinationMailboxes = currentDestination->mailboxIds;
                const auto destinationKeywords = currentDestination->keywords;
                const bool targetPresent = std::ranges::contains(
                    currentDestination->mailboxIds, history->destinationMailboxId);
                if (!targetPresent)
                {
                    auto applied = co_await m_mail.applyExactEmailMutation(
                        history->destinationAccountId,
                        mutation(currentDestination->id, {history->destinationMailboxId}, {}, false,
                                 *destination, *currentDestination));
                    if (const auto* error =
                            std::get_if<javelin::jmap::OperationError>(&applied))
                        co_return errorResult(*error, *history, changed);
                    if (!acceptedExactlyOne(
                            std::get<javelin::jmap::SubmittedEmailMutations>(applied)))
                    {
                        co_return rejection(i18n("The destination server rejected restoring the "
                                                 "transfer mailbox membership."),
                                            *history, changed);
                    }
                    changed = true;
                }
                item.destinationPriorMailboxIds = priorDestinationMailboxes;
                item.destinationMailboxIds =
                    withAdded(priorDestinationMailboxes, history->destinationMailboxId);
                item.destinationKeywords = destinationKeywords;

                snapshotSource(item, *currentSource);
                if (history->operation == MailTransferHistoryOperation::Copy)
                {
                    item.sourceDestroyed = false;
                    item.currentSourceEmailId = sourceEmailId;
                    continue;
                }

                const bool actualDestroy =
                    removesAll(currentSource->mailboxIds, item.sourceRemovedMailboxIds);
                if (actualDestroy)
                {
                    auto retained = co_await m_mail.retainSourceForHistory(
                        entry.entryId, history->sourceAccountId, sourceEmailId);
                    if (const auto* error =
                            std::get_if<javelin::jmap::OperationError>(&retained))
                        co_return errorResult(*error, *history, changed);
                    item.rawContentHash = std::get<std::string>(std::move(retained));
                }

                auto applied = co_await m_mail.applyExactEmailMutation(
                    history->sourceAccountId,
                    mutation(sourceEmailId, {},
                             actualDestroy ? std::vector<std::string>{}
                                           : item.sourceRemovedMailboxIds,
                             actualDestroy, source, *currentSource));
                if (const auto* error =
                        std::get_if<javelin::jmap::OperationError>(&applied))
                    co_return errorResult(*error, *history, changed);
                if (!acceptedExactlyOne(std::get<javelin::jmap::SubmittedEmailMutations>(applied)))
                {
                    co_return rejection(i18n("The source server rejected repeating the move."),
                                        *history, changed);
                }
                item.sourceDestroyed = actualDestroy;
                item.currentSourceEmailId = actualDestroy
                                                ? std::nullopt
                                                : std::optional<std::string>{sourceEmailId};
                changed = true;
            }
            co_return success(*history);
        }

        bool changed = false;
        for (auto& item : history->items)
        {
            if (history->operation == MailTransferHistoryOperation::Move)
            {
                if (item.sourceDestroyed)
                {
                    if (item.currentSourceEmailId.has_value())
                    {
                        auto currentResult = co_await m_mail.getAuthoritativeEmails(
                            history->sourceAccountId, {*item.currentSourceEmailId});
                        if (const auto* error =
                                std::get_if<javelin::jmap::OperationError>(&currentResult))
                            co_return errorResult(*error, *history, changed);
                        const auto& current =
                            std::get<javelin::jmap::AuthoritativeEmails>(currentResult);
                        const auto* source = oneEmail(current, *item.currentSourceEmailId);
                        if (source == nullptr ||
                            !containsAll(source->mailboxIds, item.originalSourceMailboxIds))
                        {
                            co_return failure(
                                changed ? HistoryExecutionOutcome::PartialFailure
                                        : HistoryExecutionOutcome::Conflict,
                                i18n("A source message recreated by Undo no longer has the expected "
                                     "mailbox state."),
                                *history);
                        }
                    }
                    else
                    {
                        if (!item.rawContentHash.has_value())
                        {
                            co_return failure(
                                changed ? HistoryExecutionOutcome::PartialFailure
                                        : HistoryExecutionOutcome::DefinitiveFailure,
                                i18n("The original raw message required to restore this move is no "
                                     "longer retained."),
                                *history);
                        }
                        auto recreated = co_await m_mail.recreateSourceFromHistory(
                            entry.entryId, history->sourceAccountId, *item.rawContentHash,
                            item.originalSourceMailboxIds, item.sourceKeywords,
                            item.sourceMessageIds, item.sourceReceivedAt, item.sourceSize);
                        if (const auto* error =
                                std::get_if<javelin::jmap::OperationError>(&recreated))
                            co_return errorResult(*error, *history, changed);
                        item.currentSourceEmailId =
                            std::get<RecreatedMailTransferSource>(recreated).emailId;
                        changed = true;
                    }
                }
                else
                {
                    if (!item.currentSourceEmailId.has_value())
                    {
                        co_return failure(
                            changed ? HistoryExecutionOutcome::PartialFailure
                                    : HistoryExecutionOutcome::DefinitiveFailure,
                            i18n("The source message identity is missing from transfer history."),
                            *history);
                    }
                    auto currentResult = co_await m_mail.getAuthoritativeEmails(
                        history->sourceAccountId, {*item.currentSourceEmailId});
                    if (const auto* error =
                            std::get_if<javelin::jmap::OperationError>(&currentResult))
                        co_return errorResult(*error, *history, changed);
                    const auto& current =
                        std::get<javelin::jmap::AuthoritativeEmails>(currentResult);
                    const auto* source = oneEmail(current, *item.currentSourceEmailId);
                    if (source == nullptr)
                    {
                        co_return failure(
                            changed ? HistoryExecutionOutcome::PartialFailure
                                    : HistoryExecutionOutcome::Conflict,
                            i18n("The source message is no longer available on the server."),
                            *history);
                    }
                    const auto additions =
                        missingFrom(source->mailboxIds, item.sourceRemovedMailboxIds);
                    if (!additions.empty())
                    {
                        auto applied = co_await m_mail.applyExactEmailMutation(
                            history->sourceAccountId,
                            mutation(source->id, additions, {}, false, current, *source));
                        if (const auto* error =
                                std::get_if<javelin::jmap::OperationError>(&applied))
                            co_return errorResult(*error, *history, changed);
                        if (!acceptedExactlyOne(
                                std::get<javelin::jmap::SubmittedEmailMutations>(applied)))
                            co_return rejection(i18n("The source server rejected restoring the "
                                                     "message mailbox membership."),
                                                *history, changed);
                        changed = true;
                    }
                }
            }

            if (!item.currentDestinationEmailId.has_value())
            {
                ++item.redoGeneration;
                continue;
            }
            auto destinationResult = co_await m_mail.getAuthoritativeEmails(
                history->destinationAccountId, {*item.currentDestinationEmailId});
            if (const auto* error =
                    std::get_if<javelin::jmap::OperationError>(&destinationResult))
                co_return errorResult(*error, *history, changed);
            const auto& destination =
                std::get<javelin::jmap::AuthoritativeEmails>(destinationResult);
            const auto* currentDestination =
                oneEmail(destination, *item.currentDestinationEmailId);
            if (currentDestination == nullptr)
            {
                item.currentDestinationEmailId = std::nullopt;
                ++item.redoGeneration;
                continue;
            }

            const bool targetPresent = std::ranges::contains(
                currentDestination->mailboxIds, history->destinationMailboxId);
            if (!targetPresent)
            {
                ++item.redoGeneration;
                continue;
            }

            if (std::ranges::contains(item.destinationPriorMailboxIds,
                                      history->destinationMailboxId))
            {
                ++item.redoGeneration;
                continue;
            }

            if (item.destinationReusedExisting)
            {
                if (currentDestination->mailboxIds.size() <= 1)
                {
                    co_return failure(
                        changed ? HistoryExecutionOutcome::PartialFailure
                                : HistoryExecutionOutcome::Conflict,
                        i18n("The pre-existing destination message no longer has another mailbox "
                             "membership, so Undo cannot safely remove the transfer membership."),
                        *history);
                }
                auto applied = co_await m_mail.applyExactEmailMutation(
                    history->destinationAccountId,
                    mutation(currentDestination->id, {}, {history->destinationMailboxId}, false,
                             destination, *currentDestination));
                if (const auto* error =
                        std::get_if<javelin::jmap::OperationError>(&applied))
                    co_return errorResult(*error, *history, changed);
                if (!acceptedExactlyOne(std::get<javelin::jmap::SubmittedEmailMutations>(applied)))
                    co_return rejection(i18n("The destination server rejected removing the "
                                             "transfer mailbox membership."),
                                        *history, changed);
                changed = true;
                ++item.redoGeneration;
                continue;
            }

            if (currentDestination->mailboxIds.size() > 1)
            {
                auto applied = co_await m_mail.applyExactEmailMutation(
                    history->destinationAccountId,
                    mutation(currentDestination->id, {}, {history->destinationMailboxId}, false,
                             destination, *currentDestination));
                if (const auto* error =
                        std::get_if<javelin::jmap::OperationError>(&applied))
                    co_return errorResult(*error, *history, changed);
                if (!acceptedExactlyOne(std::get<javelin::jmap::SubmittedEmailMutations>(applied)))
                    co_return rejection(i18n("The destination server rejected removing the "
                                             "transfer mailbox membership."),
                                        *history, changed);
                changed = true;
                ++item.redoGeneration;
                continue;
            }

            if (!sameStrings(currentDestination->mailboxIds, item.destinationMailboxIds) ||
                !sameStrings(currentDestination->keywords, item.destinationKeywords))
            {
                co_return failure(
                    changed ? HistoryExecutionOutcome::PartialFailure
                            : HistoryExecutionOutcome::Conflict,
                    i18n("The transferred destination message changed on another client; Undo will "
                         "not destroy it."),
                    *history);
            }

            auto destroyed = co_await m_mail.applyExactEmailMutation(
                history->destinationAccountId,
                mutation(currentDestination->id, {}, {}, true, destination, *currentDestination));
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&destroyed))
                co_return errorResult(*error, *history, changed);
            if (!acceptedExactlyOne(std::get<javelin::jmap::SubmittedEmailMutations>(destroyed)))
                co_return rejection(i18n("The destination server rejected removing the transferred "
                                         "message."),
                                    *history, changed);
            item.currentDestinationEmailId = std::nullopt;
            changed = true;
            ++item.redoGeneration;
        }

        co_return success(*history);
    }

} // namespace javelin::app::undo
