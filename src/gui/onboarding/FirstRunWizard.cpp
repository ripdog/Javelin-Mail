#include "gui/onboarding/FirstRunWizard.h"

#include "app/OnboardingApplicationPorts.h"
#include "gui/settings/GuiSettings.h"

#include <QCoroTask>

#include <QAbstractSocket>
#include <QDesktopServices>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QToolButton>
#include <QUrlQuery>
#include <QUuid>
#include <QVBoxLayout>
#include <QWizardPage>

#include <utility>

namespace javelin::gui::onboarding
{
    class CompletionPage final : public QWizardPage
    {
      public:
        using QWizardPage::QWizardPage;

        [[nodiscard]] bool isComplete() const override
        {
            return m_complete;
        }

        void setComplete(const bool complete)
        {
            if (m_complete == complete)
                return;
            m_complete = complete;
            Q_EMIT completeChanged();
        }

      private:
        bool m_complete = false;
    };

    namespace
    {
        constexpr int welcomePageId = 0;
        constexpr int discoveryPageId = 1;
        constexpr int authenticationPageId = 2;
        constexpr int finishedPageId = 3;

        [[nodiscard]] QString featureName(const javelin::app::OnboardingFeatureKind kind)
        {
            using Kind = javelin::app::OnboardingFeatureKind;
            switch (kind)
            {
            case Kind::Jmap:
                return QStringLiteral("JMAP account access");
            case Kind::Mail:
                return QStringLiteral("Mail synchronization");
            case Kind::Sending:
                return QStringLiteral("Send mail");
            case Kind::Contacts:
                return QStringLiteral("Contacts");
            case Kind::Calendars:
                return QStringLiteral("Calendars");
            case Kind::Sieve:
                return QStringLiteral("Server-side mail rules");
            case Kind::Push:
                return QStringLiteral("Instant updates");
            case Kind::OAuth:
                return QStringLiteral("Secure browser sign-in");
            case Kind::DynamicClientRegistration:
                return QStringLiteral("Automatic app registration");
            case Kind::OfflineAccess:
                return QStringLiteral("Stay signed in");
            }
            return QStringLiteral("Unknown feature");
        }
    } // namespace

    FirstRunWizard::FirstRunWizard(javelin::app::OnboardingPort& onboarding,
                                   javelin::gui::settings::GuiSettings& settings, QWidget* parent)
        : QWizard(parent), m_onboarding(onboarding), m_settings(settings),
          m_firstRun(m_settings.accounts().empty())
    {
        setWindowTitle(m_firstRun ? QStringLiteral("Welcome to Javelin Mail")
                                  : QStringLiteral("Add a Mail Account"));
        setWindowIcon(QIcon(QStringLiteral(":/icons/icon.svg")));
        setWizardStyle(QWizard::ModernStyle);
        setOption(QWizard::NoBackButtonOnStartPage);
        setOption(QWizard::NoBackButtonOnLastPage);
        setButtonText(QWizard::FinishButton,
                      m_firstRun ? QStringLiteral("Open Javelin") : QStringLiteral("Add Account"));
        resize(700, 560);
        setStyleSheet(QStringLiteral(
            "QWizard { background: palette(window); }"
            "QWizardPage { padding: 10px; }"
            "QLineEdit { padding: 8px; border-radius: 6px; }"
            "QPushButton { padding: 8px 16px; }"
            "QListWidget { border: 0; background: transparent; font-size: 10.5pt; }"));

        buildWelcomePage();
        buildDiscoveryPage();
        buildAuthenticationPage();
        buildFinishedPage();

        connect(this, &QWizard::currentIdChanged, this,
                [this](const int pageId)
                {
                    if (pageId == discoveryPageId &&
                        m_discoveredEmail != m_emailEdit->text().trimmed())
                        beginDiscovery();
                    else if (pageId == authenticationPageId && m_discovery.has_value())
                    {
                        m_serverEdit->setText(m_discovery->sessionUrl);
                        updateDiscovery();
                    }
                });
    }

    FirstRunWizard::~FirstRunWizard() = default;

