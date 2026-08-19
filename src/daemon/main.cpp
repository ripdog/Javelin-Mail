#include "app/LogStore.h"
#include "app/PerformanceMetrics.h"
#include "daemon/DaemonProcess.h"

#include <KLocalizedString>

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include <csignal>
#include <exception>
#include <utility>

#ifndef JAVELIN_APP_VERSION
#define JAVELIN_APP_VERSION "0.0.0"
#endif

namespace
{
    volatile std::sig_atomic_t shutdownRequested = 0;

    void handleShutdownSignal(int)
    {
        shutdownRequested = 1;
    }

    [[nodiscard]] QString runtimeDirectory()
    {
        return QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    }

    [[nodiscard]] int fail(const QString& detail)
    {
        qCritical().noquote() << QStringLiteral("javelind:") << detail;
        return 1;
    }
} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application{argc, argv};
    KLocalizedString::setApplicationDomain(QByteArrayLiteral("javelinmail"));
    javelin::app::LogStore::install(1000);
    QCoreApplication::setOrganizationName(QStringLiteral("Javelin Mail"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("javelin.app"));
    QCoreApplication::setApplicationName(QStringLiteral("Javelin Mail"));
    QCoreApplication::setApplicationVersion(QStringLiteral(JAVELIN_APP_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(i18n("Javelin Mail background daemon"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption runtimeOption{QStringLiteral("runtime-directory"),
                                           i18n("Private runtime directory."),
                                           i18nc("@info:shell command-line value", "directory")};
    const QCommandLineOption socketOption{QStringLiteral("socket"),
                                          i18n("Daemon socket path inside the runtime directory."),
                                          i18nc("@info:shell command-line value", "path")};
    parser.addOption(runtimeOption);
    parser.addOption(socketOption);
    parser.process(application);

    const auto runtime =
        parser.isSet(runtimeOption) ? parser.value(runtimeOption) : runtimeDirectory();
    if (runtime.isEmpty())
        return fail(i18n("No private runtime directory is available."));

    const auto socketPath = parser.isSet(socketOption)
                                ? parser.value(socketOption)
                                : QDir{runtime}.filePath(QStringLiteral("javelind.sock"));
    const QSettings legacySettings{QSettings::NativeFormat, QSettings::UserScope,
                                   QStringLiteral("Javelin Mail"), QStringLiteral("javelinmail")};
    const javelin::app::DaemonProcessOptions options{
        .socket = {.runtimeDirectory = runtime,
                   .socketPath = socketPath,
                   .limits = {.maximumStringBytes = 4096,
                              .maximumCollectionItems = 256,
                              .maximumAffectedKeys = 64,
                              .maximumMaterializationItems = 500,
                              .maximumFrameBytes = 64 * 1024 * 1024},
                   .protocol = {.major = 5, .minor = 11},
                   .expectedBuild = std::nullopt,
                   .maximumQueuedFrames = 128,
                   .maximumQueuedBytes = 128 * 1024 * 1024,
                   .responseTimeoutMilliseconds = 5000,
                   .enforcePeerCredentials = true},
        .protocol = {.major = 5, .minor = 11},
        .build = {.application = QStringLiteral("Javelin-Mail"),
                  .revision = QStringLiteral(JAVELIN_APP_VERSION)},
        .guiExecutable =
            QDir{QCoreApplication::applicationDirPath()}.filePath(QStringLiteral("javelin")),
        .cacheRootPath = {},
        .settingsPath = legacySettings.fileName(),
        .credentialStore = {},
    };

    try
    {
        std::signal(SIGINT, handleShutdownSignal);
        std::signal(SIGTERM, handleShutdownSignal);
        javelin::app::DaemonProcess process{std::move(options)};
        javelin::app::PerformanceSpan startupMetrics{QStringLiteral("daemon"),
                                                     QStringLiteral("process_startup")};
        QObject::connect(&process, &javelin::app::DaemonProcess::shutdownRequested, &application,
                         &QCoreApplication::quit);
        if (const auto error = process.start())
        {
            startupMetrics.finish(QStringLiteral("failed"));
            return fail(error->detail);
        }
        startupMetrics.finish(QStringLiteral("ready"), QStringLiteral("socket_ready=true"));

        QTimer shutdownTimer;
        shutdownTimer.setInterval(100);
        QObject::connect(&shutdownTimer, &QTimer::timeout, &application,
                         [&process, &shutdownTimer]
                         {
                             if (shutdownRequested == 0)
                                 return;
                             shutdownTimer.stop();
                             process.requestShutdown();
                         });
        shutdownTimer.start();
        return application.exec();
    }
    catch (const std::exception& exception)
    {
        return fail(QString::fromUtf8(exception.what()));
    }
}
