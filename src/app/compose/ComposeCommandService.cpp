#include "app/ComposeCommandService.h"

#include "app/ComposeService.h"

#include <utility>

namespace javelin::app
{
    ComposeCommandService::ComposeCommandService(ComposeService& service) : m_service(service)
    {
    }

    QCoro::Task<
        std::variant<javelin::jmap::submission::DraftSnapshot, javelin::jmap::OperationError>>
    ComposeCommandService::open(AccountConnectionSettings settings,
                                javelin::jmap::submission::OpenComposeRequest request)
    {
        return m_service.open(std::move(settings), std::move(request));
    }

    QCoro::Task<
        std::variant<std::vector<javelin::jmap::domain::Identity>, javelin::jmap::OperationError>>
    ComposeCommandService::loadSenderIdentities(AccountConnectionSettings settings,
                                                std::string accountId)
    {
        return m_service.loadSenderIdentities(std::move(settings), std::move(accountId));
    }

    QCoro::Task<
        std::variant<javelin::jmap::submission::DraftSaveSummary, javelin::jmap::OperationError>>
    ComposeCommandService::saveDraft(AccountConnectionSettings settings,
                                     javelin::jmap::submission::DraftSnapshot snapshot)
    {
        return m_service.saveDraft(std::move(settings), std::move(snapshot));
    }

    QCoro::Task<std::variant<javelin::jmap::submission::SendSummary, javelin::jmap::OperationError>>
    ComposeCommandService::send(AccountConnectionSettings settings,
                                javelin::jmap::submission::DraftSnapshot snapshot)
    {
        return m_service.send(std::move(settings), std::move(snapshot));
    }

    QCoro::Task<std::variant<javelin::jmap::submission::SendSummary, javelin::jmap::OperationError>>
    ComposeCommandService::scheduleSend(AccountConnectionSettings settings,
                                        javelin::jmap::submission::ScheduledSendRequest request)
    {
        return m_service.scheduleSend(std::move(settings), std::move(request));
    }

    QCoro::Task<std::variant<bool, javelin::jmap::OperationError>>
    ComposeCommandService::cancelDeferredSend(QString sendId)
    {
        co_return m_service.cancelDeferredSend(sendId);
    }

    std::variant<std::optional<javelin::jmap::submission::DraftSnapshot>,
                 javelin::jmap::OperationError>
    ComposeCommandService::loadWorkingCopy(std::string_view composeSessionId) const
    {
        return m_service.loadWorkingCopy(composeSessionId);
    }

    std::optional<javelin::jmap::OperationError> ComposeCommandService::storeWorkingCopy(
        const javelin::jmap::submission::DraftSnapshot& snapshot)
    {
        return m_service.storeWorkingCopy(snapshot);
    }

    std::optional<javelin::jmap::OperationError>
    ComposeCommandService::discard(const std::string_view composeSessionId)
    {
        return m_service.discard(composeSessionId);
    }

} // namespace javelin::app
