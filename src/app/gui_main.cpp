#include "app/GuiDaemonSession.h"
#include "app/GuiServices.h"
#include "app/InlineMessageSchemeHandler.h"
#include "app/LogStore.h"
#include "app/MessageNavigationPort.h"
#include "app/WorkTaskPort.h"

#include "gui/shell/MainWindow.h"
#include "gui/tasks/TaskCenterDialog.h"

#include <KAboutData>
#include <KAboutLicense>

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QEvent>
#include <QIcon>
#include <QLabel>
#include <QLocale>
#include <QMainWindow>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QToolButton>
#include <QTranslator>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdlib>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#ifndef JAVELIN_APP_VERSION
#define JAVELIN_APP_VERSION "0.0.0"
#endif

#ifndef JAVELIN_DATA_DIR
#define JAVELIN_DATA_DIR ""
#endif

namespace
{
    [[nodiscard]] QString runtimeDirectory()
    {
        return QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    }

    void configureLocalDataDirectory()
    {
        constexpr auto localDataDir = JAVELIN_DATA_DIR;
        if constexpr (localDataDir[0] != '\0')
        {
            const char* existingDirs = std::getenv("XDG_DATA_DIRS");
            const QString dataDirs = existingDirs
                                         ? QString::fromLatin1(localDataDir) + QLatin1Char(':') +
                                               QString::fromLocal8Bit(existingDirs)
                                         : QString::fromLatin1(localDataDir);
            qputenv("XDG_DATA_DIRS", dataDirs.toUtf8());
        }
    }

    class ActivationTokenScope final
    {
      public:
        explicit ActivationTokenScope(const QString& activationToken)
        {
            if (activationToken.isEmpty())
                return;
            m_previous = qgetenv("XDG_ACTIVATION_TOKEN");
            m_hadPrevious = !m_previous.isNull();
            qputenv("XDG_ACTIVATION_TOKEN", activationToken.toUtf8());
        }

        ~ActivationTokenScope()
        {
            if (m_hadPrevious)
                qputenv("XDG_ACTIVATION_TOKEN", m_previous);
            else
                qunsetenv("XDG_ACTIVATION_TOKEN");
        }

      private:
        QByteArray m_previous;
        bool m_hadPrevious = false;
    };
} // namespace

