#include "gui/messageview/MessageBannerCoordinator.h"

#include "gui/settings/GuiSettings.h"

#include <KLocalizedString>

#include <QUrl>

namespace javelin::gui::messageview
{
    MessageBannerCoordinator::MessageBannerCoordinator(
        javelin::gui::settings::GuiSettings& settings, const std::optional<std::string>& accountId,
        const std::optional<std::string>& emailId)
        : m_settings(settings), m_accountId(accountId), m_emailId(emailId)
    {
    }

    std::string MessageBannerCoordinator::key(const std::string_view bannerId) const
    {
        if (!m_accountId.has_value() || !m_emailId.has_value())
            return {};
        return std::string{bannerId} + '\n' + *m_accountId + '\n' + *m_emailId;
    }

    bool MessageBannerCoordinator::dismissed(const std::string_view bannerId) const
    {
        const auto bannerKey = key(bannerId);
        return !bannerKey.empty() && m_dismissed.contains(bannerKey);
    }

    void MessageBannerCoordinator::dismiss(const std::string_view bannerId)
    {
        const auto bannerKey = key(bannerId);
        if (!bannerKey.empty())
            m_dismissed.insert(bannerKey);
    }

    QString MessageBannerCoordinator::serverDisplayName() const
    {
        if (!m_accountId.has_value())
            return i18n("Server");

        const auto account = m_settings.accountForCachedId(QString::fromStdString(*m_accountId));
        auto name = QUrl{account.sessionUrl}.host();
        if (name.isEmpty())
            name = account.displayName.trimmed();
        if (name.isEmpty())
            name = account.loginEmail.trimmed();
        return name.isEmpty() ? i18n("Server") : name;
    }
} // namespace javelin::gui::messageview
