#include "desktop/tray/DaemonTrayController.h"

#include "app/WorkScheduler.h"

#include <KLocalizedString>

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusVariant>
#include <QDBusVirtualObject>
#include <QDebug>
#include <QMetaType>
#include <QVariantList>
#include <QVariantMap>

#include <tuple>
#include <vector>

namespace javelin::app
{
    QDBusArgument& operator<<(QDBusArgument& argument, const TrayIconPixmap& pixmap)
    {
        argument.beginStructure();
        argument << pixmap.width << pixmap.height << pixmap.data;
        argument.endStructure();
        return argument;
    }

    const QDBusArgument& operator>>(const QDBusArgument& argument, TrayIconPixmap& pixmap)
    {
        argument.beginStructure();
        argument >> pixmap.width >> pixmap.height >> pixmap.data;
        argument.endStructure();
        return argument;
    }

    QDBusArgument& operator<<(QDBusArgument& argument, const TrayToolTip& tooltip)
    {
        argument.beginStructure();
        argument << tooltip.iconName << tooltip.iconPixmaps << tooltip.title << tooltip.description;
        argument.endStructure();
        return argument;
    }

    const QDBusArgument& operator>>(const QDBusArgument& argument, TrayToolTip& tooltip)
    {
        argument.beginStructure();
        argument >> tooltip.iconName >> tooltip.iconPixmaps >> tooltip.title >> tooltip.description;
        argument.endStructure();
        return argument;
    }

    namespace
    {
        constexpr auto trayPath = "/StatusNotifierItem";
        constexpr auto menuPath = "/org/javelin/JavelinMail/Menu";
        constexpr auto watcherService = "org.kde.StatusNotifierWatcher";
        constexpr auto watcherPath = "/StatusNotifierWatcher";
        constexpr auto watcherInterface = "org.kde.StatusNotifierWatcher";
        constexpr auto menuInterface = "com.canonical.dbusmenu";

        using MenuProperties = std::tuple<qint32, QVariantMap>;
        using MenuLayout = std::tuple<qint32, QVariantMap, QVariantList>;

        [[nodiscard]] QVariantMap actionProperties(const QString& label, const bool enabled,
                                                   const QString& iconName = {},
                                                   const bool separator = false)
        {
            QVariantMap properties;
            properties.insert(QStringLiteral("label"), label);
            properties.insert(QStringLiteral("enabled"), enabled);
            properties.insert(QStringLiteral("visible"), true);
            if (!iconName.isEmpty())
                properties.insert(QStringLiteral("icon-name"), iconName);
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

        [[nodiscard]] QString runningWorkSummary(const WorkScheduler& workScheduler)
        {
            const auto listed = workScheduler.list();
            const auto* jobs = std::get_if<std::vector<WorkRecord>>(&listed);
            if (jobs == nullptr)
                return {};

            std::vector<const WorkRecord*> running;
            for (const auto& job : *jobs)
            {
                if (job.status == WorkStatus::Running)
                    running.push_back(&job);
            }

            if (running.empty())
                return {};
            if (running.size() > 1)
                return i18np("%1 background task running", "%1 background tasks running",
                             running.size());

            const auto& current = *running.front();
            QString detail = current.progress.detail;
            if (current.progress.totalUnits.has_value() && *current.progress.totalUnits > 0)
            {
                const auto progress = QStringLiteral("%1 / %2")
                                          .arg(current.progress.completedUnits)
                                          .arg(*current.progress.totalUnits);
                detail =
                    detail.isEmpty() ? progress : QStringLiteral("%1 — %2").arg(detail, progress);
            }
            return detail.isEmpty() ? current.title
                                    : QStringLiteral("%1 — %2").arg(current.title, detail);
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
                        for (int id = 1; id <= 9; ++id)
                            children.push_back(
                                QVariant::fromValue(menuLayoutItem(id, itemProperties(id))));
                    }
                    const auto layout = menuLayoutItem(0, {}, children);
                    QList<QVariant> replyArguments;
                    replyArguments.push_back(QVariant::fromValue(m_revision));
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
                    const auto value = itemProperties(id).value(name);
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

      public:
        void notifyLayoutChanged()
        {
            ++m_revision;
            if (m_revision == 0)
                m_revision = 1;
            auto signal = QDBusMessage::createSignal(QString::fromLatin1(menuPath),
                                                     QString::fromLatin1(menuInterface),
                                                     QStringLiteral("LayoutUpdated"));
            signal << m_revision << qint32{0};
            QDBusConnection::sessionBus().send(signal);
        }

      private:
        [[nodiscard]] QVariantMap itemProperties(const int id) const
        {
            switch (id)
            {
            case 1:
                return actionProperties(i18n("Open Javelin Mail"), true,
                                        QStringLiteral("javelinmail"));
            case 2:
                return actionProperties(i18n("New Message"), true,
                                        QStringLiteral("mail-message-new"));
            case 3:
                return actionProperties({}, false, {}, true);
            case 4:
                return actionProperties(i18np("Inbox — %1 unread message",
                                              "Inbox — %1 unread messages",
                                              m_controller.m_inboxUnreadCount),
                                        true, QStringLiteral("mail-folder-inbox"));
            case 5:
                return actionProperties(i18n("Contacts"), true,
                                        QStringLiteral("view-pim-contacts"));
            case 6:
                return actionProperties(i18n("Calendar"), true, QStringLiteral("view-calendar"));
            case 7:
                return actionProperties(i18n("Task Center"), true, QStringLiteral("view-task"));
            case 8:
                return actionProperties({}, false, {}, true);
            case 9:
                return actionProperties(i18n("Stop Background Service"), true,
                                        QStringLiteral("process-stop"));
            default:
                return {};
            }
        }

        void activate(const int id)
        {
            switch (id)
            {
            case 1:
                Q_EMIT m_controller.raiseGuiRequested({});
                break;
            case 2:
                Q_EMIT m_controller.newMessageRequested();
                break;
            case 4:
                Q_EMIT m_controller.inboxRequested();
                break;
            case 5:
                Q_EMIT m_controller.contactsRequested();
                break;
            case 6:
                Q_EMIT m_controller.calendarRequested();
                break;
            case 7:
                Q_EMIT m_controller.taskCenterRequested();
                break;
            case 9:
                Q_EMIT m_controller.stopBackgroundServiceRequested();
                break;
            default:
                break;
            }
        }

        DaemonTrayController& m_controller;
        quint32 m_revision = 1;
    };

