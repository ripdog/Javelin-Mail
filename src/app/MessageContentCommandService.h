#pragma once

#include "app/MessageContentApplicationPorts.h"

namespace javelin::app
{
    class MailApplicationService;

    class MessageContentCommandService final : public MessageContentPort
    {
      public:
        explicit MessageContentCommandService(MailApplicationService& service);

        [[nodiscard]] QCoro::Task<javelin::jmap::MessageContentRefreshResult>
        requestMessageContent(std::string accountId, std::string emailId) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::AttachmentDownloadResult>
        requestAttachment(std::string accountId, std::string emailId, std::string partId) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::MessageSourceDownloadResult>
        requestMessageSource(std::string accountId, std::string emailId) override;

      private:
        MailApplicationService& m_service;
    };
} // namespace javelin::app
