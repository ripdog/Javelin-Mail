#include "app/DaemonTrayController.h"

#include "app/WorkScheduler.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QDBusVirtualObject>
#include <QDebug>
#include <QMetaType>
#include <QVariantList>
#include <QVariantMap>

#include <tuple>

namespace javelin::app
{
    namespace
    {
        constexpr auto trayService = "org.javelin.JavelinMail";
        constexpr auto trayPath = "/org/javelin/JavelinMail/StatusNotifierItem";
        constexpr auto menuPath = "/org/javelin/JavelinMail/Menu";
        constexpr auto watcherService = "org.kde.StatusNotifierWatcher";
        constexpr auto watcherPath = "/StatusNotifierWatcher";
        constexpr auto watcherInterface = "org.kde.StatusNotifierWatcher";
        constexpr auto menuInterface = "com.canonical.dbusmenu";

        using MenuProperties = std::tuple<qint32, QVariantMap>;
        using MenuLayout = std::tuple<qint32, QVariantMap, QVariantList>;

        [[nodiscard]] QVariantMap actionProperties(const QString& label, const bool enabled,
                                                   const bool separator = false)
        {
            QVariantMap properties;
            properties.insert(QStringLiteral("label"), label);
            properties.insert(QStringLiteral("enabled"), enabled);
            properties.insert(QStringLiteral("visible"), true);
            if (separator)
                properties.insert(QStringLiteral("type"), QStringLiteral("separator"));
            return properties;
        }

        [[nodiscard]] QDBusArgument menuLayoutItem(const qint32 id, const QVariantMap& properties,
                                                   const QVariantList& children = {})
        {
            QDBusArgument item;
            item << MenuLayout{id, properties, children};
            return item;
        }

        [[nodiscard]] QDBusVariant variant(const QVariant& value)
        {
            return QDBusVariant{value};
        }

        [[nodiscard]] QVariant propertyValue(const QString& name, const QString& summary)
        {
            if (name == QStringLiteral("label"))
                return summary;
            if (name == QStringLiteral("enabled"))
                return true;
            if (name == QStringLiteral("visible"))
                return true;
            return {};
        }
    } // namespace

    class DaemonTrayController::Menu final : public QDBusVirtualObject
    {
      public:
        explicit Menu(DaemonTrayController& controller)
            : QDBusVirtualObject(&controller), m_controller(controller)
        {
        }

        [[nodiscard]] QString introspect(const QString&) const override
        {
            return QStringLiteral(
                "<!DOCTYPE node PUBLIC '-//freedesktop//DTD D-BUS Object Introspection 1.0//EN' "
                "'http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd'>"
                "<node>"
                "<interface name='com.canonical.dbusmenu'>"
                "<property name='Version' type='u' access='read'/>"
                "<property name='TextDirection' type='s' access='read'/>"
                "<property name='Status' type='s' access='read'/>"
                "<method name='GetLayout'><arg type='i' direction='in'/><arg type='i' "
                "direction='in'/>"
                "<arg type='as' direction='in'/><arg type='u' direction='out'/>"
                "<arg type='(ia{sv}av)' direction='out'/></method>"
                "<method name='GetGroupProperties'><arg type='ai' direction='in'/><arg type='as' "
                "direction='in'/><arg type='a(ia{sv})' direction='out'/></method>"
                "<method name='GetProperty'><arg type='i' direction='in'/><arg type='s' "
                "direction='in'/>"
                "<arg type='v' direction='out'/></method>"
                "<method name='Event'><arg type='i' direction='in'/><arg type='s' direction='in'/>"
                "<arg type='v' direction='in'/><arg type='u' direction='in'/></method>"
                "<method name='AboutToShow'><arg type='i' direction='in'/><arg type='b' "
                "direction='out'/>"
                "</method>"
                "<signal name='LayoutUpdated'><arg type='u'/><arg type='i'/></signal>"
                "</interface>"
                "<interface name='org.freedesktop.DBus.Properties'>"
                "<method name='Get'><arg type='s' direction='in'/><arg type='s' direction='in'/>"
                "<arg type='v' direction='out'/></method>"
                "<method name='GetAll'><arg type='s' direction='in'/><arg type='a{sv}' "
                "direction='out'/>"
                "</method></interface></node>");
        }

