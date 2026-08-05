#include "gui/onboarding/FirstRunWizard.h"

#include "app/OnboardingApplicationPorts.h"
#include "gui/settings/GuiSettings.h"

#include <QCoroTask>

#include <KLocalizedString>

#include <QAbstractSocket>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QToolButton>
#include <QUrlQuery>
#include <QUuid>
#include <QVBoxLayout>
#include <QWizardPage>

#include <algorithm>
#include <utility>

Q_LOGGING_CATEGORY(onboardingOAuthLog, "javelin.oauth.gui")

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
                return i18n("JMAP account access");
            case Kind::Mail:
                return i18n("Mail synchronization");
            case Kind::Sending:
                return i18n("Send mail");
            case Kind::Contacts:
                return i18n("Contacts");
            case Kind::Calendars:
                return i18n("Calendars");
            case Kind::Sieve:
                return i18n("Server-side mail rules");
            case Kind::Push:
                return i18n("Instant updates");
            case Kind::OAuth:
                return i18n("Secure browser sign-in");
            case Kind::DynamicClientRegistration:
                return i18n("Automatic app registration");
            case Kind::OfflineAccess:
                return i18n("Stay signed in");
            }
            return i18n("Unknown feature");
        }

        void logOAuthResult(const char* stage,
                            const javelin::app::AccountAuthenticationResult& result)
        {
            qCInfo(onboardingOAuthLog).noquote()
                << stage << "succeeded=" << result.succeeded
                << "accessTokenPresent=" << !result.accessToken.isEmpty()
                << "refreshTokenPresent=" << !result.refreshToken.isEmpty()
                << "clientIdPresent=" << !result.clientId.isEmpty()
                << "tokenEndpointHost=" << QUrl{result.tokenEndpoint}.host()
                << "expiresAtEpochSeconds=" << result.expiresAtEpochSeconds;
        }

        void logOAuthSettings(const char* stage,
                              const javelin::gui::settings::ConnectionSettings& account)
        {
            qCInfo(onboardingOAuthLog).noquote()
                << stage << "connection=" << account.id
                << "accessTokenPresent=" << !account.apiKey.isEmpty()
                << "refreshTokenPresent=" << !account.refreshToken.isEmpty()
                << "clientIdPresent=" << !account.oauthClientId.isEmpty()
                << "tokenEndpointHost=" << QUrl{account.tokenEndpoint}.host()
                << "expiresAtEpochSeconds=" << account.tokenExpiresAtEpochSeconds;
        }
    } // namespace

    FirstRunWizard::FirstRunWizard(javelin::app::OnboardingPort& onboarding,
                                   javelin::gui::settings::GuiSettings& settings, QWidget* parent)
        : FirstRunWizard(onboarding, settings, {}, parent)
    {
    }

    FirstRunWizard::FirstRunWizard(javelin::app::OnboardingPort& onboarding,
                                   javelin::gui::settings::GuiSettings& settings,
                                   QString connectionId, QWidget* parent)
        : QWizard(parent), m_onboarding(onboarding), m_settings(settings),
          m_connectionId(std::move(connectionId)),
          m_firstRun(m_connectionId.isEmpty() && m_settings.accounts().empty())
    {
        setWindowTitle(!m_connectionId.isEmpty() ? i18n("Sign In Again")
                       : m_firstRun              ? i18n("Welcome to Javelin Mail")
                                                 : i18n("Add a Mail Account"));
        setWindowIcon(QIcon(QStringLiteral(":/icons/icon.svg")));
        setWizardStyle(QWizard::ModernStyle);
        setOption(QWizard::NoBackButtonOnStartPage);
        setOption(QWizard::NoBackButtonOnLastPage);
        setButtonText(QWizard::FinishButton, !m_connectionId.isEmpty() ? i18n("Finish Sign-In")
                                             : m_firstRun              ? i18n("Open Javelin")
                                                                       : i18n("Add Account"));
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

        if (!m_connectionId.isEmpty())
        {
            const auto accounts = m_settings.accounts();
            const auto account = std::ranges::find(accounts, m_connectionId,
                                                   &javelin::gui::settings::ConnectionSettings::id);
            if (account != accounts.end())
            {
                m_nameEdit->setText(account->displayName);
                m_emailEdit->setText(account->loginEmail);
                m_emailEdit->setReadOnly(true);
            }
            setStartId(discoveryPageId);
        }

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

    void FirstRunWizard::cancelOAuthFlow()
    {
        if (m_oauthFlowId.isEmpty())
            return;
        const auto flowId = std::exchange(m_oauthFlowId, {});
        m_oauthState.clear();
        auto task = m_onboarding.cancelOAuth({.flowId = flowId});
        QCoro::connect(
            std::move(task), QCoreApplication::instance(),
            [](javelin::app::OnboardingCallResult<javelin::app::OAuthCancelResult> result)
            {
                if (const auto* error = std::get_if<QString>(&result))
                    qCWarning(onboardingOAuthLog).noquote()
                        << "OAuth flow cancellation failed" << *error;
                else if (const auto& cancelled =
                             std::get<javelin::app::OAuthCancelResult>(result);
                         !cancelled.error.isEmpty())
                    qCWarning(onboardingOAuthLog).noquote()
                        << "OAuth registration cleanup failed" << cancelled.error;
            });
    }

    void FirstRunWizard::reject()
    {
        cancelOAuthFlow();
        QWizard::reject();
    }

    void FirstRunWizard::buildWelcomePage()
    {
        auto* page = new CompletionPage(this);
        page->setTitle(!m_connectionId.isEmpty() ? i18n("Sign in to your account again")
                       : m_firstRun              ? i18n("Let’s set up your mail")
                                                 : i18n("Add another mail account"));
        page->setSubTitle(
            i18n("Javelin’s background service is ready. Choose a label for this account."));
        auto* layout = new QVBoxLayout(page);
        auto* ready = new QLabel(i18n("✓ Background sync is ready"), page);
        ready->setStyleSheet(QStringLiteral("color: palette(highlight); font-weight: 600;"));
        layout->addWidget(ready);
        auto* form = new QFormLayout();
        m_nameEdit = new QLineEdit(page);
        m_nameEdit->setPlaceholderText(i18n("Personal mail, Work, or another label"));
        m_emailEdit = new QLineEdit(page);
        m_emailEdit->setPlaceholderText(QStringLiteral("you@example.com"));
        form->addRow(i18n("Account name"), m_nameEdit);
        form->addRow(i18n("Email address"), m_emailEdit);
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
        m_discoveryPage->setTitle(i18n("Checking your mail service"));
        m_discoveryPage->setSubTitle(
            i18n("Javelin uses your email address to find the right server automatically."));
        auto* layout = new QVBoxLayout(m_discoveryPage);
        m_discoveryStatus = new QLabel(i18n("Looking for your server…"), m_discoveryPage);
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
        m_authenticationPage->setTitle(i18n("Sign in"));
        m_authenticationPage->setSubTitle(i18n("Sign in with your mail service to continue."));
        auto* layout = new QVBoxLayout(m_authenticationPage);
        m_authenticationStatus = new QLabel(m_authenticationPage);
        m_authenticationStatus->setWordWrap(true);
        layout->addWidget(m_authenticationStatus);
        m_oauthButton = new QPushButton(i18n("Continue in browser"), m_authenticationPage);
        m_oauthButton->setDefault(true);
        m_oauthButton->setIcon(QIcon::fromTheme(QStringLiteral("internet-web-browser")));
        layout->addWidget(m_oauthButton);
        connect(m_oauthButton, &QPushButton::clicked, this, &FirstRunWizard::beginOAuth);

        m_manualToggle = new QToolButton(m_authenticationPage);
        m_manualToggle->setText(i18n("Use manual details instead"));
        m_manualToggle->setCheckable(true);
        m_manualToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        m_manualToggle->setArrowType(Qt::RightArrow);
        layout->addWidget(m_manualToggle, 0, Qt::AlignLeft);

        m_manualPanel = new QGroupBox(i18n("Manual JMAP sign-in"), m_authenticationPage);
        auto* manualLayout = new QFormLayout(m_manualPanel);
        m_serverEdit = new QLineEdit(m_manualPanel);
        m_serverEdit->setPlaceholderText(i18n("Discovered automatically when left blank"));
        m_tokenEdit = new QLineEdit(m_manualPanel);
        m_tokenEdit->setEchoMode(QLineEdit::Password);
        m_tokenEdit->setPlaceholderText(i18n("API token or access token"));
        m_manualButton = new QPushButton(i18n("Verify details"), m_manualPanel);
        manualLayout->addRow(i18n("JMAP server"), m_serverEdit);
        manualLayout->addRow(i18n("Access token"), m_tokenEdit);
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
        page->setTitle(!m_connectionId.isEmpty() ? i18n("You’re signed in again")
                                                 : i18n("Your account is ready"));
        page->setSubTitle(
            !m_connectionId.isEmpty()
                ? i18n("Javelin will resume synchronizing when setup finishes.")
                : i18n("Javelin will fetch your mailboxes and recent mail when setup finishes."));
        auto* layout = new QVBoxLayout(page);
        auto* heading = new QLabel(i18n("Available for this account"), page);
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
        setBusy(true, i18n("Looking for your mail service…"));
        auto task = m_onboarding.discover({.emailAddress = m_discoveredEmail});
        QCoro::connect(
            std::move(task), this,
            [this](
                javelin::app::OnboardingCallResult<javelin::app::AccountDiscoveryResult> callResult)
            {
                setBusy(false);
                if (const auto* error = std::get_if<QString>(&callResult))
                {
                    m_discoveryStatus->setText(i18n("We couldn’t check that service. %1", *error));
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
            i18n("We found a compatible service for %1.", m_discovery->emailAddress));
        m_discoveryPage->setComplete(true);
        const bool browserLogin = !m_discovery->authorizationEndpoint.isEmpty() &&
                                  !m_discovery->registrationEndpoint.isEmpty();
        m_oauthButton->setVisible(browserLogin);
        m_authenticationStatus->setText(
            browserLogin
                ? i18n("Your provider supports secure browser sign-in.")
                : i18n("This provider needs an API token or manually registered application."));
        if (!browserLogin)
            m_manualToggle->setChecked(true);
    }

    void FirstRunWizard::beginOAuth()
    {
        if (!m_discovery.has_value() || m_busy)
            return;
        cancelOAuthFlow();
        if (m_callbackServer == nullptr)
        {
            m_callbackServer = new QTcpServer(this);
            connect(m_callbackServer, &QTcpServer::newConnection, this,
                    &FirstRunWizard::handleBrowserCallback);
        }
        if (m_callbackServer->isListening())
            m_callbackServer->close();
        if (!m_callbackServer->listen(QHostAddress::LocalHost, 0) &&
            !m_callbackServer->listen(QHostAddress::LocalHostIPv6, 0))
        {
            m_authenticationStatus->setText(
                i18n("Javelin couldn’t prepare a secure browser callback."));
            return;
        }
        const bool ipv6 =
            m_callbackServer->serverAddress().protocol() == QAbstractSocket::IPv6Protocol;
        const auto redirect =
            ipv6 ? QStringLiteral("http://[::1]:%1/oauth/callback")
                       .arg(m_callbackServer->serverPort())
                 : QStringLiteral("http://127.0.0.1:%1/oauth/callback")
                       .arg(m_callbackServer->serverPort());
        setBusy(true, i18n("Preparing secure sign-in…"));
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
                m_oauthState = result.callbackState;
                m_authenticationStatus->setText(
                    i18n("Finish signing in in your browser. You can return here when it closes."));
                if (!QDesktopServices::openUrl(QUrl{result.authorizationUrl}))
                {
                    cancelOAuthFlow();
                    m_authenticationStatus->setText(
                        i18n("Your browser could not be opened. Use manual details below."));
                }
            });
    }

    void FirstRunWizard::handleBrowserCallback()
    {
        while (m_callbackServer != nullptr && m_callbackServer->hasPendingConnections())
        {
            auto* socket = m_callbackServer->nextPendingConnection();
            connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
            connect(
                socket, &QTcpSocket::readyRead, this,
                [this, socket]
                {
                    if (socket->property("javelinCallbackHandled").toBool())
                        return;

                    const auto respond = [socket](const QByteArray& status, const QString& text)
                    {
                        const auto body = text.toUtf8();
                        socket->write(QByteArrayLiteral("HTTP/1.1 ") + status +
                                      QByteArrayLiteral(
                                          "\r\nContent-Type: text/html; charset=utf-8"
                                          "\r\nCache-Control: no-store"
                                          "\r\nConnection: close\r\nContent-Length: ") +
                                      QByteArray::number(body.size()) +
                                      QByteArrayLiteral("\r\n\r\n") + body);
                        socket->disconnectFromHost();
                    };

                    auto request = socket->property("javelinCallbackRequest").toByteArray();
                    request += socket->readAll();
                    constexpr qsizetype maximumCallbackRequestBytes = 16 * 1024;
                    if (request.size() > maximumCallbackRequestBytes)
                    {
                        socket->setProperty("javelinCallbackHandled", true);
                        respond(QByteArrayLiteral("413 Payload Too Large"),
                                i18n("<h1>Invalid sign-in response</h1>"));
                        return;
                    }
                    socket->setProperty("javelinCallbackRequest", request);
                    const auto headerEnd = request.indexOf("\r\n\r\n");
                    if (headerEnd < 0)
                        return;

                    socket->setProperty("javelinCallbackHandled", true);
                    const auto headerLines = request.first(headerEnd).split('\n');
                    if (headerLines.empty())
                    {
                        respond(QByteArrayLiteral("400 Bad Request"),
                                i18n("<h1>Invalid sign-in response</h1>"));
                        return;
                    }
                    const auto requestParts = headerLines.front().trimmed().simplified().split(' ');
                    if (requestParts.size() != 3 || requestParts.at(0) != QByteArrayLiteral("GET") ||
                        !requestParts.at(2).startsWith(QByteArrayLiteral("HTTP/1.")))
                    {
                        respond(QByteArrayLiteral("405 Method Not Allowed"),
                                i18n("<h1>Invalid sign-in response</h1>"));
                        return;
                    }

                    QByteArray host;
                    for (const auto& rawLine : headerLines)
                    {
                        const auto line = rawLine.trimmed();
                        if (line.left(5).compare(QByteArrayLiteral("Host:"),
                                                 Qt::CaseInsensitive) == 0)
                        {
                            host = line.sliced(5).trimmed();
                            break;
                        }
                    }
                    const bool ipv6 = m_callbackServer != nullptr &&
                                      m_callbackServer->serverAddress().protocol() ==
                                          QAbstractSocket::IPv6Protocol;
                    QByteArray expectedHost =
                        ipv6 ? QByteArrayLiteral("[::1]:") : QByteArrayLiteral("127.0.0.1:");
                    expectedHost += QByteArray::number(m_callbackServer == nullptr
                                                           ? 0
                                                           : m_callbackServer->serverPort());
                    if (host.compare(expectedHost, Qt::CaseInsensitive) != 0)
                    {
                        respond(QByteArrayLiteral("400 Bad Request"),
                                i18n("<h1>Invalid sign-in response</h1>"));
                        return;
                    }

                    const auto target = QString::fromLatin1(requestParts.at(1));
                    const QUrl callbackUrl{(ipv6 ? QStringLiteral("http://[::1]")
                                                 : QStringLiteral("http://127.0.0.1")) +
                                           target};
                    if (!callbackUrl.isValid() ||
                        callbackUrl.path() != QStringLiteral("/oauth/callback"))
                    {
                        respond(QByteArrayLiteral("404 Not Found"),
                                i18n("<h1>Invalid sign-in response</h1>"));
                        return;
                    }

                    const QUrlQuery query{callbackUrl};
                    const auto code =
                        query.queryItemValue(QStringLiteral("code"), QUrl::FullyDecoded);
                    const auto state =
                        query.queryItemValue(QStringLiteral("state"), QUrl::FullyDecoded);
                    const auto issuer =
                        query.queryItemValue(QStringLiteral("iss"), QUrl::FullyDecoded);
                    const auto error =
                        query.queryItemValue(QStringLiteral("error"), QUrl::FullyDecoded);
                    if (state.isEmpty() || state != m_oauthState || !m_discovery.has_value() ||
                        issuer != m_discovery->issuer)
                    {
                        respond(QByteArrayLiteral("400 Bad Request"),
                                i18n("<h1>Invalid sign-in response</h1>"));
                        return;
                    }
                    if (!error.isEmpty())
                    {
                        respond(QByteArrayLiteral("200 OK"),
                                i18n("<h1>Sign-in was cancelled</h1><p>You can return to Javelin "
                                     "and try again.</p>"));
                        if (m_callbackServer != nullptr)
                            m_callbackServer->close();
                        m_authenticationStatus->setText(i18n(
                            "Sign-in was cancelled. You can try again or use manual details."));
                        cancelOAuthFlow();
                        return;
                    }
                    if (code.isEmpty())
                    {
                        respond(QByteArrayLiteral("400 Bad Request"),
                                i18n("<h1>Invalid sign-in response</h1>"));
                        return;
                    }

                    respond(QByteArrayLiteral("200 OK"),
                            i18n("<h1>Sign-in received</h1><p>You can close this tab and return to "
                                 "Javelin.</p>"));
                    if (m_callbackServer != nullptr)
                        m_callbackServer->close();
                    setBusy(true, i18n("Finishing sign-in…"));
                    auto task = m_onboarding.finishOAuth(
                        {.flowId = m_oauthFlowId, .code = code, .state = state, .issuer = issuer});
                    QCoro::connect(
                        std::move(task), this,
                        [this](javelin::app::OnboardingCallResult<
                               javelin::app::AccountAuthenticationResult>
                                   callResult)
                        {
                            setBusy(false);
                            if (const auto* callError = std::get_if<QString>(&callResult))
                            {
                                m_authenticationStatus->setText(*callError);
                                cancelOAuthFlow();
                                return;
                            }
                            m_oauthFlowId.clear();
                            m_oauthState.clear();
                            auto result = std::get<javelin::app::AccountAuthenticationResult>(
                                std::move(callResult));
                            logOAuthResult("OAuth result received by wizard", result);
                            m_oauthAuthentication = true;
                            completeAuthentication(std::move(result));
                        });
                });
        }
    }

    void FirstRunWizard::beginManualAuthentication()
    {
        if (m_busy || m_tokenEdit->text().trimmed().isEmpty())
        {
            m_authenticationStatus->setText(i18n("Enter an API or access token first."));
            return;
        }
        setBusy(true, i18n("Checking those details…"));
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
                m_oauthAuthentication = false;
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
        m_authenticationStatus->setText(i18n("✓ Signed in successfully"));
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
            const auto suffix =
                pending ? i18nc("@item suffix for onboarding feature", " — checked after sign-in")
                        : QString{};
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
        std::optional<javelin::app::OAuthRevocationRequest> previousOAuth;
        QString savedConnectionId;
        if (m_connectionId.isEmpty())
        {
            savedConnectionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            accounts.push_back({
                .id = savedConnectionId,
                .revision = 1,
                .displayName = m_nameEdit->text().trimmed(),
                .sessionUrl = m_authentication->sessionUrl,
                .loginEmail = m_emailEdit->text().trimmed(),
                .apiKey = m_authentication->accessToken,
                .refreshToken = m_authentication->refreshToken,
                .tokenEndpoint = m_authentication->tokenEndpoint,
                .oauthClientId = m_authentication->clientId,
                .oauthIssuer = m_authentication->issuer,
                .oauthResource = m_authentication->resourceUrl,
                .oauthScope = m_authentication->scope,
                .revocationEndpoint = m_authentication->revocationEndpoint,
                .registrationClientUri = m_authentication->registrationClientUri,
                .registrationAccessToken = m_authentication->registrationAccessToken,
                .tokenExpiresAtEpochSeconds = m_authentication->expiresAtEpochSeconds,
                .reauthenticationRequired = false,
                .cachedAccountIds = {},
            });
        }
        else
        {
            savedConnectionId = m_connectionId;
            const auto account = std::ranges::find(accounts, m_connectionId,
                                                   &javelin::gui::settings::ConnectionSettings::id);
            if (account == accounts.end())
            {
                QMessageBox::warning(this, i18n("Couldn’t update the account"),
                                     i18n("This account is no longer configured."));
                return;
            }
            previousOAuth = javelin::app::OAuthRevocationRequest{
                .revocationEndpoint = account->revocationEndpoint,
                .clientId = account->oauthClientId,
                .accessToken = account->apiKey,
                .refreshToken = account->refreshToken,
                .registrationClientUri = account->registrationClientUri,
                .registrationAccessToken = account->registrationAccessToken,
            };
            ++account->revision;
            account->displayName = m_nameEdit->text().trimmed();
            account->sessionUrl = m_authentication->sessionUrl;
            account->loginEmail = m_emailEdit->text().trimmed();
            account->apiKey = m_authentication->accessToken;
            account->refreshToken = m_authentication->refreshToken;
            account->tokenEndpoint = m_authentication->tokenEndpoint;
            account->oauthClientId = m_authentication->clientId;
            account->oauthIssuer = m_authentication->issuer;
            account->oauthResource = m_authentication->resourceUrl;
            account->oauthScope = m_authentication->scope;
            account->revocationEndpoint = m_authentication->revocationEndpoint;
            account->registrationClientUri = m_authentication->registrationClientUri;
            account->registrationAccessToken = m_authentication->registrationAccessToken;
            account->tokenExpiresAtEpochSeconds = m_authentication->expiresAtEpochSeconds;
            account->reauthenticationRequired = false;
        }
        if (m_oauthAuthentication)
        {
            const auto account = std::ranges::find(accounts, savedConnectionId,
                                                   &javelin::gui::settings::ConnectionSettings::id);
            if (account != accounts.end())
                logOAuthSettings("OAuth credentials submitted to settings", *account);
        }
        javelin::protocol::SettingsUpdate update;
        update.accounts = javelin::gui::settings::GuiSettings::protocolAccounts(accounts);
        if (const auto error = m_settings.update(std::move(update)))
        {
            QMessageBox::warning(this, i18n("Couldn’t save the account"),
                                 i18n("Javelin couldn’t save your account yet. Please try again."));
            return;
        }
        if (previousOAuth.has_value() &&
            (previousOAuth->clientId != m_authentication->clientId ||
             previousOAuth->registrationClientUri != m_authentication->registrationClientUri))
        {
            auto cleanup = m_onboarding.revokeOAuth(std::move(*previousOAuth));
            QCoro::connect(
                std::move(cleanup), QCoreApplication::instance(),
                [](javelin::app::OnboardingCallResult<javelin::app::OAuthRevocationResult> result)
                {
                    if (const auto* error = std::get_if<QString>(&result))
                        qCWarning(onboardingOAuthLog).noquote()
                            << "Previous OAuth authorization cleanup failed" << *error;
                    else if (const auto& revoked =
                                 std::get<javelin::app::OAuthRevocationResult>(result);
                             revoked.attempted && !revoked.succeeded)
                        qCWarning(onboardingOAuthLog).noquote()
                            << "Previous OAuth authorization cleanup failed" << revoked.error;
                });
        }
        if (m_oauthAuthentication)
        {
            const auto persistedAccounts = m_settings.accounts();
            const auto account = std::ranges::find(persistedAccounts, savedConnectionId,
                                                   &javelin::gui::settings::ConnectionSettings::id);
            if (account != persistedAccounts.end())
                logOAuthSettings("OAuth credentials present after settings update", *account);
            else
                qCWarning(onboardingOAuthLog).noquote()
                    << "OAuth account missing after settings update"
                    << "connection=" << savedConnectionId;
        }
        QWizard::accept();
    }
} // namespace javelin::gui::onboarding
