#include "app/undo/SieveHistoryExecutor.h"

#include <algorithm>
#include <ranges>
#include <utility>

namespace javelin::app::undo
{
    namespace
    {
        [[nodiscard]] HistoryExecutionOutcome outcomeFor(const javelin::jmap::OperationError& error)
        {
            using enum javelin::jmap::OperationErrorCode;
            if (error.code == Conflict || error.code == PreconditionFailed ||
                error.code == NotFound)
                return HistoryExecutionOutcome::Conflict;
            if (javelin::jmap::isTransientError(error))
                return HistoryExecutionOutcome::Unknown;
            return HistoryExecutionOutcome::DefinitiveFailure;
        }

        [[nodiscard]] HistoryExecutionResult failure(const javelin::jmap::OperationError& error)
        {
            return {
                .outcome = outcomeFor(error),
                .updatedPayload = std::nullopt,
                .refreshScope = {},
                .summary = error.message,
                .objectFailures = {},
                .mayRemoveFromHistory = !javelin::jmap::isTransientError(error),
            };
        }

        [[nodiscard]] HistoryExecutionResult conflict(QString message)
        {
            return {
                .outcome = HistoryExecutionOutcome::Conflict,
                .updatedPayload = std::nullopt,
                .refreshScope = {},
                .summary = std::move(message),
                .objectFailures = {},
                .mayRemoveFromHistory = true,
            };
        }

        [[nodiscard]] HistoryExecutionResult success(SieveHistory history)
        {
            const auto accountId = QString::fromStdString(history.accountId);
            return {
                .outcome = HistoryExecutionOutcome::Success,
                .updatedPayload = std::move(history),
                .refreshScope =
                    {
                        .accountIds = {accountId},
                        .objectTypes = {QStringLiteral("SieveScript")},
                        .views = {QStringLiteral("sieve")},
                    },
                .summary = {},
                .objectFailures = {},
                .mayRemoveFromHistory = false,
            };
        }

        [[nodiscard]] const javelin::jmap::sieve::SieveScript*
        findScript(const std::vector<javelin::jmap::sieve::SieveScript>& scripts,
                   const std::optional<std::string>& id)
        {
            if (!id.has_value())
                return nullptr;
            const auto found =
                std::ranges::find(scripts, *id, &javelin::jmap::sieve::SieveScript::id);
            return found == scripts.end() ? nullptr : &*found;
        }
    } // namespace

    SieveHistoryExecutor::SieveHistoryExecutor(SieveHistoryPort& mailService)
        : m_mailService(mailService)
    {
    }

    QCoro::Task<HistoryExecutionResult>
    SieveHistoryExecutor::execute(HistoryEntry entry, const HistoryExecutionDirection direction)
    {
        auto* history = std::get_if<SieveHistory>(&entry.payload);
        if (history == nullptr || direction == HistoryExecutionDirection::Recover)
        {
            co_return conflict(QStringLiteral(
                "The previous Sieve operation requires authoritative reconciliation."));
        }

        auto listed = co_await m_mailService.requestSieveScripts(history->accountId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&listed))
            co_return failure(*error);
        const auto& scripts = std::get<std::vector<javelin::jmap::sieve::SieveScript>>(listed);

        const bool undo = direction == HistoryExecutionDirection::Undo;
        const auto& expectedContent = undo ? history->afterContent : history->beforeContent;
        const auto& desiredContent = undo ? history->beforeContent : history->afterContent;
        const auto& expectedName = undo ? history->afterName : history->beforeName;
        const auto& desiredName = undo ? history->beforeName : history->afterName;
        const auto& expectedActive =
            undo ? history->activeScriptIdAfter : history->activeScriptIdBefore;
        const auto& desiredActive =
            undo ? history->activeScriptIdBefore : history->activeScriptIdAfter;

        const bool activationOnly = !history->beforeContent.has_value() &&
                                    !history->afterContent.has_value() &&
                                    (history->activeScriptIdBefore != history->activeScriptIdAfter);
        if (activationOnly)
        {
            const auto active =
                std::ranges::find(scripts, true, &javelin::jmap::sieve::SieveScript::isActive);
            const std::optional<std::string> currentActive =
                active == scripts.end() ? std::nullopt : std::optional{active->id};
            if (currentActive != expectedActive)
                co_return conflict(
                    QStringLiteral("The active Sieve script changed on another client."));

            if (desiredActive.has_value())
            {
                const auto* desired = findScript(scripts, desiredActive);
                if (desired == nullptr)
                    co_return conflict(
                        QStringLiteral("The Sieve script to restore is no longer available."));
                auto changed = co_await m_mailService.setSieveScriptActive(
                    history->accountId, *desired, true,
                    undo ? CommandOrigin::Undo : CommandOrigin::Redo);
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&changed))
                    co_return failure(*error);
            }
            else if (active != scripts.end())
            {
                auto changed = co_await m_mailService.setSieveScriptActive(
                    history->accountId, *active, false,
                    undo ? CommandOrigin::Undo : CommandOrigin::Redo);
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&changed))
                    co_return failure(*error);
            }
            co_return success(*history);
        }

        const auto* current = findScript(scripts, history->currentScriptId);
        if (expectedContent.has_value())
        {
            if (current == nullptr || (expectedName.has_value() && current->name != *expectedName))
                co_return conflict(
                    QStringLiteral("The Sieve script changed or was deleted on another client."));
            auto loaded = co_await m_mailService.requestSieveScript(history->accountId, *current);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&loaded))
                co_return failure(*error);
            if (std::get<QByteArray>(loaded).toStdString() != *expectedContent)
                co_return conflict(
                    QStringLiteral("The Sieve script content changed on another client."));
        }
        else if (current != nullptr)
        {
            co_return conflict(
                QStringLiteral("A Sieve script with this history identity already exists."));
        }

        if (desiredContent.has_value())
        {
            javelin::jmap::sieve::SieveScript target;
            if (current != nullptr)
                target = *current;
            else
                target.name = desiredName.value_or(std::string{});
            auto saved = co_await m_mailService.saveSieveScript(
                history->accountId, std::move(target), QByteArray::fromStdString(*desiredContent),
                undo ? CommandOrigin::Undo : CommandOrigin::Redo);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&saved))
                co_return failure(*error);
            const auto& script = std::get<javelin::jmap::sieve::SieveScript>(saved);
            history->currentScriptId = script.id;
            if (desiredActive == script.id && !script.isActive)
            {
                auto activated = co_await m_mailService.setSieveScriptActive(
                    history->accountId, script, true,
                    undo ? CommandOrigin::Undo : CommandOrigin::Redo);
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&activated))
                    co_return failure(*error);
            }
        }
        else
        {
            auto removed = co_await m_mailService.deleteSieveScript(
                history->accountId, *current, undo ? CommandOrigin::Undo : CommandOrigin::Redo);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&removed))
                co_return failure(*error);
            history->currentScriptId = std::nullopt;
        }

        co_return success(*history);
    }

} // namespace javelin::app::undo
