#pragma once

#include <QByteArray>
#include <QDBusObjectPath>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

#include <chrono>
#include <cstdint>
#include <memory>

class QDBusArgument;

namespace javelin::app
{
    class WorkScheduler;

    struct TrayIconPixmap
    {
        int width = 0;
        int height = 0;
        QByteArray data;

        friend bool operator==(const TrayIconPixmap&, const TrayIconPixmap&) = default;
    };

    struct TrayToolTip
    {
        QString iconName;
        QList<TrayIconPixmap> iconPixmaps;
        QString title;
        QString description;

        friend bool operator==(const TrayToolTip&, const TrayToolTip&) = default;
    };

    QDBusArgument& operator<<(QDBusArgument& argument, const TrayIconPixmap& pixmap);
    const QDBusArgument& operator>>(const QDBusArgument& argument, TrayIconPixmap& pixmap);
    QDBusArgument& operator<<(QDBusArgument& argument, const TrayToolTip& tooltip);
    const QDBusArgument& operator>>(const QDBusArgument& argument, TrayToolTip& tooltip);

    class DaemonTrayController final : public QObject
    {
        Q_OBJECT
        Q_CLASSINFO("D-Bus Interface", "org.kde.StatusNotifierItem")
        Q_PROPERTY(QString Category READ category CONSTANT)
        Q_PROPERTY(QString Id READ id CONSTANT)
        Q_PROPERTY(QString Title READ title CONSTANT)
        Q_PROPERTY(QString Status READ status)
        Q_PROPERTY(int WindowId READ windowId CONSTANT)
        Q_PROPERTY(QString IconThemePath READ iconThemePath CONSTANT)
        Q_PROPERTY(QDBusObjectPath Menu READ menu CONSTANT)
        Q_PROPERTY(bool ItemIsMenu READ itemIsMenu CONSTANT)
        Q_PROPERTY(QString IconName READ iconName CONSTANT)
        Q_PROPERTY(javelin::app::TrayToolTip ToolTip READ toolTip)

      public:
        explicit DaemonTrayController(WorkScheduler& workScheduler, QObject* parent = nullptr);
        DaemonTrayController(WorkScheduler& workScheduler,
                             std::chrono::milliseconds toolTipUpdateInterval,
                             QObject* parent = nullptr);
        ~DaemonTrayController() override;

        DaemonTrayController(const DaemonTrayController&) = delete;
        DaemonTrayController& operator=(const DaemonTrayController&) = delete;
        DaemonTrayController(DaemonTrayController&&) = delete;
        DaemonTrayController& operator=(DaemonTrayController&&) = delete;

        [[nodiscard]] bool start();
        void stop();
        [[nodiscard]] bool isAvailable() const;

        [[nodiscard]] QString category() const;
        [[nodiscard]] QString id() const;
        [[nodiscard]] QString title() const;
        [[nodiscard]] QString status() const;
        [[nodiscard]] int windowId() const;
        [[nodiscard]] QString iconThemePath() const;
        [[nodiscard]] QDBusObjectPath menu() const;
        [[nodiscard]] bool itemIsMenu() const;
        [[nodiscard]] QString iconName() const;
        [[nodiscard]] TrayToolTip toolTip() const;
        void setInboxUnreadCount(std::uint64_t unreadCount);

      public Q_SLOTS:
        void Activate(int x, int y);
        void ContextMenu(int x, int y);
        void ProvideXdgActivationToken(const QString& token);
        void Scroll(int delta, const QString& orientation);
        void SecondaryActivate(int x, int y);

      Q_SIGNALS:
        void raiseGuiRequested(const QString& activationToken);
        void quitRequested();
        void NewTitle();
        void NewIcon();
        void NewMenu();
        void NewToolTip();
        void NewStatus(const QString& status);

      private:
        class Menu;

        void requestToolTipUpdate();
        void updateToolTip();

        WorkScheduler& m_workScheduler;
        QTimer m_toolTipUpdateTimer;
        bool m_toolTipUpdatePending = false;
        std::unique_ptr<Menu> m_menu;
        QString m_activationToken;
        QString m_serviceName;
        QString m_title = QStringLiteral("Javelin Mail");
        QString m_status = QStringLiteral("Passive");
        TrayToolTip m_toolTip;
        std::uint64_t m_inboxUnreadCount = 0;
        bool m_available = false;
    };
} // namespace javelin::app

Q_DECLARE_METATYPE(javelin::app::TrayIconPixmap)
Q_DECLARE_METATYPE(javelin::app::TrayToolTip)
