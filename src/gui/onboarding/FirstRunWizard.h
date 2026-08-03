#pragma once

#include "app/OnboardingTypes.h"

#include <QWizard>

#include <optional>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTcpServer;
class QToolButton;
class QWidget;

namespace javelin::app
{
    class OnboardingPort;
}

namespace javelin::gui::settings
{
    class GuiSettings;
}

namespace javelin::gui::onboarding
{
    class CompletionPage;

    class FirstRunWizard final : public QWizard
    {
        Q_OBJECT

      public:
        FirstRunWizard(javelin::app::OnboardingPort& onboarding,
                       javelin::gui::settings::GuiSettings& settings, QWidget* parent = nullptr);
        ~FirstRunWizard() override;

      protected:
        void accept() override;

      private:
        void buildWelcomePage();
        void buildDiscoveryPage();
        void buildAuthenticationPage();
        void buildFinishedPage();
        void beginDiscovery();
        void updateDiscovery();
        void beginOAuth();
        void beginManualAuthentication();
        void handleBrowserCallback();
        void completeAuthentication(javelin::app::AccountAuthenticationResult result);
        void showFeatures(QListWidget& list,
                          const std::vector<javelin::app::OnboardingFeature>& features) const;
        void setBusy(bool busy, const QString& message = {});

        javelin::app::OnboardingPort& m_onboarding;
        javelin::gui::settings::GuiSettings& m_settings;
        QLineEdit* m_nameEdit = nullptr;
        QLineEdit* m_emailEdit = nullptr;
        CompletionPage* m_discoveryPage = nullptr;
        QLabel* m_discoveryStatus = nullptr;
        QListWidget* m_discoveryFeatures = nullptr;
        CompletionPage* m_authenticationPage = nullptr;
        QLabel* m_authenticationStatus = nullptr;
        QPushButton* m_oauthButton = nullptr;
        QToolButton* m_manualToggle = nullptr;
        QWidget* m_manualPanel = nullptr;
        QLineEdit* m_serverEdit = nullptr;
        QLineEdit* m_tokenEdit = nullptr;
        QPushButton* m_manualButton = nullptr;
        QListWidget* m_finishedFeatures = nullptr;
        QTcpServer* m_callbackServer = nullptr;
        QString m_discoveredEmail;
        QString m_oauthFlowId;
        std::optional<javelin::app::AccountDiscoveryResult> m_discovery;
        std::optional<javelin::app::AccountAuthenticationResult> m_authentication;
        bool m_busy = false;
    };
} // namespace javelin::gui::onboarding
