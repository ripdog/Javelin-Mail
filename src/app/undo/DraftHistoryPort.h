#pragma once

#include "app/undo/HistoryTypes.h"
#include "jmap/submission/ComposeService.h"

namespace javelin::app::undo
{

    class DraftHistoryPort
    {
      public:
        virtual ~DraftHistoryPort() = default;

        [[nodiscard]] virtual QCoro::Task<
            std::variant<javelin::jmap::submission::DraftSnapshot, javelin::jmap::OperationError>>
        loadAuthoritativeDraft(std::string accountId, std::string draftEmailId,
                               std::string composeSessionId) = 0;
        [[nodiscard]] virtual QCoro::Task<std::variant<javelin::jmap::submission::DraftSaveSummary,
                                                       javelin::jmap::OperationError>>
        saveDraftFromHistory(javelin::jmap::submission::DraftSnapshot snapshot,
                             CommandOrigin origin) = 0;
        [[nodiscard]] virtual QCoro::Task<std::variant<
            javelin::jmap::submission::DraftDeleteSummary, javelin::jmap::OperationError>>
        deleteDraftFromHistory(std::string accountId, std::string draftEmailId,
                               CommandOrigin origin) = 0;
    };

} // namespace javelin::app::undo