    void FirstRunWizard::buildWelcomePage()
    {
        auto* page = new CompletionPage(this);
        page->setTitle(m_firstRun ? QStringLiteral("Let’s set up your mail")
                                  : QStringLiteral("Add another mail account"));
        page->setSubTitle(QStringLiteral(
            "Javelin’s background service is ready. Tell us who this account belongs to."));
        auto* layout = new QVBoxLayout(page);
        auto* ready = new QLabel(QStringLiteral("✓ Background sync is ready"), page);
        ready->setStyleSheet(QStringLiteral("color: palette(highlight); font-weight: 600;"));
        layout->addWidget(ready);
        auto* form = new QFormLayout();
        m_nameEdit = new QLineEdit(page);
        m_nameEdit->setPlaceholderText(QStringLiteral("Your name"));
        m_emailEdit = new QLineEdit(page);
        m_emailEdit->setPlaceholderText(QStringLiteral("you@example.com"));
        form->addRow(QStringLiteral("Name"), m_nameEdit);
        form->addRow(QStringLiteral("Email address"), m_emailEdit);
        layout->addSpacing(18);
        layout->addLayout(form);
        layout->addStretch();
        const auto updateComplete = [page, this]
        {
            const auto email = m_emailEdit->text().trimmed();
            page->setComplete(!m_nameEdit->text().trimmed().isEmpty() &&
                              email.contains(QLatin1Char('@')) &&
                              !email.endsWith(QLatin1Char('@')));
        };
        connect(m_nameEdit, &QLineEdit::textChanged, page, updateComplete);
        connect(m_emailEdit, &QLineEdit::textChanged, page, updateComplete);
        updateComplete();
        setPage(welcomePageId, page);
    }

    void FirstRunWizard::buildDiscoveryPage()
    {
        m_discoveryPage = new CompletionPage(this);
        m_discoveryPage->setTitle(QStringLiteral("Checking your mail service"));
        m_discoveryPage->setSubTitle(QStringLiteral(
            "Javelin uses your email address to find the right server automatically."));
        auto* layout = new QVBoxLayout(m_discoveryPage);
        m_discoveryStatus = new QLabel(QStringLiteral("Looking for your server…"), m_discoveryPage);
        m_discoveryStatus->setWordWrap(true);
        layout->addWidget(m_discoveryStatus);
        m_discoveryFeatures = new QListWidget(m_discoveryPage);
        m_discoveryFeatures->setFocusPolicy(Qt::NoFocus);
        layout->addWidget(m_discoveryFeatures, 1);
        setPage(discoveryPageId, m_discoveryPage);
    }

    void FirstRunWizard::buildAuthenticationPage()
    {
        m_authenticationPage = new CompletionPage(this);
        m_authenticationPage->setTitle(QStringLiteral("Sign in"));
        m_authenticationPage->setSubTitle(QStringLiteral(
            "Use your normal browser so password managers and security keys work as expected."));
        auto* layout = new QVBoxLayout(m_authenticationPage);
        m_authenticationStatus = new QLabel(m_authenticationPage);
        m_authenticationStatus->setWordWrap(true);
        layout->addWidget(m_authenticationStatus);
        m_oauthButton =
            new QPushButton(QStringLiteral("Continue in browser"), m_authenticationPage);
        m_oauthButton->setDefault(true);
        m_oauthButton->setIcon(QIcon::fromTheme(QStringLiteral("internet-web-browser")));
        layout->addWidget(m_oauthButton);
        connect(m_oauthButton, &QPushButton::clicked, this, &FirstRunWizard::beginOAuth);

        m_manualToggle = new QToolButton(m_authenticationPage);
        m_manualToggle->setText(QStringLiteral("Use manual details instead"));
        m_manualToggle->setCheckable(true);
        m_manualToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        m_manualToggle->setArrowType(Qt::RightArrow);
        layout->addWidget(m_manualToggle, 0, Qt::AlignLeft);

        m_manualPanel = new QGroupBox(QStringLiteral("Manual JMAP sign-in"), m_authenticationPage);
        auto* manualLayout = new QFormLayout(m_manualPanel);
        m_serverEdit = new QLineEdit(m_manualPanel);
        m_serverEdit->setPlaceholderText(
            QStringLiteral("Discovered automatically when left blank"));
        m_tokenEdit = new QLineEdit(m_manualPanel);
        m_tokenEdit->setEchoMode(QLineEdit::Password);
        m_tokenEdit->setPlaceholderText(QStringLiteral("API token or access token"));
        m_manualButton = new QPushButton(QStringLiteral("Verify details"), m_manualPanel);
        manualLayout->addRow(QStringLiteral("JMAP server"), m_serverEdit);
        manualLayout->addRow(QStringLiteral("Access token"), m_tokenEdit);
        manualLayout->addRow(QString{}, m_manualButton);
        m_manualPanel->hide();
        layout->addWidget(m_manualPanel);
        layout->addStretch();
        connect(m_manualToggle, &QToolButton::toggled, this,
                [this](const bool expanded)
                {
                    m_manualToggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
                    m_manualPanel->setVisible(expanded);
                });
        connect(m_manualButton, &QPushButton::clicked, this,
                &FirstRunWizard::beginManualAuthentication);
        setPage(authenticationPageId, m_authenticationPage);
    }

