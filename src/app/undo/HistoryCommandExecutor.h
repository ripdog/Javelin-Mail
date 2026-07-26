#pragma once

#include "app/undo/HistoryTypes.h"

#include <QCoroTask>

#include <QStringList>

#include <optional>
#include <vector>

namespace javelin::app::undo
{

    enum class HistoryExecutionDirection
    {
        Undo,
        Redo,
        Recover,
    };

    enum class HistoryExecutionOutcome
    {
        Success,
        Conflict,
        DefinitiveFailure,
        Unknown,
        PartialFailure,
        Impossible,
        Expired,
    };

    struct HistoryRefreshScope
    {
        QStringList accountIds;
        QStringList objectTypes;
        QStringList views;
    };

    struct HistoryExecutionResult
    {
        HistoryExecutionOutcome outcome = HistoryExecutionOutcome::DefinitiveFailure;
        std::optional<HistoryPayload> updatedPayload;
        HistoryRefreshScope refreshScope;
        QString summary;
        std::vector<HistoryObjectFailure> objectFailures;
        bool mayRemoveFromHistory = false;
    };

    class HistoryCommandExecutor
    {
      public:
        virtual ~HistoryCommandExecutor() = default;

        [[nodiscard]] virtual QCoro::Task<HistoryExecutionResult>
        execute(HistoryEntry entry, HistoryExecutionDirection direction) = 0;
    };

} // namespace javelin::app::undo
