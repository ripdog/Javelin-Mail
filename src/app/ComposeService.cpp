#include "app/ComposeService.h"
#include "app/ApplicationErrorCoordinator.h"

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

    ComposeService::ComposeService(javelin::jmap::submission::ComposeService& service,
                                   ApplicationErrorCoordinator& errorCoordinator)
        : m_service(service), m_errorCoordinator(errorCoordinator)
    {
    }

    QCoro::Task<
        std::variant<javelin::jmap::submission::DraftSnapshot, javelin::jmap::OperationError>>
    ComposeService::open(AccountConnectionSettings settings,
                         javelin::jmap::submission::OpenComposeRequest request)
    {
        const auto accountId = request.accountId;
        auto result =
            co_await m_service.open(toLiveConnectionSettings(settings), std::move(request));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
            m_errorCoordinator.reportFailure(settings, accountId, QStringLiteral("Open composer"),
                                             *error);
        else
            m_errorCoordinator.reportSuccess(settings.connectionId);
        co_return result;
    }

    QCoro::Task<
        std::variant<std::vector<javelin::jmap::domain::Identity>, javelin::jmap::OperationError>>
    ComposeService::loadSenderIdentities(AccountConnectionSettings settings, std::string accountId)
    {
        const auto reportedAccountId = accountId;
        auto result = co_await m_service.loadSenderIdentities(toLiveConnectionSettings(settings),
                                                              std::move(accountId));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
            m_errorCoordinator.reportFailure(settings, reportedAccountId,
                                             QStringLiteral("Load sender identities"), *error);
        else
            m_errorCoordinator.reportSuccess(settings.connectionId);
        co_return result;
    }

    QCoro::Task<
        std::variant<javelin::jmap::submission::DraftSaveSummary, javelin::jmap::OperationError>>
    ComposeService::saveDraft(AccountConnectionSettings settings,
                              javelin::jmap::submission::DraftSnapshot snapshot)
    {
        const auto accountId = snapshot.accountId;
        auto result =
            co_await m_service.saveDraft(toLiveConnectionSettings(settings), std::move(snapshot));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
            m_errorCoordinator.reportFailure(settings, accountId, QStringLiteral("Save draft"),
                                             *error);
        else
            m_errorCoordinator.reportSuccess(settings.connectionId);
        co_return result;
    }

    QCoro::Task<std::variant<javelin::jmap::submission::SendSummary, javelin::jmap::OperationError>>
    ComposeService::send(AccountConnectionSettings settings,
                         javelin::jmap::submission::DraftSnapshot snapshot)
    {
        const auto accountId = snapshot.accountId;
        auto result =
            co_await m_service.send(toLiveConnectionSettings(settings), std::move(snapshot));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
            m_errorCoordinator.reportFailure(settings, accountId, QStringLiteral("Send message"),
                                             *error);
        else
            m_errorCoordinator.reportSuccess(settings.connectionId);
        co_return result;
    }

    std::variant<std::optional<javelin::jmap::submission::DraftSnapshot>,
                 javelin::jmap::OperationError>
    ComposeService::loadWorkingCopy(const std::string_view composeSessionId) const
    {
        return m_service.loadWorkingCopy(composeSessionId);
    }

    std::optional<javelin::jmap::OperationError>
    ComposeService::storeWorkingCopy(const javelin::jmap::submission::DraftSnapshot& snapshot)
    {
        return m_service.storeWorkingCopy(snapshot);
    }

    std::optional<javelin::jmap::OperationError>
    ComposeService::discard(const std::string_view composeSessionId)
    {
        return m_service.discard(composeSessionId);
    }

} // namespace javelin::app
