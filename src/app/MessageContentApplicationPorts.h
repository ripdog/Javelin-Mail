#pragma once

#include "app/MessageSelection.h"
#include "jmap/MessageContentClient.h"

#include <QCoroTask>

#include <QString>

#include <cstddef>
#include <optional>
#include <string>
#include <variant>

namespace javelin::app
{
    enum class MessageSaveTargetKind
    {
        SingleFile,
        Directory,
    };

    struct SaveMessagesIntent
    {
        std::string accountId;
        std::optional<std::string> sourceMailboxId;
        MessageSelection selection;
        MessageSaveTargetKind targetKind = MessageSaveTargetKind::SingleFile;
        QString destinationPath;
    };

    struct SaveMessagesSummary
    {
        std::size_t savedMessageCount = 0;
        QString destinationPath;
    };

    using SaveMessagesResult = std::variant<SaveMessagesSummary, javelin::jmap::OperationError>;

    class MessageContentPort
    {
      public:
        virtual ~MessageContentPort() = default;

        [[nodiscard]] virtual QCoro::Task<javelin::jmap::MessageContentRefreshResult>
        requestMessageContent(std::string accountId, std::string emailId) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::AttachmentDownloadResult>
        requestAttachment(std::string accountId, std::string emailId, std::string partId) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::MessageSourceDownloadResult>
        requestMessageSource(std::string accountId, std::string emailId) = 0;
        [[nodiscard]] virtual QCoro::Task<SaveMessagesResult>
        saveMessages(SaveMessagesIntent intent) = 0;
    };
} // namespace javelin::app