    void FirstRunWizard::buildFinishedPage()
    {
        auto* page = new QWizardPage(this);
        page->setTitle(QStringLiteral("Your account is ready"));
        page->setSubTitle(QStringLiteral(
            "Javelin will fetch your mailboxes and recent mail when setup finishes."));
        auto* layout = new QVBoxLayout(page);
        auto* heading = new QLabel(QStringLiteral("Available for this account"), page);
        heading->setStyleSheet(QStringLiteral("font-weight: 600; font-size: 12pt;"));
        layout->addWidget(heading);
        m_finishedFeatures = new QListWidget(page);
        m_finishedFeatures->setFocusPolicy(Qt::NoFocus);
        layout->addWidget(m_finishedFeatures, 1);
        setPage(finishedPageId, page);
    }

    void FirstRunWizard::beginDiscovery()
    {
        m_discoveredEmail = m_emailEdit->text().trimmed();
        m_discovery.reset();
        m_authentication.reset();
        m_discoveryPage->setComplete(false);
        m_discoveryFeatures->clear();
        setBusy(true, QStringLiteral("Looking for your mail service…"));
        auto task = m_onboarding.discover({.emailAddress = m_discoveredEmail});
        QCoro::connect(
            std::move(task), this,
            [this](
                javelin::app::OnboardingCallResult<javelin::app::AccountDiscoveryResult> callResult)
            {
                setBusy(false);
                if (const auto* error = std::get_if<QString>(&callResult))
                {
                    m_discoveryStatus->setText(
                        QStringLiteral("We couldn’t check that service. %1").arg(*error));
                    return;
                }
                m_discovery = std::get<javelin::app::AccountDiscoveryResult>(std::move(callResult));
                updateDiscovery();
            });
    }

    void FirstRunWizard::updateDiscovery()
    {
        if (!m_discovery.has_value())
            return;
        m_discoveryFeatures->clear();
        showFeatures(*m_discoveryFeatures, m_discovery->features);
        if (!m_discovery->succeeded)
        {
            m_discoveryStatus->setText(m_discovery->error);
            m_discoveryPage->setComplete(false);
            return;
        }
        m_discoveryStatus->setText(
            QStringLiteral("We found a compatible service for %1.").arg(m_discovery->emailAddress));
        m_discoveryPage->setComplete(true);
        const bool browserLogin = !m_discovery->authorizationEndpoint.isEmpty() &&
                                  !m_discovery->registrationEndpoint.isEmpty();
        m_oauthButton->setVisible(browserLogin);
        m_authenticationStatus->setText(
            browserLogin
                ? QStringLiteral("Your provider supports secure browser sign-in.")
                : QStringLiteral(
                      "This provider needs an API token or manually registered application."));
        if (!browserLogin)
            m_manualToggle->setChecked(true);
    }

