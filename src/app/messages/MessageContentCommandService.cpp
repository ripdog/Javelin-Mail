#include "app/MessageContentCommandService.h"

#include "app/MessageContentApplicationService.h"

#include <utility>

namespace javelin::app
{
    MessageContentCommandService::MessageContentCommandService(
        MessageContentApplicationService& service)
        : m_service(service)
    {
    }

    QCoro::Task<javelin::jmap::MessageContentRefreshResult>
    MessageContentCommandService::requestMessageContent(std::string accountId, std::string emailId)
    {
        return m_service.requestMessageContent(std::move(accountId), std::move(emailId));
    }

    QCoro::Task<javelin::jmap::AttachmentDownloadResult>
    MessageContentCommandService::requestAttachment(std::string accountId, std::string emailId,
                                                    std::string partId)
    {
        return m_service.requestAttachment(std::move(accountId), std::move(emailId),
                                           std::move(partId));
    }

    QCoro::Task<javelin::jmap::MessageSourceDownloadResult>
    MessageContentCommandService::requestMessageSource(std::string accountId, std::string emailId)
    {
        return m_service.requestMessageSource(std::move(accountId), std::move(emailId));
    }
} // namespace javelin::app
