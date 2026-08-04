#include "app/GuiDaemonSession.h"
#include "app/GuiServices.h"
#include "app/InlineMessageSchemeHandler.h"
#include "app/LogStore.h"
#include "app/MessageNavigationPort.h"
#include "app/PerformanceMetrics.h"
#include "app/ProcessInstanceLock.h"
#include "app/WorkTaskPort.h"

#include "gui/onboarding/FirstRunWizard.h"
#include "gui/settings/GuiSettings.h"
#include "gui/shell/MainWindow.h"
#include "gui/tasks/TaskCenterDialog.h"

#include <KAboutData>
#include <KAboutLicense>

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QIcon>
#include <QLabel>
#include <QLocale>
#include <QLockFile>
#include <QMainWindow>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QStandardPaths>
#include <QStatusBar>
#include <QThread>
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

    class ProfilingApplication final : public QApplication
    {
      public:
        using QApplication::QApplication;

        bool notify(QObject* receiver, QEvent* event) override
        {
            if (!javelin::app::PerformanceMetrics::enabled())
                return QApplication::notify(receiver, event);

            const int eventType = event == nullptr ? 0 : static_cast<int>(event->type());
            const auto receiverName =
                receiver == nullptr ? QStringLiteral("null")
                                    : QString::fromLatin1(receiver->metaObject()->className());
            QElapsedTimer timer;
            timer.start();
            const bool delivered = QApplication::notify(receiver, event);
            if (timer.elapsed() > 50)
            {
                javelin::app::PerformanceMetrics::recordDuration(
                    QStringLiteral("gui"), QStringLiteral("qt_event_handler"),
                    std::chrono::microseconds{timer.elapsed() * 1000}, QStringLiteral("slow"),
                    QStringLiteral("event_type=%1 receiver=%2").arg(eventType).arg(receiverName));
            }
            return delivered;
        }

        void startProfiling()
        {
            if (!javelin::app::PerformanceMetrics::enabled())
                return;

            m_heartbeatTimer.setInterval(50);
            connect(&m_heartbeatTimer, &QTimer::timeout, this,
                    [this]
                    {
                        const auto interval = m_heartbeatClock.restart();
                        if (interval <= 200)
                            return;
                        javelin::app::PerformanceMetrics::recordDuration(
                            QStringLiteral("gui"), QStringLiteral("event_loop_stall"),
                            std::chrono::microseconds{interval * 1000}, QStringLiteral("stalled"),
                            QStringLiteral("heartbeat_interval_ms=50"));
                    });
            m_heartbeatClock.start();
            m_heartbeatTimer.start();
        }

      private:
        QElapsedTimer m_heartbeatClock;
        QTimer m_heartbeatTimer;
    };
} // namespace

