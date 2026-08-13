#pragma once

#include "jmap/cache/MessageViewReader.h"

#include <QString>

#include <optional>

namespace javelin::gui::settings
{
    class GuiSettings;
}

namespace javelin::gui::messageview
{
    class HtmlMessageView;

    class RemoteContentController final
    {
      public:
        explicit RemoteContentController(javelin::gui::settings::GuiSettings& settings);

        [[nodiscard]] QString senderAddress(
            const std::optional<javelin::jmap::cache::MessageViewSnapshot>& snapshot) const;
        [[nodiscard]] QString senderDomain(
            const std::optional<javelin::jmap::cache::MessageViewSnapshot>& snapshot) const;
        [[nodiscard]] bool savedPermitAllows(
            const std::optional<javelin::jmap::cache::MessageViewSnapshot>& snapshot) const;
        void
        applySavedPermit(const std::optional<javelin::jmap::cache::MessageViewSnapshot>& snapshot,
                         HtmlMessageView& view) const;
        void permitSender(const std::optional<javelin::jmap::cache::MessageViewSnapshot>& snapshot);
        void permitDomain(const std::optional<javelin::jmap::cache::MessageViewSnapshot>& snapshot);

      private:
        void addPermit(bool sender, QString value);

        javelin::gui::settings::GuiSettings& m_settings;
    };
} // namespace javelin::gui::messageview