int main(int argc, char* argv[])
{
    configureLocalDataDirectory();
    javelin::app::registerInlineMessageUrlScheme();

    QApplication application{argc, argv};
    javelin::app::LogStore::install();
    QCoreApplication::setOrganizationName(QStringLiteral("Javelin Mail"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("javelin.app"));
    QCoreApplication::setApplicationName(QStringLiteral("Javelin Mail"));
    QCoreApplication::setApplicationVersion(QStringLiteral(JAVELIN_APP_VERSION));

    KAboutData aboutData(QStringLiteral("javelinmail"), QStringLiteral("Javelin Mail"),
                         QStringLiteral(JAVELIN_APP_VERSION), QStringLiteral("A JMAP email client"),
                         KAboutLicense::GPL_V3);
    aboutData.setOrganizationDomain("javelin.app");
    KAboutData::setApplicationData(aboutData);
    application.setWindowIcon(QIcon(QStringLiteral(":/icons/icon.svg")));
    application.setQuitOnLastWindowClosed(false);

    QTranslator translator;
    for (const QString& locale : QLocale::system().uiLanguages())
    {
        const QString baseName = QStringLiteral("Javelin-Mail_") + QLocale(locale).name();
        if (translator.load(QStringLiteral(":/i18n/") + baseName))
        {
            application.installTranslator(&translator);
            break;
        }
    }

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption runtimeOption{QStringLiteral("runtime-directory"),
                                           QStringLiteral("Private runtime directory."),
                                           QStringLiteral("directory")};
    const QCommandLineOption socketOption{
        QStringLiteral("socket"), QStringLiteral("Daemon socket path."), QStringLiteral("path")};
    parser.addOption(runtimeOption);
    parser.addOption(socketOption);
    parser.process(application);

    const auto runtime =
        parser.isSet(runtimeOption) ? parser.value(runtimeOption) : runtimeDirectory();
    if (runtime.isEmpty())
    {
        qCritical() << QStringLiteral("No private runtime directory is available.");
        return 1;
    }

    const auto socketPath = parser.isSet(socketOption)
                                ? parser.value(socketOption)
                                : QDir{runtime}.filePath(QStringLiteral("javelind.sock"));
    auto activationOptions = javelin::protocol::SocketClientOptions{
        .runtimeDirectory = runtime,
        .socketPath = socketPath + QStringLiteral(".activation"),
        .limits = {},
        .protocol = {.major = 2, .minor = 0},
        .expectedBuild =
            javelin::protocol::BuildIdentity{.application = QStringLiteral("Javelin-Mail"),
                                             .revision = QStringLiteral(JAVELIN_APP_VERSION)},
        .maximumQueuedFrames = 1,
        .maximumQueuedBytes = 4096,
        .responseTimeoutMilliseconds = 100,
        .enforcePeerCredentials = true,
    };
    const auto activation = javelin::protocol::SocketActivationClient::request(
        activationOptions, javelin::protocol::RaiseGuiRoute{});
    if (const auto* activationReply =
            std::get_if<std::optional<javelin::protocol::BoundaryError>>(&activation);
        activationReply != nullptr && !activationReply->has_value())
    {
        return 0;
    }

    javelin::app::GuiDaemonSession session{
        {.runtimeDirectory = runtime,
         .socketPath = socketPath,
         .daemonExecutable =
             QDir{QCoreApplication::applicationDirPath()}.filePath(QStringLiteral("javelind")),
         .protocol = {.major = 2, .minor = 0},
         .build = {.application = QStringLiteral("Javelin-Mail"),
                   .revision = QStringLiteral(JAVELIN_APP_VERSION)},
         .startTimeoutMilliseconds = 5000,
         .startDaemonIfMissing = true}};

    QMainWindow recoveryWindow;
    recoveryWindow.setWindowTitle(QStringLiteral("Javelin Mail — reconnecting"));
    recoveryWindow.setWindowIcon(application.windowIcon());
    auto* recoveryCentral = new QWidget(&recoveryWindow);
    auto* recoveryLayout = new QVBoxLayout(recoveryCentral);
    auto* recoveryStatus = new QLabel(recoveryCentral);
    recoveryStatus->setWordWrap(true);
    auto* retry = new QPushButton(QStringLiteral("Retry daemon connection"), recoveryCentral);
    recoveryLayout->addWidget(recoveryStatus);
    recoveryLayout->addWidget(retry);
    recoveryWindow.setCentralWidget(recoveryCentral);
    recoveryWindow.resize(520, 160);

    std::unique_ptr<javelin::app::GuiServices> services;
    QPointer<javelin::gui::shell::MainWindow> mainWindow;
    QPointer<javelin::gui::tasks::TaskCenterDialog> taskCenter;

    const auto showRecovery = [&recoveryWindow, recoveryStatus, retry](const QString& detail)
    {
        recoveryStatus->setText(
            QStringLiteral("The Javelin daemon is unavailable: %1").arg(detail));
        retry->setEnabled(true);
        recoveryWindow.show();
        recoveryWindow.raise();
        recoveryWindow.activateWindow();
    };

    std::function<void(const QString&)> restoreMainWindow;
    std::function<void()> showTaskCenter;
    const auto createMainWindow = [&]
    {
        if (mainWindow != nullptr)
            return;
        if (!services)
            services = std::make_unique<javelin::app::GuiServices>(session);

        mainWindow = new javelin::gui::shell::MainWindow(
            services->accountCommandPort(), services->accountReader(), services->mailboxReader(),
            services->contactReader(), services->calendarReader(), services->calendarCommandPort(),
            services->contactIdentityLookup(), services->identityReader(),
            services->messageViewReader(), services->queryReader(), services->translationPort(),
            services->composeCommandPort(), services->contactCommandPort(),
            services->mailCommandPort(), services->sieveCommandPort(),
            services->accountRefreshPort(), services->messageContentPort(),
            services->messageListSessionFactory(), services->mailEvents(),
            services->messageNavigationPort(), services->undoCommandPort());
        mainWindow->setAttribute(Qt::WA_DeleteOnClose);

        auto* taskButton = new QToolButton(mainWindow);
        taskButton->setAutoRaise(true);
        taskButton->setToolTip(QStringLiteral("Open Task Center"));
        mainWindow->statusBar()->addPermanentWidget(taskButton);
        const auto updateTaskButton = [&, taskButton]
        {
            if (!services || mainWindow == nullptr)
                return;
            const QString summary = services->workTaskPort().summary();
            taskButton->setText(summary);
            taskButton->setVisible(!summary.isEmpty());
        };
        QObject::connect(taskButton, &QToolButton::clicked, mainWindow,
                         [&showTaskCenter] { showTaskCenter(); });
        static_cast<void>(services->workTaskPort().connectChanged(mainWindow, updateTaskButton));
        updateTaskButton();

        QObject::connect(
            mainWindow, &javelin::gui::shell::MainWindow::accountSettingsChanged, mainWindow,
            [&services, &mainWindow]
            {
                if (!services || mainWindow == nullptr)
                    return;
                if (const auto error = services->reloadDaemonSettings())
                {
                    QMessageBox::critical(
                        mainWindow, QStringLiteral("Settings update failed"),
                        QStringLiteral("The daemon could not apply the updated settings: %1")
                            .arg(error->detail));
                }
            });
        QObject::connect(mainWindow, &QObject::destroyed, &application,
                         [&application, &mainWindow]
                         {
                             mainWindow = nullptr;
                             application.quit();
                         });
    };

    restoreMainWindow = [&](const QString& activationToken)
    {
        createMainWindow();
        if (mainWindow == nullptr)
            return;
        ActivationTokenScope tokenScope{activationToken};
        recoveryWindow.hide();
        mainWindow->setEnabled(true);
        mainWindow->show();
        if (mainWindow->isMinimized())
            mainWindow->showNormal();
        mainWindow->raise();
        mainWindow->activateWindow();
    };

    showTaskCenter = [&]
    {
        restoreMainWindow({});
        if (mainWindow == nullptr || !services)
            return;
        if (taskCenter == nullptr)
        {
            taskCenter =
                new javelin::gui::tasks::TaskCenterDialog(services->workTaskPort(), mainWindow);
            taskCenter->setAttribute(Qt::WA_DeleteOnClose);
        }
        taskCenter->show();
        taskCenter->raise();
        taskCenter->activateWindow();
    };

    const auto handleActivation = [&](const javelin::protocol::ActivationRoute& route)
    {
        std::visit(
            [&](const auto& activationRoute)
            {
                using Route = std::decay_t<decltype(activationRoute)>;
                if constexpr (std::is_same_v<Route, javelin::protocol::OpenMessageRoute>)
                {
                    restoreMainWindow(activationRoute.activationToken);
                    const auto thread =
                        activationRoute.threadId.isEmpty()
                            ? std::optional<std::string>{}
                            : std::optional<std::string>{activationRoute.threadId.toStdString()};
                    static_cast<void>(services->messageNavigationPort().openEmail(
                        activationRoute.accountId.toStdString(),
                        activationRoute.mailboxId.toStdString(), thread,
                        activationRoute.emailId.toStdString()));
                }
                else if constexpr (std::is_same_v<Route, javelin::protocol::OpenSettingsRoute>)
                {
                    restoreMainWindow(activationRoute.activationToken);
                    mainWindow->openPreferencesForConnection(activationRoute.connectionId);
                }
                else if constexpr (std::is_same_v<Route, javelin::protocol::RestoreDraftRoute>)
                {
                    restoreMainWindow(activationRoute.activationToken);
                    mainWindow->restoreDraft(activationRoute.accountId,
                                             activationRoute.draftEmailId,
                                             activationRoute.composeSessionId);
                }
                else if constexpr (std::is_same_v<Route, javelin::protocol::OpenTaskCenterRoute>)
                {
                    restoreMainWindow(activationRoute.activationToken);
                    showTaskCenter();
                }
                else if constexpr (requires { activationRoute.activationToken; })
                {
                    restoreMainWindow(activationRoute.activationToken);
                }
                else
                {
                    restoreMainWindow({});
                }
            },
            route);
    };

    QObject::connect(&session, &javelin::app::GuiDaemonSession::recoveryStarted, &recoveryWindow,
                     [&](const QString& detail)
                     {
                         if (mainWindow != nullptr)
                             mainWindow->setEnabled(false);
                         showRecovery(detail);
                     });
    QObject::connect(&session, &javelin::app::GuiDaemonSession::recoveryFinished, &recoveryWindow,
                     [&] { restoreMainWindow({}); });
    QObject::connect(&session, &javelin::app::GuiDaemonSession::daemonShutdownRequested,
                     &application, &QCoreApplication::quit);
    QObject::connect(&session, &javelin::app::GuiDaemonSession::activationRequested,
                     &recoveryWindow, handleActivation);
    QObject::connect(retry, &QPushButton::clicked, &recoveryWindow,
                     [&]
                     {
                         retry->setEnabled(false);
                         if (const auto error = session.reconnect())
                             showRecovery(error->detail);
                         else
                             restoreMainWindow({});
                     });

    if (const auto error = session.start())
        showRecovery(error->detail);
    else
        restoreMainWindow({});

    return application.exec();
}
