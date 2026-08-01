#include "app/GuiDaemonSession.h"

#include <QApplication>
#include <QDir>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

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

    const auto runtime = runtimeDirectory();
    if (runtime.isEmpty())
        return 1;

    const auto socketPath = QDir{runtime}.filePath(QStringLiteral("javelind.sock"));
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

    QMainWindow window;
    window.setWindowTitle(QStringLiteral("Javelin Mail"));
    auto* central = new QWidget(&window);
    auto* layout = new QVBoxLayout(central);
    auto* status = new QLabel(central);
    auto* retry = new QPushButton(QStringLiteral("Retry daemon connection"), central);
    retry->setEnabled(false);
    layout->addWidget(status);
    layout->addWidget(retry);
    window.setCentralWidget(central);

    const auto showRecovery = [&status, retry](const QString& detail)
    {
        status->setText(QStringLiteral("Daemon recovery required: %1").arg(detail));
        retry->setEnabled(true);
    };
    QObject::connect(&session, &javelin::app::GuiDaemonSession::recoveryStarted, &window,
                     showRecovery);
    QObject::connect(&session, &javelin::app::GuiDaemonSession::recoveryFinished, &window,
                     [&status, retry]
                     {
                         status->setText(QStringLiteral("Javelin Mail daemon reconnected"));
                         retry->setEnabled(false);
                     });
    QObject::connect(&session, &javelin::app::GuiDaemonSession::activationRequested, &window,
                     [&window, &status](const javelin::protocol::ActivationRoute&)
                     {
                         status->setText(QStringLiteral("Activation received"));
                         window.show();
                         window.raise();
                         window.activateWindow();
                     });
    QObject::connect(retry, &QPushButton::clicked, &window,
                     [&session, showRecovery]
                     {
                         if (const auto error = session.reconnect())
                             showRecovery(error->detail);
                     });

    if (const auto error = session.start())
    {
        showRecovery(error->detail);
        retry->setEnabled(true);
    }
    else
    {
        status->setText(QStringLiteral("Javelin Mail daemon connected; cache is read-only"));
    }

    window.resize(480, 140);
    window.show();
    return application.exec();
}
