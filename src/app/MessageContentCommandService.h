#pragma once

#include "app/MessageContentApplicationPorts.h"

namespace javelin::app
{
    class MessageContentApplicationService;

    class MessageContentCommandService final : public MessageContentPort
    {
      public:
        explicit MessageContentCommandService(MessageContentApplicationService& service);

        [[nodiscard]] QCoro::Task<javelin::jmap::MessageContentRefreshResult>
        requestMessageContent(std::string accountId, std::string emailId) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::AttachmentDownloadResult>
        requestAttachment(std::string accountId, std::string emailId, std::string partId) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::MessageSourceDownloadResult>
        requestMessageSource(std::string accountId, std::string emailId) override;
        [[nodiscard]] QCoro::Task<SaveMessagesResult>
        saveMessages(SaveMessagesIntent intent) override;

      private:
        MessageContentApplicationService& m_service;
    };
} // namespace javelin::app
