#pragma once

#include "jmap/OperationError.h"
#include "jmap/api/LiveConnectionSettings.h"

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
    struct MailCapabilityContext;

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
        std::variant<MessageContentRefreshSummary, MessageContentUnavailable, OperationError>;

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

    using AttachmentDownloadResult = std::variant<AttachmentDownload, OperationError>;

    struct MessageSourceDownload
    {
        std::string accountId;
        std::string emailId;
        std::string blobId;
        std::optional<std::string> subject;
        QByteArray payload;
    };

    using MessageSourceDownloadResult = std::variant<MessageSourceDownload, OperationError>;

    class MessageContentClient
    {
      public:
        MessageContentClient(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                             javelin::jmap::api::AbstractTransport& resourceTransport);
        ~MessageContentClient();

        [[nodiscard]] QCoro::Task<MessageContentRefreshResult>
        refresh(LiveConnectionSettings settings, std::string accountId, std::string emailId,
                std::function<void(const QString&)> progressCallback = {});
        [[nodiscard]] QCoro::Task<AttachmentDownloadResult>
        loadAttachment(std::string accountId, std::string emailId, std::string partId);
        [[nodiscard]] QCoro::Task<MessageSourceDownloadResult>
        loadCachedSource(std::string accountId, std::string emailId);

      private:
        std::unique_ptr<MailCapabilityContext> m_impl;
    };

} // namespace javelin::jmap
