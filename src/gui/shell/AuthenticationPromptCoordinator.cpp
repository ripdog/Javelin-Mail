#include "gui/shell/AuthenticationPromptCoordinator.h"

#include "app/OnboardingApplicationPorts.h"
#include "gui/onboarding/FirstRunWizard.h"
#include "gui/settings/GuiSettings.h"
#include "gui/shell/AccountRefreshController.h"

#include <KLocalizedString>

#include <QAbstractButton>
#include <QDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QWidget>

#include <algorithm>

namespace javelin::gui::shell
{
    AuthenticationPromptCoordinator::AuthenticationPromptCoordinator(
        javelin::gui::settings::GuiSettings& settings, javelin::app::OnboardingPort& onboardingPort,
        AccountRefreshController& accountRefreshController, QWidget& parentWidget, QObject* parent)
        : QObject(parent), m_settings(settings), m_onboardingPort(onboardingPort),
          m_accountRefreshController(accountRefreshController), m_parentWidget(parentWidget)
    {
    }

    bool AuthenticationPromptCoordinator::connectionRequiresAuthentication(
        const QString& connectionId) const
    {
        const auto accounts = m_settings.accounts();
        const auto account = std::ranges::find(accounts, connectionId,
                                               &javelin::gui::settings::ConnectionSettings::id);
        if (account == accounts.end())
            return false;
        return std::ranges::any_of(
            account->cachedAccountIds, [this](const QString& accountId)
            { return m_authenticationRequiredAccountIds.contains(accountId); });
    }

    void AuthenticationPromptCoordinator::updateAccountStatus(
        const QString& accountId, const javelin::app::MailAccountStatus status)
    {
        const auto account = m_settings.accountForCachedId(accountId);
        if (account.id.isEmpty())
            return;

        if (status == javelin::app::MailAccountStatus::AuthenticationPaused)
            m_authenticationRequiredAccountIds.insert(accountId);
        else
            m_authenticationRequiredAccountIds.remove(accountId);

        if (!connectionRequiresAuthentication(account.id))
        {
            m_promptedConnections.remove(account.id);
            m_pendingPrompts.removeAll(account.id);
            return;
        }

        if (m_promptedConnections.contains(account.id))
            return;
        m_promptedConnections.insert(account.id);
        m_pendingPrompts.push_back(account.id);
        QTimer::singleShot(0, this, &AuthenticationPromptCoordinator::showNextPrompt);
    }

    void AuthenticationPromptCoordinator::showNextPrompt()
    {
        if (m_promptOpen)
            return;
        while (!m_pendingPrompts.isEmpty())
        {
            const auto connectionId = m_pendingPrompts.takeFirst();
            const auto accounts = m_settings.accounts();
            const auto account = std::ranges::find(accounts, connectionId,
                                                   &javelin::gui::settings::ConnectionSettings::id);
            if (account == accounts.end() || !connectionRequiresAuthentication(connectionId))
                continue;

            const auto accountName =
                account->displayName.isEmpty() ? account->loginEmail : account->displayName;
            QMessageBox prompt{
                QMessageBox::Warning, i18n("Sign in required"),
                i18n("%1 needs you to sign in again before Javelin can synchronize mail.",
                     accountName),
                QMessageBox::NoButton, &m_parentWidget};
            auto* signInButton = prompt.addButton(i18n("Sign In Again"), QMessageBox::AcceptRole);
            prompt.setDefaultButton(signInButton);
            prompt.addButton(i18nc("@action:button", "Later"), QMessageBox::RejectRole);
            m_promptOpen = true;
            prompt.exec();
            m_promptOpen = false;
            if (prompt.clickedButton() == signInButton)
                reauthenticateConnection(connectionId);
            break;
        }

        if (!m_pendingPrompts.isEmpty())
            QTimer::singleShot(0, this, &AuthenticationPromptCoordinator::showNextPrompt);
    }

    void AuthenticationPromptCoordinator::reauthenticateConnection(const QString& connectionId)
    {
        javelin::gui::onboarding::FirstRunWizard wizard{m_onboardingPort, m_settings, connectionId,
                                                        &m_parentWidget};
        if (wizard.exec() != QDialog::Accepted)
            return;

        const auto accounts = m_settings.accounts();
        const auto account = std::ranges::find(accounts, connectionId,
                                               &javelin::gui::settings::ConnectionSettings::id);
        if (account != accounts.end())
            m_accountRefreshController.refreshConnection(*account);
    }
} // namespace javelin::gui::shell