        bool handleMessage(const QDBusMessage& message, const QDBusConnection& connection) override
        {
            if (message.interface() == QString::fromLatin1(menuInterface))
            {
                if (message.member() == QStringLiteral("GetLayout"))
                {
                    const auto arguments = message.arguments();
                    const int recursionDepth = arguments.value(1).toInt();
                    QVariantList children;
                    if (recursionDepth != 0)
                    {
                        children.push_back(QVariant::fromValue(menuLayoutItem(
                            1, actionProperties(QStringLiteral("Open Javelin"), true))));
                        children.push_back(QVariant::fromValue(menuLayoutItem(
                            2, actionProperties(QStringLiteral("Task Center…"), true))));
                        children.push_back(QVariant::fromValue(menuLayoutItem(
                            3, actionProperties(QStringLiteral("Refresh accounts"), true))));
                        children.push_back(QVariant::fromValue(
                            menuLayoutItem(4, actionProperties(QStringLiteral("Quit"), true))));
                    }
                    const auto layout = menuLayoutItem(0, {}, children);
                    QList<QVariant> replyArguments;
                    replyArguments.push_back(QVariant::fromValue(quint32(1)));
                    replyArguments.push_back(QVariant::fromValue(layout));
                    return connection.send(message.createReply(replyArguments));
                }

                if (message.member() == QStringLiteral("GetGroupProperties"))
                {
                    const auto arguments = message.arguments();
                    const auto ids = arguments.value(0).value<QList<int>>();
                    QDBusArgument properties;
                    properties.beginArray(QMetaType::fromType<MenuProperties>());
                    for (const int id : ids)
                    {
                        const auto item = itemProperties(id);
                        if (!item.isEmpty())
                            properties << MenuProperties{id, item};
                    }
                    properties.endArray();
                    return connection.send(message.createReply(QVariant::fromValue(properties)));
                }

                if (message.member() == QStringLiteral("GetProperty"))
                {
                    const auto arguments = message.arguments();
                    const int id = arguments.value(0).toInt();
                    const auto name = arguments.value(1).toString();
                    const auto value = propertyValue(name, labelFor(id));
                    return connection.send(
                        message.createReply(QVariant::fromValue(variant(value))));
                }

                if (message.member() == QStringLiteral("Event"))
                {
                    const auto arguments = message.arguments();
                    if (arguments.value(1).toString() == QStringLiteral("clicked"))
                        activate(arguments.value(0).toInt());
                    return connection.send(message.createReply());
                }

                if (message.member() == QStringLiteral("AboutToShow"))
                    return connection.send(message.createReply(false));
            }

            if (message.interface() == QStringLiteral("org.freedesktop.DBus.Properties"))
            {
                if (message.member() == QStringLiteral("Get"))
                {
                    const auto arguments = message.arguments();
                    const auto property = arguments.value(1).toString();
                    if (property == QStringLiteral("Version"))
                        return connection.send(message.createReply(
                            QVariant::fromValue(variant(QVariant::fromValue(quint32(1))))));
                    if (property == QStringLiteral("TextDirection"))
                        return connection.send(message.createReply(
                            QVariant::fromValue(variant(QStringLiteral("ltr")))));
                    if (property == QStringLiteral("Status"))
                        return connection.send(message.createReply(
                            QVariant::fromValue(variant(QStringLiteral("normal")))));
                }
                if (message.member() == QStringLiteral("GetAll"))
                {
                    QVariantMap values;
                    values.insert(QStringLiteral("Version"),
                                  QVariant::fromValue(variant(QVariant::fromValue(quint32(1)))));
                    values.insert(QStringLiteral("TextDirection"),
                                  QVariant::fromValue(variant(QStringLiteral("ltr"))));
                    values.insert(QStringLiteral("Status"),
                                  QVariant::fromValue(variant(QStringLiteral("normal"))));
                    return connection.send(message.createReply(QVariant::fromValue(values)));
                }
            }

            return false;
        }

      private:
        [[nodiscard]] QVariantMap itemProperties(const int id) const
        {
            switch (id)
            {
            case 1:
                return actionProperties(QStringLiteral("Open Javelin"), true);
            case 2:
                return actionProperties(QStringLiteral("Task Center…"), true);
            case 3:
                return actionProperties(QStringLiteral("Refresh accounts"), true);
            case 4:
                return actionProperties(QStringLiteral("Quit"), true);
            default:
                return {};
            }
        }

        [[nodiscard]] QString labelFor(const int id) const
        {
            const auto properties = itemProperties(id);
            return properties.value(QStringLiteral("label")).toString();
        }

        void activate(const int id)
        {
            switch (id)
            {
            case 1:
                Q_EMIT m_controller.raiseGuiRequested({});
                break;
            case 3:
                Q_EMIT m_controller.refreshRequested();
                break;
            case 2:
                Q_EMIT m_controller.taskCenterRequested({});
                break;
            case 4:
                Q_EMIT m_controller.quitRequested();
                break;
            default:
                break;
            }
        }

        DaemonTrayController& m_controller;
    };

    DaemonTrayController::DaemonTrayController(WorkScheduler& workScheduler, QObject* parent)
        : QObject(parent), m_workScheduler(workScheduler)
    {
        connect(&m_workScheduler, &WorkScheduler::jobsChanged, this,
                &DaemonTrayController::updateSummary);
    }