    DaemonTrayController::DaemonTrayController(WorkScheduler& workScheduler, QObject* parent)
        : DaemonTrayController(workScheduler, std::chrono::seconds{1}, parent)
    {
    }

    DaemonTrayController::DaemonTrayController(
        WorkScheduler& workScheduler, const std::chrono::milliseconds toolTipUpdateInterval,
        QObject* parent)
        : QObject(parent), m_workScheduler(workScheduler)
    {
        qDBusRegisterMetaType<TrayIconPixmap>();
        qDBusRegisterMetaType<QList<TrayIconPixmap>>();
        qDBusRegisterMetaType<TrayToolTip>();
        m_toolTipUpdateTimer.setSingleShot(true);
        m_toolTipUpdateTimer.setInterval(toolTipUpdateInterval);
        connect(&m_toolTipUpdateTimer, &QTimer::timeout, this,
                [this]
                {
                    if (!m_toolTipUpdatePending)
                        return;
                    m_toolTipUpdatePending = false;
                    updateToolTip();
                    if (m_toolTipUpdateTimer.interval() > 0)
                        m_toolTipUpdateTimer.start();
                });
        connect(&m_workScheduler, &WorkScheduler::jobsChanged, this,
                &DaemonTrayController::requestToolTipUpdate);
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
        const auto flatpakId = qEnvironmentVariable("FLATPAK_ID");
        m_serviceName = flatpakId.isEmpty() ? QStringLiteral("org.kde.StatusNotifierItem-%1-1")
                                                  .arg(QCoreApplication::applicationPid())
                                            : flatpakId + QStringLiteral(".StatusNotifierItem");
        if (!bus.registerService(m_serviceName))
        {
            const auto detail = bus.lastError().message();
            qWarning().noquote() << QStringLiteral(
                                        "StatusNotifier tray service %1 could not be registered%2")
                                        .arg(m_serviceName,
                                             detail.isEmpty() ? QString{}
                                                              : QStringLiteral(": %1").arg(detail));
            m_serviceName.clear();
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
            bus.unregisterService(m_serviceName);
            m_serviceName.clear();
            m_menu.reset();
            return false;
        }

        m_toolTipUpdateTimer.stop();
        m_toolTipUpdatePending = false;
        updateToolTip();
        if (m_toolTipUpdateTimer.interval() > 0)
            m_toolTipUpdateTimer.start();
        m_available = true;

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
            const auto reply =
                watcher.call(QStringLiteral("RegisterStatusNotifierItem"), m_serviceName);
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
            if (!m_serviceName.isEmpty())
                bus.unregisterService(m_serviceName);
        }
        m_serviceName.clear();
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
        return QStringLiteral(JAVELIN_ICON_THEME_PATH);
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
        return QStringLiteral("javelinmail");
    }

    TrayToolTip DaemonTrayController::toolTip() const
    {
        return m_toolTip;
    }

    void DaemonTrayController::setInboxUnreadCount(const std::uint64_t unreadCount)
    {
        if (m_inboxUnreadCount == unreadCount)
            return;
        m_inboxUnreadCount = unreadCount;
        if (m_menu != nullptr)
            m_menu->notifyLayoutChanged();
        requestToolTipUpdate();
    }

    void DaemonTrayController::setAttentionRequired(const bool required)
    {
        if (m_attentionRequired == required)
            return;
        m_attentionRequired = required;
        updateStatus();
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
    }

    void DaemonTrayController::requestToolTipUpdate()
    {
        if (m_toolTipUpdateTimer.interval() <= 0)
        {
            updateToolTip();
            return;
        }
        if (m_toolTipUpdateTimer.isActive())
        {
            m_toolTipUpdatePending = true;
            return;
        }
        updateToolTip();
        m_toolTipUpdateTimer.start();
    }

    void DaemonTrayController::updateToolTip()
    {
        const auto tooltip = TrayToolTip{
            .iconName = QStringLiteral("javelinmail"),
            .iconPixmaps = {},
            .title =
                i18np("%1 unread email in Inbox", "%1 unread emails in Inbox", m_inboxUnreadCount),
            .description = runningWorkSummary(m_workScheduler),
        };
        if (m_toolTip != tooltip)
        {
            m_toolTip = tooltip;
            if (m_available)
                Q_EMIT NewToolTip();
        }

        updateStatus();
    }

    void DaemonTrayController::updateStatus()
    {
        const auto status =
            m_attentionRequired ? QStringLiteral("NeedsAttention") : QStringLiteral("Active");
        if (m_status == status)
            return;
        m_status = status;
        if (m_available)
            Q_EMIT NewStatus(m_status);
    }
} // namespace javelin::app
