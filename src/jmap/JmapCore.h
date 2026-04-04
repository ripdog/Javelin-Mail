#pragma once

#include <QCoroTask>

#include <QString>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace javelin::jmap::cache
{
    class DatabaseConnection;
}

namespace javelin::jmap::api
{
    class AbstractTransport;
}

namespace javelin::jmap
{

    struct LiveConnectionSettings
    {
        std::string sessionUrl;
        std::string loginEmail;
        std::string apiKey;
    };

    struct LiveRefreshSummary
    {
        std::string accountId;
        std::optional<std::string> selectedMailboxId;
        std::size_t mailboxCount = 0;
        std::size_t emailCount = 0;
    };

    struct LiveRefreshError
    {
        QString message;
    };

    using LiveRefreshResult = std::variant<LiveRefreshSummary, LiveRefreshError>;

    class JmapCore
    {
      public:
        JmapCore();
        JmapCore(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                 javelin::jmap::api::AbstractTransport& transport);
        ~JmapCore();

        JmapCore(const JmapCore&) = delete;
        JmapCore& operator=(const JmapCore&) = delete;
        JmapCore(JmapCore&&) = delete;
        JmapCore& operator=(JmapCore&&) = delete;

        [[nodiscard]] QString statusSummary() const;
        [[nodiscard]] QCoro::Task<LiveRefreshResult>
        refreshFromServer(const LiveConnectionSettings& settings);

      private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };

} // namespace javelin::jmap
