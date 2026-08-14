#pragma once

#include "jmap/MessageContentClient.h"

#include <QCoroTask>

#include <string>

namespace javelin::app
{
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
    };
} // namespace javelin::app
