#pragma once

#include <QString>

#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace javelin::gui::settings
{
    class GuiSettings;
}

namespace javelin::gui::messageview
{
    class MessageBannerCoordinator final
    {
      public:
        MessageBannerCoordinator(javelin::gui::settings::GuiSettings& settings,
                                 const std::optional<std::string>& accountId,
                                 const std::optional<std::string>& emailId);

        [[nodiscard]] bool dismissed(std::string_view bannerId) const;
        void dismiss(std::string_view bannerId);
        [[nodiscard]] QString serverDisplayName() const;

      private:
        [[nodiscard]] std::string key(std::string_view bannerId) const;

        javelin::gui::settings::GuiSettings& m_settings;
        const std::optional<std::string>& m_accountId;
        const std::optional<std::string>& m_emailId;
        std::unordered_set<std::string> m_dismissed;
    };
} // namespace javelin::gui::messageview
