#pragma once

#include "jmap/OperationError.h"
#include "jmap/api/LiveConnectionSettings.h"
#include "jmap/api/MailMethods.h"
#include "jmap/domain/MailEntities.h"
#include "jmap/query/EmailListSort.h"

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

namespace javelin::jmap::api
{
    class JmapMethodTransport;
}

namespace javelin::jmap
{
    struct MailCapabilityContext;

    struct FullMailboxPage
    {
        std::string accountId;
        std::string mailboxId;
        std::string queryState;
        std::size_t position = 0;
        std::optional<std::size_t> total;
        std::vector<std::string> emailIds;
        std::vector<javelin::jmap::domain::Email> emails;
        std::string emailState;
    };

    using FullMailboxPageResult = std::variant<FullMailboxPage, OperationError>;

    struct EmailIdQueryPage
    {
        std::string accountId;
        std::string queryState;
        std::optional<std::size_t> total;
        std::vector<std::string> emailIds;
    };

    using EmailIdQueryPageResult = std::variant<EmailIdQueryPage, OperationError>;

    struct CollapsedQueryPage
    {
        std::size_t representativeCount = 0;
        std::size_t position = 0;
        std::size_t returnedLimit = 0;
        std::optional<std::size_t> total;
        std::string queryState;
        std::string emailState;
        std::vector<std::string> representativeIds;
        std::vector<javelin::jmap::domain::Email> representatives;
    };

    using CollapsedQueryPageResult = std::variant<CollapsedQueryPage, OperationError>;

    class MailQueryClient
    {
      public:
        MailQueryClient(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                        javelin::jmap::api::JmapMethodTransport& methodTransport);
        ~MailQueryClient();

        [[nodiscard]] QCoro::Task<CollapsedQueryPageResult>
        queryCollapsedPage(LiveConnectionSettings settings, std::string accountId,
                           javelin::jmap::api::EmailQueryFilter filter, std::size_t offset,
                           std::size_t limit, javelin::jmap::query::EmailListSort sort,
                           std::optional<std::string> anchor = std::nullopt,
                           std::int64_t anchorOffset = 1,
                           std::function<void(const QString&)> progressCallback = {});
        [[nodiscard]] QCoro::Task<EmailIdQueryPageResult>
        queryEmailIdsByKeyword(LiveConnectionSettings settings, std::string accountId,
                               std::string keyword, std::size_t limit = 50);
        [[nodiscard]] QCoro::Task<FullMailboxPageResult>
        fetchFullMailboxPage(LiveConnectionSettings settings, std::string accountId,
                             std::string mailboxId, std::size_t position, std::size_t limit = 250,
                             std::optional<std::string> anchor = std::nullopt);

      private:
        std::unique_ptr<MailCapabilityContext> m_impl;
    };

} // namespace javelin::jmap
