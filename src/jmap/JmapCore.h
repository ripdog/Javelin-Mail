#pragma once

#include "jmap/cache/QueryService.h"
#include "jmap/query/EmailListSort.h"
#include "jmap/search/EmailSearch.h"

#include <QCoroTask>

#include <QByteArray>
#include <QString>

#include <cstddef>
#include <functional>
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
        std::string resolvedSessionUrl;
    };

    struct LiveRefreshError
    {
        QString message;
        bool requiresUserIntervention = false;
    };

    using LiveRefreshResult = std::variant<LiveRefreshSummary, LiveRefreshError>;

    struct MessageContentRefreshSummary
    {
        std::string accountId;
        std::string emailId;
        std::size_t partCount = 0;
        std::size_t bodyValueCount = 0;
        bool usedCachedContent = false;
    };

    struct MessageContentUnavailable
    {
        std::string accountId;
        std::string emailId;
        QString message;
    };

    using MessageContentRefreshResult =
        std::variant<MessageContentRefreshSummary, MessageContentUnavailable, LiveRefreshError>;

    struct MailboxMessagesRefreshSummary
    {
        std::string accountId;
        std::string mailboxId;
        std::size_t emailCount = 0;
    };

    using MailboxMessagesRefreshResult =
        std::variant<MailboxMessagesRefreshSummary, LiveRefreshError>;

    struct MessageSearchSummary
    {
        std::string accountId;
        std::string query;
        std::size_t offset = 0;
        std::size_t limit = 0;
        std::size_t representativeCount = 0;
        std::optional<std::size_t> total;
        std::vector<javelin::jmap::cache::MessageListItem> results;
    };

    using MessageSearchResult = std::variant<MessageSearchSummary, LiveRefreshError>;

    struct MailboxPageSummary
    {
        std::string accountId;
        std::string mailboxId;
        std::size_t offset = 0;
        std::size_t limit = 0;
        std::size_t representativeCount = 0;
        std::optional<std::size_t> total;
        std::vector<javelin::jmap::cache::MessageListItem> results;
    };

    using MailboxPageResult = std::variant<MailboxPageSummary, LiveRefreshError>;

    struct QueuedEmailMutation
    {
        std::string pendingActionId;
        std::string accountId;
        std::string emailId;
    };

    using QueuedEmailMutationResult = std::variant<QueuedEmailMutation, LiveRefreshError>;

    struct SubmittedEmailMutations
    {
        std::string accountId;
        std::size_t attemptedEmailCount = 0;
        std::size_t updatedEmailCount = 0;
        std::size_t failedEmailCount = 0;
    };

    using SubmittedEmailMutationsResult = std::variant<SubmittedEmailMutations, LiveRefreshError>;

    struct AttachmentDownload
    {
        std::string accountId;
        std::string emailId;
        std::string partId;
        std::optional<std::string> name;
        std::string mediaType;
        QByteArray payload;
        bool usedCachedInlinePayload = false;
    };

    using AttachmentDownloadResult = std::variant<AttachmentDownload, LiveRefreshError>;

    struct MessageSourceDownload
    {
        std::string accountId;
        std::string emailId;
        std::string blobId;
        std::optional<std::string> subject;
        QByteArray payload;
    };

    using MessageSourceDownloadResult = std::variant<MessageSourceDownload, LiveRefreshError>;

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
        refreshFromServer(LiveConnectionSettings settings,
                          std::function<void(const QString&)> progressCallback = {});
        [[nodiscard]] QCoro::Task<MessageContentRefreshResult>
        refreshMessageContent(LiveConnectionSettings settings, std::string accountId,
                              std::string emailId,
                              std::function<void(const QString&)> progressCallback = {});
        [[nodiscard]] QCoro::Task<MailboxMessagesRefreshResult>
        refreshMailboxMessages(LiveConnectionSettings settings, std::string accountId,
                               std::string mailboxId,
                               std::function<void(const QString&)> progressCallback = {});
        [[nodiscard]] QCoro::Task<MailboxPageResult>
        queryMailboxPage(LiveConnectionSettings settings, std::string accountId,
                         std::string mailboxId, std::size_t offset = 0, std::size_t limit = 100,
                         javelin::jmap::query::EmailListSort sort = {},
                         std::function<void(const QString&)> progressCallback = {});
        [[nodiscard]] QCoro::Task<MessageSearchResult>
        searchMessages(LiveConnectionSettings settings, std::string accountId, std::string query,
                       std::size_t offset = 0, std::size_t limit = 100,
                       javelin::jmap::query::EmailListSort sort = {},
                       std::function<void(const QString&)> progressCallback = {});
        [[nodiscard]] QCoro::Task<MessageSearchResult>
        searchMessages(LiveConnectionSettings settings, std::string accountId,
                       javelin::jmap::search::EmailSearchCriteria criteria, std::size_t offset = 0,
                       std::size_t limit = 100, javelin::jmap::query::EmailListSort sort = {},
                       std::function<void(const QString&)> progressCallback = {});
        [[nodiscard]] QCoro::Task<AttachmentDownloadResult>
        downloadAttachment(LiveConnectionSettings settings, std::string accountId,
                           std::string emailId, std::string partId);
        [[nodiscard]] QCoro::Task<MessageSourceDownloadResult>
        downloadMessageSource(LiveConnectionSettings settings, std::string accountId,
                              std::string emailId);
        [[nodiscard]] QueuedEmailMutationResult queueMoveEmail(std::string accountId,
                                                               std::string emailId,
                                                               std::string sourceMailboxId,
                                                               std::string destinationMailboxId);
        [[nodiscard]] QueuedEmailMutationResult queueCopyEmail(std::string accountId,
                                                               std::string emailId,
                                                               std::string sourceMailboxId,
                                                               std::string destinationMailboxId);
        [[nodiscard]] QueuedEmailMutationResult queueArchiveEmail(std::string accountId,
                                                                  std::string emailId,
                                                                  std::string sourceMailboxId,
                                                                  std::string archiveMailboxId);
        [[nodiscard]] QueuedEmailMutationResult queueDeleteEmail(std::string accountId,
                                                                 std::string emailId,
                                                                 std::string sourceMailboxId,
                                                                 std::string trashMailboxId);
        [[nodiscard]] QueuedEmailMutationResult queueDestroyEmail(std::string accountId,
                                                                  std::string emailId);
        [[nodiscard]] QueuedEmailMutationResult queueMarkEmailRead(std::string accountId,
                                                                   std::string emailId);
        [[nodiscard]] QueuedEmailMutationResult queueMarkEmailUnread(std::string accountId,
                                                                     std::string emailId);
        [[nodiscard]] QueuedEmailMutationResult
        queueSetEmailFlagged(std::string accountId, std::string emailId, bool flagged);
        [[nodiscard]] QCoro::Task<SubmittedEmailMutationsResult>
        submitPendingEmailMutations(LiveConnectionSettings settings, std::string accountId,
                                    std::size_t limit = 25);

      private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };

} // namespace javelin::jmap
