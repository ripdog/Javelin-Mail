#pragma once

#include "storage/sqlite/DatabaseConnection.h"

#include "jmap/JmapCore.h"
#include "jmap/submission/ComposeRevisionGate.h"
#include "jmap/submission/ComposeTypes.h"

#include <QCoroTask>

#include <chrono>
#include <functional>
#include <optional>
#include <variant>

namespace javelin::jmap::api
{
    class AbstractTransport;
    class JmapMethodTransport;
} // namespace javelin::jmap::api

namespace javelin::jmap::cache
{
    class ComposeSessionRepository;
    class DatabaseConnection;
} // namespace javelin::jmap::cache

namespace javelin::jmap::submission
{

    class ComposeService
    {
      public:
        ComposeService(javelin::jmap::cache::DatabaseConnection& connection,
                       javelin::jmap::api::AbstractTransport& resourceTransport,
                       javelin::jmap::api::JmapMethodTransport& methodTransport,
                       javelin::jmap::JmapCore& jmapCore);

        [[nodiscard]] QCoro::Task<std::variant<DraftSnapshot, javelin::jmap::OperationError>>
        open(javelin::jmap::LiveConnectionSettings settings, OpenComposeRequest request);
        [[nodiscard]] QCoro::Task<std::variant<std::vector<javelin::jmap::domain::Identity>,
                                               javelin::jmap::OperationError>>
        loadSenderIdentities(javelin::jmap::LiveConnectionSettings settings, std::string accountId);
        [[nodiscard]] QCoro::Task<std::variant<DraftSaveSummary, javelin::jmap::OperationError>>
        saveDraft(javelin::jmap::LiveConnectionSettings settings, DraftSnapshot snapshot,
                  std::optional<std::string> operationGroupId = std::nullopt);
        [[nodiscard]] QCoro::Task<std::variant<DraftSnapshot, javelin::jmap::OperationError>>
        loadAuthoritativeDraft(javelin::jmap::LiveConnectionSettings settings,
                               std::string accountId, std::string draftEmailId,
                               std::string composeSessionId);
        [[nodiscard]] QCoro::Task<std::variant<DraftDeleteSummary, javelin::jmap::OperationError>>
        deleteDraft(javelin::jmap::LiveConnectionSettings settings, std::string accountId,
                    std::string draftEmailId, std::string operationGroupId);
        [[nodiscard]] QCoro::Task<std::variant<SendSummary, javelin::jmap::OperationError>>
        send(javelin::jmap::LiveConnectionSettings settings, DraftSnapshot snapshot);
        [[nodiscard]] QCoro::Task<std::variant<PreparedSend, javelin::jmap::OperationError>>
        prepareSend(javelin::jmap::LiveConnectionSettings settings, DraftSnapshot snapshot);
        [[nodiscard]] QCoro::Task<std::variant<SendSummary, javelin::jmap::OperationError>>
        submitPreparedSend(javelin::jmap::LiveConnectionSettings settings, PreparedSend prepared,
                           std::function<void()> dispatched = {});
        [[nodiscard]] QCoro::Task<std::variant<SendSummary, javelin::jmap::OperationError>>
        submitPreparedSendAt(javelin::jmap::LiveConnectionSettings settings, PreparedSend prepared,
                             std::chrono::system_clock::time_point sendAt,
                             std::function<void()> dispatched = {});
        [[nodiscard]] std::optional<javelin::jmap::OperationError>
        validateScheduledSend(std::string_view accountId,
                              std::chrono::system_clock::time_point sendAt) const;
        [[nodiscard]] std::variant<std::optional<DraftSnapshot>, javelin::jmap::OperationError>
        loadWorkingCopy(std::string_view composeSessionId) const;
        [[nodiscard]] std::optional<javelin::jmap::OperationError>
        storeWorkingCopy(const DraftSnapshot& snapshot);
        [[nodiscard]] std::optional<javelin::jmap::OperationError>
        discard(std::string_view composeSessionId);

      private:
        [[nodiscard]] QCoro::Task<std::variant<SendSummary, javelin::jmap::OperationError>>
        submitPreparedSendImpl(javelin::jmap::LiveConnectionSettings settings,
                               PreparedSend prepared,
                               std::optional<std::chrono::system_clock::time_point> sendAt,
                               std::function<void()> dispatched);

        javelin::jmap::cache::DatabaseConnection& m_connection;
        javelin::jmap::api::AbstractTransport& m_resourceTransport;
        javelin::jmap::api::JmapMethodTransport& m_methodTransport;
        javelin::jmap::JmapCore& m_jmapCore;
        ComposeRevisionGate m_revisionGate;
    };

} // namespace javelin::jmap::submission