    DaemonTrayController::~DaemonTrayController()
    {
        stop();
    }

    bool DaemonTrayController::start()
    {
        if (m_available)
            return true;

        auto bus = QDBusConnection::sessionBus();
        if (!bus.isConnected())
        {
            qWarning() << QStringLiteral(
                "StatusNotifier tray unavailable: session bus is not connected");
            return false;
        }
        if (!bus.registerService(QString::fromLatin1(trayService)))
        {
            qWarning().noquote() << QStringLiteral("StatusNotifier tray service unavailable:")
                                 << bus.lastError().message();
            return false;
        }

        m_menu = std::make_unique<Menu>(*this);
        if (!bus.registerObject(QString::fromLatin1(trayPath), this,
                                QDBusConnection::ExportAllSlots |
                                    QDBusConnection::ExportAllSignals |
                                    QDBusConnection::ExportAllProperties) ||
            !bus.registerVirtualObject(QString::fromLatin1(menuPath), m_menu.get()))
        {
            qWarning() << QStringLiteral("StatusNotifier tray objects could not be registered");
            bus.unregisterObject(QString::fromLatin1(trayPath));
            bus.unregisterObject(QString::fromLatin1(menuPath));
            bus.unregisterService(QString::fromLatin1(trayService));
            m_menu.reset();
            return false;
        }

        m_available = true;
        updateSummary();

        QDBusInterface watcher{QString::fromLatin1(watcherService),
                               QString::fromLatin1(watcherPath),
                               QString::fromLatin1(watcherInterface), bus};
        if (!watcher.isValid())
        {
            qWarning() << QStringLiteral(
                "StatusNotifier watcher unavailable; daemon tray controls remain on D-Bus");
        }
        else
        {
            const auto reply = watcher.call(QStringLiteral("RegisterStatusNotifierItem"),
                                            QString::fromLatin1(trayService));
            if (reply.type() == QDBusMessage::ErrorMessage)
                qWarning().noquote() << QStringLiteral("StatusNotifier item registration failed:")
                                     << reply.errorMessage();
        }
        return true;
    }

    void DaemonTrayController::stop()
    {
        if (!m_available && m_menu == nullptr)
            return;
        auto bus = QDBusConnection::sessionBus();
        if (bus.isConnected())
        {
            bus.unregisterObject(QString::fromLatin1(trayPath));
            bus.unregisterObject(QString::fromLatin1(menuPath));
            bus.unregisterService(QString::fromLatin1(trayService));
        }
        m_menu.reset();
        m_available = false;
    }

    bool DaemonTrayController::isAvailable() const
    {
        return m_available;
    }

    QString DaemonTrayController::category() const
    {
        return QStringLiteral("ApplicationStatus");
    }

    QString DaemonTrayController::id() const
    {
        return QStringLiteral("javelinmail");
    }

    QString DaemonTrayController::title() const
    {
        return m_title;
    }

    QString DaemonTrayController::status() const
    {
        return m_status;
    }

    int DaemonTrayController::windowId() const
    {
        return 0;
    }

    QString DaemonTrayController::iconThemePath() const
    {
        return {};
    }

    QDBusObjectPath DaemonTrayController::menu() const
    {
        return QDBusObjectPath{QString::fromLatin1(menuPath)};
    }

    bool DaemonTrayController::itemIsMenu() const
    {
        return false;
    }

    QString DaemonTrayController::iconName() const
    {
        return QStringLiteral("mail-unread");
    }

    void DaemonTrayController::Activate(const int, const int)
    {
        const auto token = std::exchange(m_activationToken, {});
        Q_EMIT raiseGuiRequested(token);
    }

    void DaemonTrayController::ContextMenu(const int, const int)
    {
    }

    void DaemonTrayController::ProvideXdgActivationToken(const QString& token)
    {
        m_activationToken = token;
    }

    void DaemonTrayController::Scroll(const int, const QString&)
    {
    }

    void DaemonTrayController::SecondaryActivate(const int, const int)
    {
        Q_EMIT refreshRequested();
    }

    void DaemonTrayController::updateSummary()
    {
        const auto summary = m_workScheduler.summary();
        const auto title = summary.isEmpty() ? QStringLiteral("Javelin Mail")
                                             : QStringLiteral("Javelin Mail — %1").arg(summary);
        const auto status =
            summary.isEmpty() ? QStringLiteral("Passive") : QStringLiteral("Active");
        if (m_title != title)
        {
            m_title = title;
            if (m_available)
                Q_EMIT NewTitle();
        }
        if (m_status != status)
        {
            m_status = status;
            if (m_available)
                Q_EMIT NewStatus(QStringLiteral("%1").arg(m_status));
        }
    }
} // namespace javelin::app
