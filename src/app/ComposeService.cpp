#include "app/ComposeService.h"

#include "jmap/submission/ComposeService.h"

namespace javelin::app
{

    namespace
    {
        [[nodiscard]] javelin::jmap::LiveConnectionSettings
        toLiveConnectionSettings(AccountConnectionSettings settings)
        {
            return javelin::jmap::LiveConnectionSettings{
                .sessionUrl = std::move(settings.sessionUrl),
                .loginEmail = std::move(settings.loginEmail),
                .apiKey = std::move(settings.apiKey),
            };
        }
    } // namespace

    ComposeService::ComposeService(javelin::jmap::submission::ComposeService& service)
        : m_service(service)
    {
    }

    QCoro::Task<
        std::variant<javelin::jmap::submission::DraftSnapshot, javelin::jmap::LiveRefreshError>>
    ComposeService::open(AccountConnectionSettings settings,
                         javelin::jmap::submission::OpenComposeRequest request)
    {
        co_return co_await m_service.open(toLiveConnectionSettings(std::move(settings)),
                                          std::move(request));
    }

    QCoro::Task<
        std::variant<std::vector<javelin::jmap::domain::Identity>, javelin::jmap::LiveRefreshError>>
    ComposeService::loadSenderIdentities(AccountConnectionSettings settings, std::string accountId)
    {
        co_return co_await m_service.loadSenderIdentities(
            toLiveConnectionSettings(std::move(settings)), std::move(accountId));
    }

    QCoro::Task<
        std::variant<javelin::jmap::submission::DraftSaveSummary, javelin::jmap::LiveRefreshError>>
    ComposeService::saveDraft(AccountConnectionSettings settings,
                              javelin::jmap::submission::DraftSnapshot snapshot)
    {
        co_return co_await m_service.saveDraft(toLiveConnectionSettings(std::move(settings)),
                                               std::move(snapshot));
    }

    QCoro::Task<
        std::variant<javelin::jmap::submission::SendSummary, javelin::jmap::LiveRefreshError>>
    ComposeService::send(AccountConnectionSettings settings,
                         javelin::jmap::submission::DraftSnapshot snapshot)
    {
        co_return co_await m_service.send(toLiveConnectionSettings(std::move(settings)),
                                          std::move(snapshot));
    }

    std::variant<std::optional<javelin::jmap::submission::DraftSnapshot>,
                 javelin::jmap::LiveRefreshError>
    ComposeService::loadWorkingCopy(const std::string_view composeSessionId) const
    {
        return m_service.loadWorkingCopy(composeSessionId);
    }

    std::optional<javelin::jmap::LiveRefreshError>
    ComposeService::storeWorkingCopy(const javelin::jmap::submission::DraftSnapshot& snapshot)
    {
        return m_service.storeWorkingCopy(snapshot);
    }

    std::optional<javelin::jmap::LiveRefreshError>
    ComposeService::discard(const std::string_view composeSessionId)
    {
        return m_service.discard(composeSessionId);
    }

} // namespace javelin::app
