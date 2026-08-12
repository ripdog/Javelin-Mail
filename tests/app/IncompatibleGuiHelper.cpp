#include "protocol/LocalDaemonClient.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>

#include <variant>

int main(int argc, char* argv[])
{
    QCoreApplication application{argc, argv};
    QCommandLineParser parser;
    const QCommandLineOption runtimeOption{
        QStringLiteral("runtime-directory"), {}, QStringLiteral("directory")};
    const QCommandLineOption socketOption{QStringLiteral("socket"), {}, QStringLiteral("path")};
    parser.addOption(runtimeOption);
    parser.addOption(socketOption);
    parser.process(application);

    const auto runtimeDirectory = parser.value(runtimeOption);
    const auto socketPath = parser.value(socketOption);
    if (runtimeDirectory.isEmpty() || socketPath.isEmpty())
        return 2;

    QFile launches{QDir{runtimeDirectory}.filePath(QStringLiteral("incompatible-gui-launches"))};
    if (!launches.open(QIODevice::WriteOnly | QIODevice::Append))
        return 3;
    launches.write("launch\n");
    launches.close();

    javelin::protocol::SocketDaemonClient client{{
        .runtimeDirectory = runtimeDirectory,
        .socketPath = socketPath,
        .limits = {},
        .protocol = {.major = 5, .minor = 0},
        .expectedBuild = std::nullopt,
        .maximumQueuedFrames = 8,
        .maximumQueuedBytes = 4096,
        .responseTimeoutMilliseconds = 1000,
        .enforcePeerCredentials = false,
    }};
    if (client.connectToDaemon().has_value())
        return 4;

    const auto hello = client.hello({
        .protocol = {.major = 5, .minor = 0},
        .build = {.application = QStringLiteral("Javelin-Mail"),
                  .revision = QStringLiteral("incompatible-gui-test")},
    });
    return std::holds_alternative<javelin::protocol::HandshakeRejected>(hello) ? 0 : 5;
}
