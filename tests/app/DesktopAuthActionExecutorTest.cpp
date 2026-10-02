#include "desktop/notifications/DesktopAuthActionExecutor.h"

#include <QCoreApplication>
#include <QDBusObjectPath>
#include <QEventLoop>
#include <QProcess>
#include <QTimer>
#include <QVariantMap>

#include <catch2/catch_test_macros.hpp>

#include <memory>

namespace
{
    class ApplicationGuard
    {
      public:
        ApplicationGuard()
        {
            if (QCoreApplication::instance())
                return;
            static int argc = 1;
            static char name[] = "desktop-auth-action-test";
            static char* argv[] = {name, nullptr};
            m_application = std::make_unique<QCoreApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };

    class PrivateBus
    {
      public:
        PrivateBus()
        {
            process.start(QStringLiteral("dbus-daemon"),
                          {QStringLiteral("--session"), QStringLiteral("--nofork"),
                           QStringLiteral("--print-address=1")});
            REQUIRE(process.waitForStarted());
            REQUIRE(process.waitForReadyRead());
            address = QString::fromUtf8(process.readLine()).trimmed();
            REQUIRE_FALSE(address.isEmpty());
        }
        ~PrivateBus()
        {
            QDBusConnection::disconnectFromBus(QStringLiteral("auth-test-client"));
            QDBusConnection::disconnectFromBus(QStringLiteral("auth-test-server"));
            process.terminate();
            if (!process.waitForFinished(1000))
            {
                process.kill();
                process.waitForFinished();
            }
        }
        QProcess process;
        QString address;
    };

    class ClipboardService final : public QObject
    {
        Q_OBJECT
        Q_CLASSINFO("D-Bus Interface", "org.kde.klipper.klipper")
      public:
        QString contents;
      public Q_SLOTS:
        void setClipboardContents(const QString& value)
        {
            contents = value;
            Q_EMIT called();
        }
      Q_SIGNALS:
        void called();
    };

    class LinkService final : public QObject
    {
        Q_OBJECT
        Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.OpenURI")
      public:
        QString parentWindow;
        QString uri;
        QVariantMap options;
      public Q_SLOTS:
        QDBusObjectPath OpenURI(const QString& parent, const QString& value,
                                const QVariantMap& requestOptions)
        {
            parentWindow = parent;
            uri = value;
            options = requestOptions;
            Q_EMIT called();
            return QDBusObjectPath{
                QStringLiteral("/org/freedesktop/portal/desktop/request/test/1")};
        }
      Q_SIGNALS:
        void called();
    };
} // namespace

TEST_CASE("desktop authentication actions use clipboard and URI services without a GUI",
          "[app][daemon][notification][authentication][dbus]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    PrivateBus bus;
    auto server = QDBusConnection::connectToBus(bus.address, QStringLiteral("auth-test-server"));
    auto client = QDBusConnection::connectToBus(bus.address, QStringLiteral("auth-test-client"));
    REQUIRE(server.isConnected());
    REQUIRE(client.isConnected());
    ClipboardService clipboard;
    LinkService links;
    REQUIRE(server.registerService(QStringLiteral("org.kde.klipper")));
    REQUIRE(server.registerObject(QStringLiteral("/klipper"), &clipboard,
                                  QDBusConnection::ExportAllSlots));
    REQUIRE(server.registerService(QStringLiteral("org.freedesktop.portal.Desktop")));
    REQUIRE(server.registerObject(QStringLiteral("/org/freedesktop/portal/desktop"), &links,
                                  QDBusConnection::ExportAllSlots));
    javelin::app::DesktopAuthActionExecutor executor{client};
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    bool called = false;
    QObject::connect(&clipboard, &ClipboardService::called, &loop,
                     [&]
                     {
                         called = true;
                         loop.quit();
                     });
    QObject::connect(&links, &LinkService::called, &loop,
                     [&]
                     {
                         called = true;
                         loop.quit();
                     });
    QString failure;
    QObject::connect(&executor, &javelin::app::DesktopAuthActionExecutor::actionFailed, &loop,
                     [&](const QString&, const QString& detail)
                     {
                         failure = detail;
                         loop.quit();
                     });
    timeout.start(5000);
    SECTION("copy code")
    {
        executor.copyCode(QStringLiteral("123456"));
        loop.exec();
        REQUIRE(failure.isEmpty());
        REQUIRE(called);
        CHECK(clipboard.contents == QStringLiteral("123456"));
        CHECK(links.uri.isEmpty());
    }
    SECTION("open link preserves the desktop activation token")
    {
        executor.openLink(QStringLiteral("https://accounts.example.test/login?token=secret"),
                          QStringLiteral("activation-token"));
        loop.exec();
        REQUIRE(failure.isEmpty());
        REQUIRE(called);
        CHECK(clipboard.contents.isEmpty());
        CHECK(links.parentWindow.isEmpty());
        CHECK(links.uri == QStringLiteral("https://accounts.example.test/login?token=secret"));
        CHECK(links.options.value(QStringLiteral("activation_token")).toString() ==
              QStringLiteral("activation-token"));
    }
}

TEST_CASE("desktop authentication actions report unavailable clipboard service and invalid links",
          "[app][daemon][notification][authentication][dbus][failure]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    PrivateBus bus;
    auto client = QDBusConnection::connectToBus(bus.address, QStringLiteral("auth-test-client"));
    REQUIRE(client.isConnected());
    javelin::app::DesktopAuthActionExecutor executor{client};
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    bool failed = false;
    QString failure;
    QObject::connect(&executor, &javelin::app::DesktopAuthActionExecutor::actionFailed, &loop,
                     [&](const QString&, const QString& detail)
                     {
                         failed = true;
                         failure = detail;
                         loop.quit();
                     });
    SECTION("unavailable clipboard")
    {
        timeout.start(5000);
        executor.copyCode(QStringLiteral("123456"));
        loop.exec();
        REQUIRE(failed);
        CHECK_FALSE(failure.contains(QStringLiteral("123456")));
    }
    SECTION("unsupported link scheme")
    {
        executor.openLink(QStringLiteral("file:///tmp/auth"), {});
        REQUIRE(failed);
    }
}

#include "DesktopAuthActionExecutorTest.moc"
