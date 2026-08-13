#pragma once

#include "jmap/OperationError.h"
#include "jmap/api/LiveConnectionSettings.h"
#include "jmap/cache/MessageListReadTypes.h"
#include "jmap/query/EmailListSort.h"
#include "jmap/search/EmailSearch.h"

#include <QCoroTask>

#include <QString>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{
    class DatabaseConnection;
}

namespace javelin::jmap
{
    class MailQueryClient;
    struct MailCapabilityContext;

    struct MessageSearchSummary
    {
        std::string accountId;
        std::string query;
        std::size_t offset = 0;
        std::size_t limit = 0;
        std::size_t position = 0;
        std::size_t returnedLimit = 0;
        std::size_t representativeCount = 0;
        std::optional<std::size_t> total;
        std::string queryState;
        std::vector<javelin::jmap::cache::MessageListItem> results;
    };

    using MessageSearchResult = std::variant<MessageSearchSummary, OperationError>;

    struct MailboxPageSummary
    {
        std::string accountId;
        std::string mailboxId;
        std::size_t offset = 0;
        std::size_t limit = 0;
        std::size_t position = 0;
        std::size_t returnedLimit = 0;
        std::size_t representativeCount = 0;
        std::optional<std::size_t> total;
        std::string queryState;
        std::vector<javelin::jmap::cache::MessageListItem> results;
    };

    using MailboxPageResult = std::variant<MailboxPageSummary, OperationError>;

    class MailQueryMaterializer
    {
      public:
        MailQueryMaterializer(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                              MailQueryClient& queryClient);
        ~MailQueryMaterializer();

        [[nodiscard]] QCoro::Task<MailboxPageResult>
        queryMailboxPage(LiveConnectionSettings settings, std::string accountId,
                         std::string mailboxId, std::size_t offset = 0, std::size_t limit = 100,
                         javelin::jmap::query::EmailListSort sort = {},
                         std::optional<std::string> anchor = std::nullopt,
                         std::int64_t anchorOffset = 1,
                         std::function<void(const QString&)> progressCallback = {});
        [[nodiscard]] QCoro::Task<MessageSearchResult>
        searchMessages(LiveConnectionSettings settings, std::string accountId, std::string query,
                       std::size_t offset = 0, std::size_t limit = 100,
                       javelin::jmap::query::EmailListSort sort = {},
                       std::optional<std::string> anchor = std::nullopt,
                       std::optional<std::string> windowKey = std::nullopt,
                       std::function<void(const QString&)> progressCallback = {});
        [[nodiscard]] QCoro::Task<MessageSearchResult>
        searchMessages(LiveConnectionSettings settings, std::string accountId,
                       javelin::jmap::search::EmailSearchCriteria criteria, std::size_t offset = 0,
                       std::size_t limit = 100, javelin::jmap::query::EmailListSort sort = {},
                       std::optional<std::string> anchor = std::nullopt,
                       std::optional<std::string> windowKey = std::nullopt,
                       std::function<void(const QString&)> progressCallback = {},
                       javelin::jmap::search::EmailSearchResolution resolution = {});

      private:
        std::unique_ptr<MailCapabilityContext> m_impl;
    };

} // namespace javelin::jmap
