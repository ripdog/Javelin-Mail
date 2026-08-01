#pragma once

#include "app/undo/HistoryCommandExecutor.h"
#include "app/undo/HistoryTypes.h"

#include <optional>

namespace javelin::app
{
    struct RemoteUndoExecutionResult
    {
        bool succeeded = false;
        std::optional<QString> completedEntryId;
        std::optional<undo::HistoryRefreshScope> refreshScope;
        std::optional<undo::HistoryFailure> failure;
    };
} // namespace javelin::app
