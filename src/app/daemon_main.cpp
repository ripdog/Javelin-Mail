#include "app/DaemonProcess.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>

#include <exception>
#include <utility>

#ifndef JAVELIN_APP_VERSION
#define JAVELIN_APP_VERSION "0.0.0"
#endif

namespace
{
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
    QCoreApplication::setOrganizationName(QStringLiteral("Javelin"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("javelin.app"));
    QCoreApplication::setApplicationName(QStringLiteral("Javelin Mail"));
    QCoreApplication::setApplicationVersion(QStringLiteral(JAVELIN_APP_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Javelin Mail background daemon"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption runtimeOption{QStringLiteral("runtime-directory"),
                                           QStringLiteral("Private runtime directory."),
                                           QStringLiteral("directory")};
    const QCommandLineOption socketOption{
        QStringLiteral("socket"),
        QStringLiteral("Daemon socket path inside the runtime directory."), QStringLiteral("path")};
    parser.addOption(runtimeOption);
    parser.addOption(socketOption);
    parser.process(application);

    const auto runtime =
        parser.isSet(runtimeOption) ? parser.value(runtimeOption) : runtimeDirectory();
    if (runtime.isEmpty())
        return fail(QStringLiteral("no private runtime directory is available"));

    const auto socketPath = parser.isSet(socketOption)
                                ? parser.value(socketOption)
                                : QDir{runtime}.filePath(QStringLiteral("javelind.sock"));
    const javelin::app::DaemonProcessOptions options{
        .socket = {.runtimeDirectory = runtime,
                   .socketPath = socketPath,
                   .limits = {},
                   .protocol = {.major = 1, .minor = 0},
                   .expectedBuild = std::nullopt,
                   .maximumQueuedFrames = 128,
                   .maximumQueuedBytes = 4 * 1024 * 1024,
                   .responseTimeoutMilliseconds = 5000,
                   .enforcePeerCredentials = true},
        .protocol = {.major = 1, .minor = 0},
        .build = {.application = QStringLiteral("Javelin-Mail"),
                  .revision = QStringLiteral(JAVELIN_APP_VERSION)},
        .cacheRootPath = {},
        .settingsPath = {},
    };

    try
    {
        javelin::app::DaemonProcess process{std::move(options)};
        if (const auto error = process.start())
            return fail(error->detail);
        return application.exec();
    }
    catch (const std::exception& exception)
    {
        return fail(QString::fromUtf8(exception.what()));
    }
}