    void FirstRunWizard::beginOAuth()
    {
        if (!m_discovery.has_value() || m_busy)
            return;
        if (m_callbackServer == nullptr)
        {
            m_callbackServer = new QTcpServer(this);
            connect(m_callbackServer, &QTcpServer::newConnection, this,
                    &FirstRunWizard::handleBrowserCallback);
        }
        if (m_callbackServer->isListening())
            m_callbackServer->close();
        if (!m_callbackServer->listen(QHostAddress::LocalHost, 0))
        {
            m_authenticationStatus->setText(
                QStringLiteral("Javelin couldn’t prepare a secure browser callback."));
            return;
        }
        const auto redirect = QStringLiteral("http://127.0.0.1:%1/oauth/callback")
                                  .arg(m_callbackServer->serverPort());
        setBusy(true, QStringLiteral("Preparing secure sign-in…"));
        auto task = m_onboarding.startOAuth({.discovery = *m_discovery, .redirectUri = redirect});
        QCoro::connect(
            std::move(task), this,
            [this](javelin::app::OnboardingCallResult<javelin::app::OAuthStartResult> callResult)
            {
                setBusy(false);
                if (const auto* error = std::get_if<QString>(&callResult))
                {
                    m_authenticationStatus->setText(*error);
                    return;
                }
                const auto result = std::get<javelin::app::OAuthStartResult>(std::move(callResult));
                if (!result.succeeded)
                {
                    m_authenticationStatus->setText(result.error);
                    m_manualToggle->setChecked(true);
                    return;
                }
                m_oauthFlowId = result.flowId;
                m_authenticationStatus->setText(QStringLiteral(
                    "Finish signing in in your browser. You can return here when it closes."));
                if (!QDesktopServices::openUrl(QUrl{result.authorizationUrl}))
                    m_authenticationStatus->setText(QStringLiteral(
                        "Your browser could not be opened. Use manual details below."));
            });
    }

    void FirstRunWizard::handleBrowserCallback()
    {
        while (m_callbackServer != nullptr && m_callbackServer->hasPendingConnections())
        {
            auto* socket = m_callbackServer->nextPendingConnection();
            connect(
                socket, &QTcpSocket::readyRead, this,
                [this, socket]
                {
                    if (socket->property("javelinCallbackHandled").toBool())
                        return;
                    auto request = socket->property("javelinCallbackRequest").toByteArray();
                    request += socket->readAll();
                    socket->setProperty("javelinCallbackRequest", request);
                    const auto firstLineEnd = request.indexOf("\r\n");
                    if (firstLineEnd < 0)
                        return;
                    const auto firstLine = request.first(firstLineEnd);
                    const auto parts = firstLine.split(' ');
                    if (parts.size() < 2)
                        return;
                    socket->setProperty("javelinCallbackHandled", true);
                    const QUrl callbackUrl{QStringLiteral("http://127.0.0.1") +
                                           QString::fromLatin1(parts.at(1))};
                    const QUrlQuery query{callbackUrl};
                    const auto code =
                        query.queryItemValue(QStringLiteral("code"), QUrl::FullyDecoded);
                    const auto state =
                        query.queryItemValue(QStringLiteral("state"), QUrl::FullyDecoded);
                    const auto issuer =
                        query.queryItemValue(QStringLiteral("iss"), QUrl::FullyDecoded);
                    const auto error =
                        query.queryItemValue(QStringLiteral("error"), QUrl::FullyDecoded);
                    const QByteArray body =
                        error.isEmpty()
                            ? QByteArrayLiteral("<h1>Signed in</h1><p>You can close this tab "
                                                "and return to Javelin.</p>")
                            : QByteArrayLiteral("<h1>Sign-in was cancelled</h1><p>You can "
                                                "return to Javelin and try again.</p>");
                    socket->write(QByteArrayLiteral(
                                      "HTTP/1.1 200 OK\r\nContent-Type: text/html; "
                                      "charset=utf-8\r\nConnection: close\r\nContent-Length: ") +
                                  QByteArray::number(body.size()) + QByteArrayLiteral("\r\n\r\n") +
                                  body);
                    socket->disconnectFromHost();
                    if (m_callbackServer != nullptr)
                        m_callbackServer->close();
                    if (!error.isEmpty())
                    {
                        m_authenticationStatus->setText(QStringLiteral(
                            "Sign-in was cancelled. You can try again or use manual details."));
                        return;
                    }
                    setBusy(true, QStringLiteral("Finishing sign-in…"));
                    auto task = m_onboarding.finishOAuth(
                        {.flowId = m_oauthFlowId, .code = code, .state = state, .issuer = issuer});
                    QCoro::connect(std::move(task), this,
                                   [this](javelin::app::OnboardingCallResult<
                                          javelin::app::AccountAuthenticationResult>
                                              callResult)
                                   {
                                       setBusy(false);
                                       if (const auto* callError =
                                               std::get_if<QString>(&callResult))
                                       {
                                           m_authenticationStatus->setText(*callError);
                                           return;
                                       }
                                       completeAuthentication(
                                           std::get<javelin::app::AccountAuthenticationResult>(
                                               std::move(callResult)));
                                   });
                });
        }
    }

