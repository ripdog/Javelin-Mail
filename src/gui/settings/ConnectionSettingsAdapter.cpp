#include "gui/settings/ConnectionSettingsAdapter.h"

#include <utility>

namespace javelin::gui::settings
{
    javelin::app::AccountConnectionSettings
    toAccountConnectionSettings(const ConnectionSettings& settings)
    {
        return {
            .connectionId = settings.id.toStdString(),
            .revision = settings.revision,
            .sessionUrl = settings.sessionUrl.toStdString(),
            .loginEmail = settings.loginEmail.toStdString(),
            .apiKey = settings.apiKey.toStdString(),
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
