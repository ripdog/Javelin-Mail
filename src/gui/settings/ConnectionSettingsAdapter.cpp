#include "gui/settings/ConnectionSettingsAdapter.h"

#include <utility>

namespace javelin::gui::settings
{
    bool needsInitialAccountBootstrap(const ConnectionSettings& settings)
    {
        return settings.cachedAccountIds.isEmpty() && !settings.sessionUrl.isEmpty() &&
               !settings.loginEmail.isEmpty() && settings.hasCredentials;
    }

    javelin::app::AccountConnectionSettings
    toAccountConnectionSettings(const ConnectionSettings& settings)
    {
        return {
            .connectionId = settings.id.toStdString(),
            .revision = settings.revision,
            .sessionUrl = settings.sessionUrl.toStdString(),
            .loginEmail = settings.loginEmail.toStdString(),
            .apiKey = {},
            .refreshToken = {},
            .tokenEndpoint = settings.tokenEndpoint.toStdString(),
            .oauthClientId = settings.oauthClientId.toStdString(),
            .oauthIssuer = settings.oauthIssuer.toStdString(),
            .oauthResource = settings.oauthResource.toStdString(),
            .oauthScope = settings.oauthScope.toStdString(),
            .revocationEndpoint = settings.revocationEndpoint.toStdString(),
            .registrationClientUri = settings.registrationClientUri.toStdString(),
            .registrationAccessToken = {},
        };
    }

    javelin::app::AccountBootstrapIntent
    toAccountBootstrapIntent(const ConnectionSettings& settings,
                             std::vector<std::string> mailboxIds)
    {
        return {
            .settings = toAccountConnectionSettings(settings),
            .mailboxIds = std::move(mailboxIds),
        };
    }
} // namespace javelin::gui::settings
