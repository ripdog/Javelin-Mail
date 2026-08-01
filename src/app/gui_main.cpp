#include "app/GuiDaemonSession.h"

#include "gui/shell/GuiWorkspaceWindow.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <memory>

#ifndef JAVELIN_APP_VERSION
#define JAVELIN_APP_VERSION "0.0.0"
#endif

namespace
{
    [[nodiscard]] QString runtimeDirectory()
    {
        return QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    }
} // namespace

int main(int argc, char* argv[])
{
    QApplication application{argc, argv};
    QCoreApplication::setOrganizationName(QStringLiteral("Javelin"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("javelin.app"));
    QCoreApplication::setApplicationName(QStringLiteral("Javelin Mail"));
    QCoreApplication::setApplicationVersion(QStringLiteral(JAVELIN_APP_VERSION));

    QCommandLineParser parser;
    parser.addHelpOption();
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
        return 1;

    const auto socketPath = parser.isSet(socketOption)
                                ? parser.value(socketOption)
                                : QDir{runtime}.filePath(QStringLiteral("javelind.sock"));
    auto activationOptions = javelin::protocol::SocketClientOptions{
        .runtimeDirectory = runtime,
        .socketPath = socketPath + QStringLiteral(".activation"),
        .limits = {},
        .protocol = {.major = 1, .minor = 0},
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
        return 0;

    javelin::app::GuiDaemonSession session{
        {.runtimeDirectory = runtime,
         .socketPath = socketPath,
         .daemonExecutable =
             QDir{QCoreApplication::applicationDirPath()}.filePath(QStringLiteral("javelind")),
         .protocol = {.major = 1, .minor = 0},
         .build = {.application = QStringLiteral("Javelin-Mail"),
                   .revision = QStringLiteral(JAVELIN_APP_VERSION)},
         .startTimeoutMilliseconds = 5000,
         .startDaemonIfMissing = true}};

    QMainWindow recoveryWindow;
    recoveryWindow.setWindowTitle(QStringLiteral("Javelin Mail — reconnecting"));
    auto* recoveryCentral = new QWidget(&recoveryWindow);
    auto* recoveryLayout = new QVBoxLayout(recoveryCentral);
    auto* recoveryStatus = new QLabel(recoveryCentral);
    auto* retry = new QPushButton(QStringLiteral("Retry daemon connection"), recoveryCentral);
    recoveryLayout->addWidget(recoveryStatus);
    recoveryLayout->addWidget(retry);
    recoveryWindow.setCentralWidget(recoveryCentral);
    recoveryWindow.resize(480, 140);

    std::unique_ptr<javelin::gui::shell::GuiWorkspaceWindow> workspace;
    const auto showRecovery = [&recoveryWindow, &recoveryStatus, retry](const QString& detail)
    {
        recoveryStatus->setText(QStringLiteral("Daemon recovery required: %1").arg(detail));
        retry->setEnabled(true);
        recoveryWindow.show();
        recoveryWindow.raise();
        recoveryWindow.activateWindow();
    };
    const auto showWorkspace = [&]
    {
        if (!workspace)
            workspace = std::make_unique<javelin::gui::shell::GuiWorkspaceWindow>(session);
        recoveryWindow.hide();
        workspace->setEnabled(true);
        workspace->show();
        workspace->raise();
        workspace->activateWindow();
    };

    QObject::connect(&session, &javelin::app::GuiDaemonSession::recoveryStarted, &recoveryWindow,
                     [&workspace, &showRecovery](const QString& detail)
                     {
                         if (workspace)
                             workspace->setEnabled(false);
                         showRecovery(detail);
                     });
    QObject::connect(&session, &javelin::app::GuiDaemonSession::recoveryFinished, &recoveryWindow,
                     showWorkspace);
    QObject::connect(&session, &javelin::app::GuiDaemonSession::daemonShutdownRequested,
                     &application, &QCoreApplication::quit);
    QObject::connect(&session, &javelin::app::GuiDaemonSession::activationRequested,
                     &recoveryWindow,
                     [&workspace, &showWorkspace](const javelin::protocol::ActivationRoute&)
                     {
                         if (workspace)
                             workspace->refresh();
                         showWorkspace();
                     });
    QObject::connect(retry, &QPushButton::clicked, &recoveryWindow,
                     [&session, &showRecovery, &showWorkspace]
                     {
                         if (const auto error = session.reconnect())
                             showRecovery(error->detail);
                         else
                             showWorkspace();
                     });

    if (const auto error = session.start())
        showRecovery(error->detail);
    else
        showWorkspace();

    return application.exec();
}
