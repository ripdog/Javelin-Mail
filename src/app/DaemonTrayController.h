#pragma once

#include <QDBusObjectPath>
#include <QObject>

#include <memory>

namespace javelin::app
{
    class WorkScheduler;

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

      public:
        explicit DaemonTrayController(WorkScheduler& workScheduler, QObject* parent = nullptr);
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

      public Q_SLOTS:
        void Activate(int x, int y);
        void ContextMenu(int x, int y);
        void ProvideXdgActivationToken(const QString& token);
        void Scroll(int delta, const QString& orientation);
        void SecondaryActivate(int x, int y);

      Q_SIGNALS:
        void raiseGuiRequested(const QString& activationToken);
        void taskCenterRequested(const QString& activationToken);
        void refreshRequested();
        void quitRequested();
        void NewTitle();
        void NewIcon();
        void NewMenu();
        void NewStatus(const QString& status);

      private:
        class Menu;

        void updateSummary();

        WorkScheduler& m_workScheduler;
        std::unique_ptr<Menu> m_menu;
        QString m_activationToken;
        QString m_title = QStringLiteral("Javelin Mail");
        QString m_status = QStringLiteral("Passive");
        bool m_available = false;
    };
} // namespace javelin::app
