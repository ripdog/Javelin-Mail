#pragma once

#include "app/AccountConnectionSettings.h"
#include "app/undo/DraftHistoryPort.h"
#include "jmap/JmapCore.h"
#include "jmap/submission/ComposeTypes.h"

#include <QCoroTask>

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace javelin::jmap::domain
{
    struct Identity;
}

namespace javelin::jmap::submission
{
    class ComposeService;
}

namespace javelin::app
{
    class ApplicationErrorCoordinator;
    class AccountConnectionProvider;
    class DeferredSendService;
    class WorkScheduler;
    namespace undo
    {
        class UndoManager;
    }

    class ComposeService final : public javelin::app::undo::DraftHistoryPort
    {
      public:
        ComposeService(javelin::jmap::submission::ComposeService& service,
                       ApplicationErrorCoordinator& errorCoordinator, WorkScheduler& workScheduler,
                       AccountConnectionProvider& connectionProvider,
                       javelin::app::undo::UndoManager& undoManager,
                       DeferredSendService& deferredSendService);

        [[nodiscard]] QCoro::Task<
            std::variant<javelin::jmap::submission::DraftSnapshot, javelin::jmap::OperationError>>
        open(AccountConnectionSettings settings,
             javelin::jmap::submission::OpenComposeRequest request);
        [[nodiscard]] QCoro::Task<std::variant<std::vector<javelin::jmap::domain::Identity>,
                                               javelin::jmap::OperationError>>
        loadSenderIdentities(AccountConnectionSettings settings, std::string accountId);
        [[nodiscard]] QCoro::Task<std::variant<javelin::jmap::submission::DraftSaveSummary,
                                               javelin::jmap::OperationError>>
        saveDraft(
            AccountConnectionSettings settings, javelin::jmap::submission::DraftSnapshot snapshot,
            javelin::app::undo::CommandOrigin origin = javelin::app::undo::CommandOrigin::User);
        [[nodiscard]] QCoro::Task<
            std::variant<javelin::jmap::submission::SendSummary, javelin::jmap::OperationError>>
        send(AccountConnectionSettings settings, javelin::jmap::submission::DraftSnapshot snapshot);
        [[nodiscard]] std::variant<std::optional<javelin::jmap::submission::DraftSnapshot>,
                                   javelin::jmap::OperationError>
        loadWorkingCopy(std::string_view composeSessionId) const;
        [[nodiscard]] std::optional<javelin::jmap::OperationError>
        storeWorkingCopy(const javelin::jmap::submission::DraftSnapshot& snapshot);
        [[nodiscard]] std::optional<javelin::jmap::OperationError>
        discard(std::string_view composeSessionId);
        [[nodiscard]] QCoro::Task<
            std::variant<javelin::jmap::submission::DraftSnapshot, javelin::jmap::OperationError>>
        loadAuthoritativeDraft(std::string accountId, std::string draftEmailId,
                               std::string composeSessionId) override;
        [[nodiscard]] QCoro::Task<std::variant<javelin::jmap::submission::DraftSaveSummary,
                                               javelin::jmap::OperationError>>
        saveDraftFromHistory(javelin::jmap::submission::DraftSnapshot snapshot,
                             javelin::app::undo::CommandOrigin origin) override;
        [[nodiscard]] QCoro::Task<std::variant<javelin::jmap::submission::DraftDeleteSummary,
                                               javelin::jmap::OperationError>>
        deleteDraftFromHistory(std::string accountId, std::string draftEmailId,
                               javelin::app::undo::CommandOrigin origin) override;

      private:
        javelin::jmap::submission::ComposeService& m_service;
        ApplicationErrorCoordinator& m_errorCoordinator;
        WorkScheduler& m_workScheduler;
        AccountConnectionProvider& m_connectionProvider;
        javelin::app::undo::UndoManager& m_undoManager;
        DeferredSendService& m_deferredSendService;
        std::unordered_map<std::string, javelin::jmap::submission::DraftSnapshot>
            m_lastSavedSnapshots;
    };

} // namespace javelin::app
