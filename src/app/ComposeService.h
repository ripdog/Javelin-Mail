#pragma once

#include "app/AccountConnectionSettings.h"
#include "jmap/JmapCore.h"
#include "jmap/submission/ComposeTypes.h"

#include <QCoroTask>

#include <optional>
#include <string>
#include <string_view>
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

    class ComposeService
    {
      public:
        ComposeService(javelin::jmap::submission::ComposeService& service,
                       ApplicationErrorCoordinator& errorCoordinator);

        [[nodiscard]] QCoro::Task<
            std::variant<javelin::jmap::submission::DraftSnapshot, javelin::jmap::OperationError>>
        open(AccountConnectionSettings settings,
             javelin::jmap::submission::OpenComposeRequest request);
        [[nodiscard]] QCoro::Task<std::variant<std::vector<javelin::jmap::domain::Identity>,
                                               javelin::jmap::OperationError>>
        loadSenderIdentities(AccountConnectionSettings settings, std::string accountId);
        [[nodiscard]] QCoro::Task<std::variant<javelin::jmap::submission::DraftSaveSummary,
                                               javelin::jmap::OperationError>>
        saveDraft(AccountConnectionSettings settings,
                  javelin::jmap::submission::DraftSnapshot snapshot);
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

      private:
        javelin::jmap::submission::ComposeService& m_service;
        ApplicationErrorCoordinator& m_errorCoordinator;
    };

} // namespace javelin::app
