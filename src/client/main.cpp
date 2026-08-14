#include "app/AccountRefreshApplicationPorts.h"
#include "app/LogStore.h"
#include "app/MessageNavigationPort.h"
#include "app/PerformanceMetrics.h"
#include "app/ProcessInstanceLock.h"
#include "app/WorkTaskPort.h"
#include "client/GuiDaemonSession.h"
#include "client/GuiServices.h"
#include "gui/compose/UndoSendDialog.h"
#include "gui/messageview/InlineMessageSchemeHandler.h"

#include "protocol/LocalActivationClient.h"
#include "protocol/LocalActivationServer.h"

#include "gui/onboarding/FirstRunWizard.h"
#include "gui/settings/GuiSettings.h"
#include "gui/shell/CalendarTabController.h"
#include "gui/shell/ComposeTabController.h"
#include "gui/shell/ContactsTabController.h"
#include "gui/shell/MainWindow.h"
#include "gui/tasks/TaskCenterDialog.h"

#include <KAboutData>
#include <KAboutLicense>
#include <KLocalizedString>

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QHash>
#include <QIcon>
#include <QLabel>
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
#include <QVBoxLayout>
#include <QWidget>

#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

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

    class GuiActivationHandler final : public javelin::protocol::ActivationRequestHandler
    {
      public:
        using Handler = std::function<void(const javelin::protocol::ActivationRoute&)>;

        [[nodiscard]] std::optional<javelin::protocol::BoundaryError>
        handleGuiActivation(const javelin::protocol::ActivationRoute& route) override
        {
            if (m_handler)
            {
                m_handler(route);
                return std::nullopt;
            }

            constexpr std::size_t maximumPendingActivations = 64;
            if (m_pending.size() >= maximumPendingActivations)
                m_pending.erase(m_pending.begin());
            m_pending.push_back(route);
            return std::nullopt;
        }

        void setHandler(Handler handler)
        {
            m_handler = std::move(handler);
            for (const auto& route : std::exchange(m_pending, {}))
                m_handler(route);
        }

        void clearHandler()
        {
            m_handler = {};
        }

      private:
        Handler m_handler;
        std::vector<javelin::protocol::ActivationRoute> m_pending;
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
    KLocalizedString::setApplicationDomain(QByteArrayLiteral("javelinmail"));
    javelin::app::LogStore::install();
    application.startProfiling();
    QCoreApplication::setOrganizationName(QStringLiteral("Javelin Mail"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("javelin.app"));
    QCoreApplication::setApplicationName(QStringLiteral("Javelin Mail"));
    QCoreApplication::setApplicationVersion(QStringLiteral(JAVELIN_APP_VERSION));

    KAboutData aboutData(QStringLiteral("javelinmail"), i18n("Javelin Mail"),
                         QStringLiteral(JAVELIN_APP_VERSION), i18n("A JMAP email client"),
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

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption runtimeOption{QStringLiteral("runtime-directory"),
                                           i18n("Private runtime directory."),
                                           i18nc("@info:shell command-line value", "directory")};
    const QCommandLineOption socketOption{QStringLiteral("socket"), i18n("Daemon socket path."),
                                          i18nc("@info:shell command-line value", "path")};
    parser.addOption(runtimeOption);
    parser.addOption(socketOption);
    parser.addPositionalArgument(QStringLiteral("mailto"), i18n("A mailto: link to compose."),
                                 QStringLiteral("[mailto-url]"));
    parser.process(application);

    const auto positionalArguments = parser.positionalArguments();
    if (positionalArguments.size() > 1)
    {
        qCritical() << QStringLiteral("Only one mailto: link may be opened at a time.");
        return 1;
    }
    const auto mailtoUri = positionalArguments.isEmpty() ? QString{} : positionalArguments.front();
    if (!mailtoUri.isEmpty() &&
        !mailtoUri.startsWith(QStringLiteral("mailto:"), Qt::CaseInsensitive))
    {
        qCritical() << QStringLiteral("The positional argument must be a mailto: link.");
        return 1;
    }

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
        .protocol = {.major = 5, .minor = 10},
        .expectedBuild =
            javelin::protocol::BuildIdentity{.application = QStringLiteral("Javelin-Mail"),
                                             .revision = QStringLiteral(JAVELIN_APP_VERSION)},
        .maximumQueuedFrames = 1,
        .maximumQueuedBytes = 4096,
        .responseTimeoutMilliseconds = 100,
        .enforcePeerCredentials = true,
    };

    const auto launchActivationToken = QString::fromUtf8(qgetenv("XDG_ACTIVATION_TOKEN"));
    const javelin::protocol::ActivationRoute requestedActivation =
        mailtoUri.isEmpty() ? javelin::protocol::ActivationRoute{javelin::protocol::RaiseGuiRoute{
                                  .activationToken = launchActivationToken}}
                            : javelin::protocol::ActivationRoute{javelin::protocol::OpenMailtoRoute{
                                  .uri = mailtoUri, .activationToken = launchActivationToken}};

    auto guiActivationOptions = activationOptions;
    guiActivationOptions.socketPath =
        QDir{runtime}.filePath(QStringLiteral("javelin-gui.activation.sock"));

    QLockFile guiInstanceLock{QDir{runtime}.filePath(QStringLiteral("javelin.lock"))};
    guiInstanceLock.setStaleLockTime(0);
    const auto requestActivation = [](const javelin::protocol::SocketClientOptions& options,
                                      const javelin::protocol::ActivationRoute& route)
    { return javelin::protocol::SocketActivationClient::request(options, route); };

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
            for (const auto* options : {&activationOptions, &guiActivationOptions})
            {
                const auto activation = requestActivation(*options, requestedActivation);
                if (const auto* activationReply =
                        std::get_if<std::optional<javelin::protocol::BoundaryError>>(&activation);
                    activationReply != nullptr && !activationReply->has_value())
                {
                    return 0;
                }
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

    const auto activation = requestActivation(activationOptions, requestedActivation);
    if (const auto* activationReply =
            std::get_if<std::optional<javelin::protocol::BoundaryError>>(&activation);
        activationReply != nullptr && !activationReply->has_value())
    {
        return 0;
    }

    GuiActivationHandler guiActivationHandler;
    javelin::protocol::SocketActivationEndpoint guiActivationEndpoint{guiActivationHandler,
                                                                      guiActivationOptions};
    if (const auto error = guiActivationEndpoint.listen())
    {
        qCritical().noquote() << QStringLiteral("Could not open GUI activation socket:")
                              << error->detail;
        return 1;
    }

    javelin::app::GuiDaemonSession session{
        {.runtimeDirectory = runtime,
         .socketPath = socketPath,
         .daemonExecutable =
             QDir{QCoreApplication::applicationDirPath()}.filePath(QStringLiteral("javelind")),
         .protocol = {.major = 5, .minor = 10},
         .build = {.application = QStringLiteral("Javelin-Mail"),
                   .revision = QStringLiteral(JAVELIN_APP_VERSION)},
         .startTimeoutMilliseconds = 5000,
         .startDaemonIfMissing = false}};

    QMainWindow recoveryWindow;
    recoveryWindow.setWindowTitle(i18n("Welcome to Javelin Mail"));
    recoveryWindow.setWindowIcon(application.windowIcon());
    auto* recoveryCentral = new QWidget(&recoveryWindow);
    auto* recoveryLayout = new QVBoxLayout(recoveryCentral);
    auto* recoveryStatus = new QLabel(recoveryCentral);
    recoveryStatus->setWordWrap(true);
    auto* enableAndStartDaemon = new QPushButton(i18n("Enable background sync"), recoveryCentral);
    auto* startDaemon = new QPushButton(i18n("Start Javelin now"), recoveryCentral);
    auto* retry = new QPushButton(i18n("Retry daemon connection"), recoveryCentral);
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
    QHash<QString, QPointer<javelin::gui::compose::UndoSendDialog>> undoSendDialogs;
    std::vector<javelin::protocol::OpenMailtoRoute> pendingMailtos;

    const auto showRecovery = [&recoveryWindow, recoveryStatus, enableAndStartDaemon, startDaemon,
                               retry, &session](const QString&, const bool offerDaemonStart)
    {
        recoveryStatus->setText(
            offerDaemonStart
                ? i18n("Javelin’s background sync service isn’t running yet. Start it once, or "
                       "enable it so mail stays up to date whenever you sign in.")
                : i18n("Javelin couldn’t open its background sync service. Please try again."));
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
    std::function<void()> openPendingMailtos;
    std::function<void()> startDaemonForPendingMailto;
    const auto createMainWindow = [&]
    {
        if (mainWindow != nullptr)
            return;
        if (!services)
            services = std::make_unique<javelin::app::GuiServices>(session);

        auto* guiServices = services.get();
        mainWindow = new javelin::gui::shell::MainWindow(
            services->settings(), services->accountCommandPort(), services->accountReader(),
            services->mailboxReader(), services->mailTagReader(), services->contactIdentityLookup(),
            services->identityReader(), services->messageViewReader(), session.databasePath(),
            services->translationService(), services->developerDiagnosticsPort(),
            services->developerMaintenancePort(), services->daemonLogPort(),
            services->mailCommandPort(), services->sieveCommandPort(),
            services->identityCommandPort(), services->accountRefreshPort(),
            services->onboardingPort(), services->messageContentPort(),
            services->messageListSessionFactory(), services->mailEvents(),
            services->messageNavigationPort(), services->undoCommandPort(),
            {.calendar =
                 [guiServices](QStackedWidget& contentStack,
                               std::vector<javelin::gui::shell::TabState>& tabs, QObject* parent)
             {
                 return new javelin::gui::shell::CalendarTabController(
                     guiServices->settings(), guiServices->calendarReader(),
                     guiServices->calendarCommandPort(), contentStack, tabs, parent);
             },
             .contacts =
                 [guiServices](QStackedWidget& contentStack,
                               std::vector<javelin::gui::shell::TabState>& tabs, QObject* parent)
             {
                 return new javelin::gui::shell::ContactsTabController(
                     guiServices->settings(), guiServices->contactReader(),
                     guiServices->accountRefreshPort(), guiServices->contactCommandPort(),
                     contentStack, tabs, parent);
             },
             .compose =
                 [guiServices](QStackedWidget& contentStack,
                               std::vector<javelin::gui::shell::TabState>& tabs, QObject* parent)
             {
                 return new javelin::gui::shell::ComposeTabController(
                     guiServices->settings(), guiServices->composeCommandPort(),
                     guiServices->accountReader(), guiServices->identityReader(),
                     guiServices->contactIdentityLookup(), guiServices->mailEvents(), contentStack,
                     tabs, parent);
             }});
        mainWindow->setAttribute(Qt::WA_DeleteOnClose);

        auto* taskButton = new QToolButton(mainWindow);
        taskButton->setAutoRaise(true);
        taskButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
        taskButton->setToolTip(i18n("Open Task Center"));
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
        if (!session.isReady())
            return;
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
        if (openPendingMailtos)
            openPendingMailtos();
    };

    openPendingMailtos = [&]
    {
        if (!session.isReady() || mainWindow == nullptr)
            return;
        for (const auto& mailto : std::exchange(pendingMailtos, {}))
        {
            ActivationTokenScope tokenScope{mailto.activationToken};
            mainWindow->openMailtoUri(mailto.uri);
        }
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
                else if constexpr (std::is_same_v<Route, javelin::protocol::OpenCalendarEventRoute>)
                {
                    restoreMainWindow(activationRoute.activationToken);
                    const auto navigationDate =
                        QDate::fromString(activationRoute.navigationDate, Qt::ISODate);
                    mainWindow->openCalendarEvent(
                        activationRoute.calendarAccountId, activationRoute.eventId,
                        activationRoute.recurrenceId.value_or(QString{}),
                        navigationDate.isValid() ? navigationDate : QDate::currentDate());
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
                else if constexpr (std::is_same_v<Route,
                                                  javelin::protocol::ShowUndoSendDialogRoute>)
                {
                    restoreMainWindow({});
                    if (mainWindow == nullptr || !services)
                        return;
                    if (const auto existing = undoSendDialogs.value(activationRoute.sendId))
                        existing->close();
                    auto* dialog = new javelin::gui::compose::UndoSendDialog(
                        activationRoute.sendId, activationRoute.title, activationRoute.message,
                        activationRoute.deadlineEpochMilliseconds, services->composeCommandPort(),
                        mainWindow);
                    undoSendDialogs.insert(activationRoute.sendId, dialog);
                    QObject::connect(dialog, &QObject::destroyed, mainWindow,
                                     [&, sendId = activationRoute.sendId, dialog]
                                     {
                                         if (undoSendDialogs.value(sendId) == dialog)
                                             undoSendDialogs.remove(sendId);
                                     });
                    dialog->show();
                    dialog->raise();
                    dialog->activateWindow();
                }
                else if constexpr (std::is_same_v<Route,
                                                  javelin::protocol::CloseUndoSendDialogRoute>)
                {
                    if (const auto dialog = undoSendDialogs.take(activationRoute.sendId))
                        dialog->close();
                }
                else if constexpr (std::is_same_v<Route, javelin::protocol::OpenMailtoRoute>)
                {
                    pendingMailtos.push_back(activationRoute);
                    if (!session.isReady())
                    {
                        if (startDaemonForPendingMailto)
                            startDaemonForPendingMailto();
                        return;
                    }
                    restoreMainWindow(activationRoute.activationToken);
                    openPendingMailtos();
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

    bool guiActivationHandlingEnabled = false;
    const auto enableGuiActivationHandling = [&]
    {
        if (guiActivationHandlingEnabled || !session.isReady())
            return;
        guiActivationHandlingEnabled = true;
        guiActivationHandler.setHandler(handleActivation);
    };

    QObject::connect(&session, &javelin::app::GuiDaemonSession::recoveryStarted, &recoveryWindow,
                     [&](const QString& detail)
                     {
                         guiActivationHandlingEnabled = false;
                         guiActivationHandler.clearHandler();
                         if (mainWindow != nullptr)
                             mainWindow->setEnabled(false);
                         showRecovery(detail, true);
                     });
    QObject::connect(&session, &javelin::app::GuiDaemonSession::recoveryFinished, &recoveryWindow,
                     [&]
                     {
                         enableGuiActivationHandling();
                         restoreMainWindow({});
                     });
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
        recoveryStatus->setText(i18n("Starting Javelin…"));
        recoveryWindow.show();
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        if (const auto error = session.startDaemon(mode))
            showRecovery(error->detail, canOfferDaemonStart(*error));
        else
        {
            enableGuiActivationHandling();
            restoreMainWindow({});
        }
    };

    startDaemonForPendingMailto = [&]
    {
        if (session.isReady())
        {
            restoreMainWindow({});
            return;
        }
        attemptDaemonStart(javelin::app::GuiDaemonStartMode::Once);
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
                         {
                             enableGuiActivationHandling();
                             restoreMainWindow({});
                         }
                     });

    if (const auto error = session.start())
    {
        if (!mailtoUri.isEmpty() && canOfferDaemonStart(*error))
        {
            pendingMailtos.push_back(
                std::get<javelin::protocol::OpenMailtoRoute>(requestedActivation));
            startDaemonForPendingMailto();
        }
        else
        {
            showRecovery(error->detail, canOfferDaemonStart(*error));
        }
    }
    else if (!mailtoUri.isEmpty())
    {
        enableGuiActivationHandling();
        handleActivation(requestedActivation);
    }
    else
    {
        enableGuiActivationHandling();
        restoreMainWindow({});
    }

    return application.exec();
}
