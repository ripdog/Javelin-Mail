#pragma once

#include "app/MailApplicationService.h"
#include "gui/settings/ConnectionSettings.h"

#include <string>
#include <vector>

namespace javelin::gui::settings
{
    [[nodiscard]] javelin::app::AccountConnectionSettings
    toAccountConnectionSettings(const ConnectionSettings& settings);

    [[nodiscard]] javelin::app::AccountBootstrapIntent
    toAccountBootstrapIntent(const ConnectionSettings& settings,
                             std::vector<std::string> mailboxIds);
} // namespace javelin::gui::settings