int main(int argc, char* argv[])
{
    configureLocalDataDirectory();
    javelin::app::registerInlineMessageUrlScheme();

    ProfilingApplication application{argc, argv};
    javelin::app::LogStore::install();
    application.startProfiling();
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

    QTimer resourceTimer;
    if (javelin::app::PerformanceMetrics::enabled())
    {
        javelin::app::PerformanceMetrics::recordProcessResources(QStringLiteral("gui"));
        resourceTimer.setInterval(30'000);
        QObject::connect(
            &resourceTimer, &QTimer::timeout, &application, []
            { javelin::app::PerformanceMetrics::recordProcessResources(QStringLiteral("gui")); });
        QObject::connect(
            &application, &QCoreApplication::aboutToQuit, &application, []
            { javelin::app::PerformanceMetrics::recordProcessResources(QStringLiteral("gui")); });
        resourceTimer.start();
    }

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
        .protocol = {.major = 4, .minor = 0},
        .expectedBuild =
            javelin::protocol::BuildIdentity{.application = QStringLiteral("Javelin-Mail"),
                                             .revision = QStringLiteral(JAVELIN_APP_VERSION)},
        .maximumQueuedFrames = 1,
        .maximumQueuedBytes = 4096,
        .responseTimeoutMilliseconds = 100,
        .enforcePeerCredentials = true,
    };

    QLockFile guiInstanceLock{QDir{runtime}.filePath(QStringLiteral("javelin.lock"))};
    guiInstanceLock.setStaleLockTime(0);
    const auto requestRaiseGui = [&activationOptions]
    {
        return javelin::protocol::SocketActivationClient::request(
            activationOptions, javelin::protocol::RaiseGuiRoute{});
    };

    if (!guiInstanceLock.tryLock(0))
    {
        if (guiInstanceLock.error() != QLockFile::LockFailedError)
        {
            qCritical().noquote() << QStringLiteral("Could not acquire GUI instance lock:")
                                  << static_cast<int>(guiInstanceLock.error());
            return 1;
        }

        QElapsedTimer activationWait;
        activationWait.start();
        constexpr qint64 activationWaitTimeoutMilliseconds = 5000;
        for (;;)
        {
            const auto activation = requestRaiseGui();
            if (const auto* activationReply =
                    std::get_if<std::optional<javelin::protocol::BoundaryError>>(&activation);
                activationReply != nullptr && !activationReply->has_value())
            {
                return 0;
            }

            if (guiInstanceLock.tryLock(0))
                break;

            const auto recovery = javelin::app::recoverAbandonedProcessLock(
                guiInstanceLock, QStringLiteral("javelin"));
            if (recovery == javelin::app::ProcessLockRecoveryResult::RemovalFailed)
            {
                qCritical() << QStringLiteral("Could not remove stale GUI instance lock.");
                return 1;
            }
            if (recovery == javelin::app::ProcessLockRecoveryResult::Removed)
            {
                if (guiInstanceLock.tryLock(0))
                {
                    qInfo() << QStringLiteral("Removed stale GUI instance lock");
                    break;
                }
                if (guiInstanceLock.error() != QLockFile::LockFailedError)
                {
                    qCritical().noquote() << QStringLiteral("Could not acquire GUI instance lock:")
                                          << static_cast<int>(guiInstanceLock.error());
                    return 1;
                }
            }

            if (activationWait.elapsed() >= activationWaitTimeoutMilliseconds)
            {
                qint64 pid = 0;
                QString hostname;
                QString applicationName;
                auto detail = QStringLiteral("another Javelin GUI instance is already running");
                if (guiInstanceLock.getLockInfo(&pid, &hostname, &applicationName))
                    detail += QStringLiteral(" (pid %1)").arg(pid);
                qCritical().noquote() << detail;
                return 1;
            }
            QThread::msleep(50);
        }
    }

    const auto activation = requestRaiseGui();
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
         .protocol = {.major = 4, .minor = 0},
         .build = {.application = QStringLiteral("Javelin-Mail"),
                   .revision = QStringLiteral(JAVELIN_APP_VERSION)},
         .startTimeoutMilliseconds = 5000,
         .startDaemonIfMissing = false}};

    QMainWindow recoveryWindow;
    recoveryWindow.setWindowTitle(QStringLiteral("Welcome to Javelin Mail"));
    recoveryWindow.setWindowIcon(application.windowIcon());
    auto* recoveryCentral = new QWidget(&recoveryWindow);
    auto* recoveryLayout = new QVBoxLayout(recoveryCentral);
    auto* recoveryStatus = new QLabel(recoveryCentral);
    recoveryStatus->setWordWrap(true);
    auto* enableAndStartDaemon =
        new QPushButton(QStringLiteral("Enable background sync"), recoveryCentral);
    auto* startDaemon = new QPushButton(QStringLiteral("Start Javelin now"), recoveryCentral);
    auto* retry = new QPushButton(QStringLiteral("Retry daemon connection"), recoveryCentral);
    recoveryLayout->addWidget(recoveryStatus);
    recoveryLayout->addWidget(enableAndStartDaemon);
    recoveryLayout->addWidget(startDaemon);
    recoveryLayout->addWidget(retry);
    recoveryWindow.setCentralWidget(recoveryCentral);
    recoveryWindow.resize(520, 230);

    std::unique_ptr<javelin::app::GuiServices> services;
    QPointer<javelin::gui::shell::MainWindow> mainWindow;
    QPointer<javelin::gui::tasks::TaskCenterDialog> taskCenter;
    QPointer<javelin::gui::onboarding::FirstRunWizard> firstRunWizard;

    const auto showRecovery = [&recoveryWindow, recoveryStatus, enableAndStartDaemon, startDaemon,
                               retry, &session](const QString&, const bool offerDaemonStart)
    {
        recoveryStatus->setText(
            offerDaemonStart
                ? QStringLiteral(
                      "Javelin’s background sync service isn’t running yet. Start it once, or "
                      "enable it so mail stays up to date whenever you sign in.")
                : QStringLiteral(
                      "Javelin couldn’t open its background sync service. Please try again."));
        enableAndStartDaemon->setVisible(offerDaemonStart && session.canUseSystemdUserService());
        startDaemon->setVisible(offerDaemonStart);
        enableAndStartDaemon->setEnabled(offerDaemonStart);
        startDaemon->setEnabled(offerDaemonStart);
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
            services->settings(), services->accountCommandPort(), services->accountReader(),
            services->mailboxReader(), services->contactReader(), services->calendarReader(),
            services->calendarCommandPort(), services->contactIdentityLookup(),
            services->identityReader(), services->messageViewReader(), services->queryReader(),
            services->translationService(), services->composeCommandPort(),
            services->contactCommandPort(), services->mailCommandPort(),
            services->sieveCommandPort(), services->accountRefreshPort(),
            services->onboardingPort(), services->messageContentPort(),
            services->messageListSessionFactory(), services->mailEvents(),
            services->messageNavigationPort(), services->undoCommandPort());
        mainWindow->setAttribute(Qt::WA_DeleteOnClose);

        auto* taskButton = new QToolButton(mainWindow);
        taskButton->setAutoRaise(true);
        taskButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
        taskButton->setToolTip(QStringLiteral("Open Task Center"));
        mainWindow->statusBar()->addPermanentWidget(taskButton);
        const auto updateTaskButton = [&, taskButton]
        {
            if (!services || mainWindow == nullptr)
                return;
            const QString summary = services->workTaskPort().summary();
            taskButton->setText(summary);
            taskButton->setAccessibleName(summary);
            taskButton->setVisible(!summary.isEmpty());
        };
        QObject::connect(taskButton, &QToolButton::clicked, mainWindow,
                         [&showTaskCenter] { showTaskCenter(); });
        static_cast<void>(services->workTaskPort().connectChanged(mainWindow, updateTaskButton));
        updateTaskButton();

        QObject::connect(mainWindow, &QObject::destroyed, &application,
                         [&application, &mainWindow]
                         {
                             mainWindow = nullptr;
                             application.quit();
                         });
    };

    restoreMainWindow = [&](const QString& activationToken)
    {
        if (!services)
            services = std::make_unique<javelin::app::GuiServices>(session);
        if (services->settings().accounts().empty())
        {
            recoveryWindow.hide();
            if (firstRunWizard == nullptr)
            {
                firstRunWizard = new javelin::gui::onboarding::FirstRunWizard(
                    services->onboardingPort(), services->settings());
                firstRunWizard->setAttribute(Qt::WA_DeleteOnClose);
                QObject::connect(firstRunWizard, &QWizard::accepted, &application,
                                 [&]
                                 {
                                     firstRunWizard = nullptr;
                                     restoreMainWindow({});
                                 });
                QObject::connect(firstRunWizard, &QWizard::rejected, &application,
                                 &QCoreApplication::quit);
            }
            firstRunWizard->show();
            firstRunWizard->raise();
            firstRunWizard->activateWindow();
            return;
        }
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
                    const auto mailboxName =
                        activationRoute.mailboxName.isEmpty()
                            ? std::optional<std::string>{}
                            : std::optional<std::string>{activationRoute.mailboxName.toStdString()};
                    static_cast<void>(services->messageNavigationPort().openEmail(
                        activationRoute.accountId.toStdString(),
                        activationRoute.mailboxId.toStdString(), thread,
                        activationRoute.emailId.toStdString(), mailboxName));
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
                         showRecovery(detail, true);
                     });
    QObject::connect(&session, &javelin::app::GuiDaemonSession::recoveryFinished, &recoveryWindow,
                     [&] { restoreMainWindow({}); });
    QObject::connect(&session, &javelin::app::GuiDaemonSession::daemonShutdownRequested,
                     &application, &QCoreApplication::quit);
    QObject::connect(&session, &javelin::app::GuiDaemonSession::activationRequested,
                     &recoveryWindow, handleActivation);

    const auto canOfferDaemonStart = [](const javelin::app::GuiBootstrapError& error)
    {
        return error.code == javelin::app::GuiBootstrapErrorCode::DaemonUnavailable ||
               error.code == javelin::app::GuiBootstrapErrorCode::DaemonStartFailed;
    };

    const auto attemptDaemonStart = [&](const javelin::app::GuiDaemonStartMode mode)
    {
        enableAndStartDaemon->setEnabled(false);
        startDaemon->setEnabled(false);
        retry->setEnabled(false);
        recoveryStatus->setText(QStringLiteral("Starting Javelin…"));
        recoveryWindow.show();
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        if (const auto error = session.startDaemon(mode))
            showRecovery(error->detail, canOfferDaemonStart(*error));
        else
            restoreMainWindow({});
    };

    QObject::connect(enableAndStartDaemon, &QPushButton::clicked, &recoveryWindow,
                     [&attemptDaemonStart]
                     { attemptDaemonStart(javelin::app::GuiDaemonStartMode::EnableAndStart); });
    QObject::connect(startDaemon, &QPushButton::clicked, &recoveryWindow, [&attemptDaemonStart]
                     { attemptDaemonStart(javelin::app::GuiDaemonStartMode::Once); });

    QObject::connect(retry, &QPushButton::clicked, &recoveryWindow,
                     [&]
                     {
                         retry->setEnabled(false);
                         if (const auto error = session.reconnect())
                             showRecovery(error->detail, canOfferDaemonStart(*error));
                         else
                             restoreMainWindow({});
                     });

    if (const auto error = session.start())
        showRecovery(error->detail, canOfferDaemonStart(*error));
    else
        restoreMainWindow({});

    return application.exec();
}
