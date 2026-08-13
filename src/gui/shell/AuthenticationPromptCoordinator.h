#pragma once

#include "app/MailApplicationEventsPorts.h"

#include <QObject>
#include <QSet>
#include <QStringList>

class QWidget;

namespace javelin::app
{
    class OnboardingPort;
}

namespace javelin::gui::settings
{
    class GuiSettings;
}

namespace javelin::gui::shell
{
    class AccountRefreshController;

    class AuthenticationPromptCoordinator final : public QObject
    {
        Q_OBJECT

      public:
        AuthenticationPromptCoordinator(javelin::gui::settings::GuiSettings& settings,
                                        javelin::app::OnboardingPort& onboardingPort,
                                        AccountRefreshController& accountRefreshController,
                                        QWidget& parentWidget, QObject* parent = nullptr);

        void updateAccountStatus(const QString& accountId, javelin::app::MailAccountStatus status);

      private:
        void showNextPrompt();
        void reauthenticateConnection(const QString& connectionId);
        [[nodiscard]] bool connectionRequiresAuthentication(const QString& connectionId) const;

        javelin::gui::settings::GuiSettings& m_settings;
        javelin::app::OnboardingPort& m_onboardingPort;
        AccountRefreshController& m_accountRefreshController;
        QWidget& m_parentWidget;
        bool m_promptOpen = false;
        QSet<QString> m_authenticationRequiredAccountIds;
        QSet<QString> m_promptedConnections;
        QStringList m_pendingPrompts;
    };
} // namespace javelin::gui::shell