    void FirstRunWizard::beginManualAuthentication()
    {
        if (m_busy || m_tokenEdit->text().trimmed().isEmpty())
        {
            m_authenticationStatus->setText(QStringLiteral("Enter an API or access token first."));
            return;
        }
        setBusy(true, QStringLiteral("Checking those details…"));
        auto task = m_onboarding.authenticateManually({
            .emailAddress = m_emailEdit->text().trimmed(),
            .sessionUrl = m_serverEdit->text().trimmed(),
            .accessToken = m_tokenEdit->text().trimmed(),
        });
        QCoro::connect(
            std::move(task), this,
            [this](javelin::app::OnboardingCallResult<javelin::app::AccountAuthenticationResult>
                       callResult)
            {
                setBusy(false);
                if (const auto* error = std::get_if<QString>(&callResult))
                {
                    m_authenticationStatus->setText(*error);
                    return;
                }
                completeAuthentication(
                    std::get<javelin::app::AccountAuthenticationResult>(std::move(callResult)));
            });
    }

    void FirstRunWizard::completeAuthentication(javelin::app::AccountAuthenticationResult result)
    {
        if (!result.succeeded)
        {
            m_authenticationStatus->setText(result.error);
            return;
        }
        m_authentication = std::move(result);
        m_authenticationStatus->setText(QStringLiteral("✓ Signed in successfully"));
        m_authenticationPage->setComplete(true);
        m_finishedFeatures->clear();
        showFeatures(*m_finishedFeatures, m_authentication->features);
        next();
    }

    void
    FirstRunWizard::showFeatures(QListWidget& list,
                                 const std::vector<javelin::app::OnboardingFeature>& features) const
    {
        for (const auto& feature : features)
        {
            const bool pending = feature.detail == QStringLiteral("Checked after sign-in");
            const auto prefix = feature.available ? QStringLiteral("✓  ")
                                : pending         ? QStringLiteral("?  ")
                                                  : QStringLiteral("—  ");
            const auto suffix = pending ? QStringLiteral(" — checked after sign-in") : QString{};
            auto* item = new QListWidgetItem(prefix + featureName(feature.kind) + suffix, &list);
            item->setForeground(feature.available || pending
                                    ? palette().brush(QPalette::Text)
                                    : palette().brush(QPalette::PlaceholderText));
            if (!feature.detail.isEmpty())
                item->setToolTip(feature.detail);
        }
    }

    void FirstRunWizard::setBusy(const bool busy, const QString& message)
    {
        m_busy = busy;
        m_oauthButton->setEnabled(!busy);
        m_manualButton->setEnabled(!busy);
        if (!message.isEmpty())
        {
            if (currentId() == discoveryPageId)
                m_discoveryStatus->setText(message);
            else
                m_authenticationStatus->setText(message);
        }
    }

    void FirstRunWizard::accept()
    {
        if (!m_authentication.has_value())
            return;
        auto accounts = m_settings.accounts();
        accounts.push_back({
            .id = QUuid::createUuid().toString(QUuid::WithoutBraces),
            .revision = 1,
            .displayName = m_nameEdit->text().trimmed(),
            .sessionUrl = m_authentication->sessionUrl,
            .loginEmail = m_emailEdit->text().trimmed(),
            .apiKey = m_authentication->accessToken,
            .refreshToken = m_authentication->refreshToken,
            .tokenEndpoint = m_authentication->tokenEndpoint,
            .oauthClientId = m_authentication->clientId,
            .tokenExpiresAtEpochSeconds = m_authentication->expiresAtEpochSeconds,
            .cachedAccountIds = {},
        });
        javelin::protocol::SettingsUpdate update;
        update.accounts = javelin::gui::settings::GuiSettings::protocolAccounts(accounts);
        if (const auto error = m_settings.update(std::move(update)))
        {
            QMessageBox::warning(
                this, QStringLiteral("Couldn’t save the account"),
                QStringLiteral("Javelin couldn’t save your account yet. Please try again."));
            return;
        }
        QWizard::accept();
    }
} // namespace javelin::gui::onboarding
