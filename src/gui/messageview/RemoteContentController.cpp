#include "gui/messageview/RemoteContentController.h"

#include "gui/messageview/HtmlMessageView.h"
#include "gui/settings/GuiSettings.h"
#include "protocol/SettingsContract.h"

#include <QDebug>

#include <utility>

namespace javelin::gui::messageview
{
    RemoteContentController::RemoteContentController(javelin::gui::settings::GuiSettings& settings)
        : m_settings(settings)
    {
    }

    QString RemoteContentController::senderAddress(
        const std::optional<javelin::jmap::cache::MessageViewSnapshot>& snapshot) const
    {
        if (!snapshot.has_value() || snapshot->email.from.empty())
            return {};
        return QString::fromStdString(snapshot->email.from.front().email).trimmed().toLower();
    }

    QString RemoteContentController::senderDomain(
        const std::optional<javelin::jmap::cache::MessageViewSnapshot>& snapshot) const
    {
        const auto sender = senderAddress(snapshot);
        const auto atIndex = sender.lastIndexOf(QLatin1Char('@'));
        if (atIndex < 0 || atIndex + 1 >= sender.size())
            return {};
        return sender.sliced(atIndex + 1).trimmed().toLower();
    }

    bool RemoteContentController::savedPermitAllows(
        const std::optional<javelin::jmap::cache::MessageViewSnapshot>& snapshot) const
    {
        const auto sender = senderAddress(snapshot);
        const auto domain = senderDomain(snapshot);
        return (!sender.isEmpty() &&
                m_settings.remoteContentSenders().contains(sender, Qt::CaseInsensitive)) ||
               (!domain.isEmpty() &&
                m_settings.remoteContentDomains().contains(domain, Qt::CaseInsensitive));
    }

    void RemoteContentController::applySavedPermit(
        const std::optional<javelin::jmap::cache::MessageViewSnapshot>& snapshot,
        HtmlMessageView& view) const
    {
        const bool shouldAllow = savedPermitAllows(snapshot);
        if (view.remoteContentEnabled() != shouldAllow)
            view.setRemoteContentEnabled(shouldAllow);
    }

    void RemoteContentController::permitSender(
        const std::optional<javelin::jmap::cache::MessageViewSnapshot>& snapshot)
    {
        addPermit(true, senderAddress(snapshot));
    }

    void RemoteContentController::permitDomain(
        const std::optional<javelin::jmap::cache::MessageViewSnapshot>& snapshot)
    {
        addPermit(false, senderDomain(snapshot));
    }

    void RemoteContentController::addPermit(const bool sender, QString value)
    {
        value = value.trimmed().toLower();
        if (value.isEmpty())
            return;

        auto values =
            sender ? m_settings.remoteContentSenders() : m_settings.remoteContentDomains();
        if (values.contains(value, Qt::CaseInsensitive))
            return;
        values.push_back(std::move(value));
        values.removeDuplicates();
        values.sort(Qt::CaseInsensitive);

        javelin::protocol::SettingsUpdate update;
        if (sender)
            update.remoteContentSenders = {values.begin(), values.end()};
        else
            update.remoteContentDomains = {values.begin(), values.end()};
        if (const auto error = m_settings.update(std::move(update)))
            qWarning().noquote() << "Could not save remote-content permission" << error->detail;
    }
} // namespace javelin::gui::messageview
