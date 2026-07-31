#pragma once

#include "app/ComposeApplicationPorts.h"

namespace javelin::app
{
    class ComposeService;

    class ComposeCommandService final : public ComposeCommandPort
    {
      public:
        explicit ComposeCommandService(ComposeService& service);

        [[nodiscard]] QCoro::Task<
            std::variant<javelin::jmap::submission::DraftSnapshot, javelin::jmap::OperationError>>
        open(AccountConnectionSettings settings,
             javelin::jmap::submission::OpenComposeRequest request) override;
        [[nodiscard]] QCoro::Task<std::variant<std::vector<javelin::jmap::domain::Identity>,
                                               javelin::jmap::OperationError>>
        loadSenderIdentities(AccountConnectionSettings settings, std::string accountId) override;
        [[nodiscard]] QCoro::Task<std::variant<javelin::jmap::submission::DraftSaveSummary,
                                               javelin::jmap::OperationError>>
        saveDraft(AccountConnectionSettings settings,
                  javelin::jmap::submission::DraftSnapshot snapshot) override;
        [[nodiscard]] QCoro::Task<
            std::variant<javelin::jmap::submission::SendSummary, javelin::jmap::OperationError>>
        send(AccountConnectionSettings settings,
             javelin::jmap::submission::DraftSnapshot snapshot) override;
        [[nodiscard]] std::variant<std::optional<javelin::jmap::submission::DraftSnapshot>,
                                   javelin::jmap::OperationError>
        loadWorkingCopy(std::string_view composeSessionId) const override;
        [[nodiscard]] std::optional<javelin::jmap::OperationError>
        storeWorkingCopy(const javelin::jmap::submission::DraftSnapshot& snapshot) override;
        [[nodiscard]] std::optional<javelin::jmap::OperationError>
        discard(std::string_view composeSessionId) override;

      private:
        ComposeService& m_service;
    };

} // namespace javelin::app
