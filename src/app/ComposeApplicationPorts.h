#pragma once

#include "app/AccountConnectionSettings.h"
#include "jmap/OperationError.h"
#include "jmap/domain/MailEntities.h"
#include "jmap/submission/ComposeTypes.h"

#include <QCoroTask>

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::app
{

    class ComposeCommandPort
    {
      public:
        virtual ~ComposeCommandPort() = default;

        [[nodiscard]] virtual QCoro::Task<
            std::variant<javelin::jmap::submission::DraftSnapshot, javelin::jmap::OperationError>>
        open(AccountConnectionSettings settings,
             javelin::jmap::submission::OpenComposeRequest request) = 0;
        [[nodiscard]] virtual QCoro::Task<std::variant<std::vector<javelin::jmap::domain::Identity>,
                                                       javelin::jmap::OperationError>>
        loadSenderIdentities(AccountConnectionSettings settings, std::string accountId) = 0;
        [[nodiscard]] virtual QCoro::Task<std::variant<javelin::jmap::submission::DraftSaveSummary,
                                                       javelin::jmap::OperationError>>
        saveDraft(AccountConnectionSettings settings,
                  javelin::jmap::submission::DraftSnapshot snapshot) = 0;
        [[nodiscard]] virtual QCoro::Task<
            std::variant<javelin::jmap::submission::SendSummary, javelin::jmap::OperationError>>
        send(AccountConnectionSettings settings,
             javelin::jmap::submission::DraftSnapshot snapshot) = 0;
        [[nodiscard]] virtual std::variant<std::optional<javelin::jmap::submission::DraftSnapshot>,
                                           javelin::jmap::OperationError>
        loadWorkingCopy(std::string_view composeSessionId) const = 0;
        [[nodiscard]] virtual std::optional<javelin::jmap::OperationError>
        storeWorkingCopy(const javelin::jmap::submission::DraftSnapshot& snapshot) = 0;
        [[nodiscard]] virtual std::optional<javelin::jmap::OperationError>
        discard(std::string_view composeSessionId) = 0;
    };

} // namespace javelin::app
