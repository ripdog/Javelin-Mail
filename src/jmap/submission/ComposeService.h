#pragma once

#include "jmap/JmapCore.h"
#include "jmap/submission/ComposeTypes.h"

#include <QCoroTask>

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

        [[nodiscard]] QCoro::Task<std::variant<DraftSnapshot, javelin::jmap::LiveRefreshError>>
        open(javelin::jmap::LiveConnectionSettings settings, OpenComposeRequest request);
        [[nodiscard]] QCoro::Task<std::variant<std::vector<javelin::jmap::domain::Identity>,
                                               javelin::jmap::LiveRefreshError>>
        loadSenderIdentities(javelin::jmap::LiveConnectionSettings settings, std::string accountId);
        [[nodiscard]] QCoro::Task<std::variant<DraftSaveSummary, javelin::jmap::LiveRefreshError>>
        saveDraft(javelin::jmap::LiveConnectionSettings settings, DraftSnapshot snapshot);
        [[nodiscard]] QCoro::Task<std::variant<SendSummary, javelin::jmap::LiveRefreshError>>
        send(javelin::jmap::LiveConnectionSettings settings, DraftSnapshot snapshot);
        [[nodiscard]] std::variant<std::optional<DraftSnapshot>, javelin::jmap::LiveRefreshError>
        loadWorkingCopy(std::string_view composeSessionId) const;
        [[nodiscard]] std::optional<javelin::jmap::LiveRefreshError>
        storeWorkingCopy(const DraftSnapshot& snapshot);
        [[nodiscard]] std::optional<javelin::jmap::LiveRefreshError>
        discard(std::string_view composeSessionId);

      private:
        javelin::jmap::cache::DatabaseConnection& m_connection;
        javelin::jmap::api::AbstractTransport& m_resourceTransport;
        javelin::jmap::api::JmapMethodTransport& m_methodTransport;
        javelin::jmap::JmapCore& m_jmapCore;
    };

} // namespace javelin::jmap::submission
