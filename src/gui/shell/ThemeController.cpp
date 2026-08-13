#include "gui/shell/ThemeController.h"

#include "gui/IconUtils.h"
#include "gui/shell/QuickFilterController.h"

#include <KLocalizedString>

#if QT_VERSION < QT_VERSION_CHECK(6, 8, 0)
#include <KColorSchemeManager>
#include <kconfigwidgets_version.h>
#endif

#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QPalette>
#include <QSignalBlocker>
#include <QStyleHints>
#include <QTimer>
#include <QToolButton>
#include <QWidget>

#include <utility>

namespace javelin::gui::shell
{
    Q_LOGGING_CATEGORY(logGuiTheme, "gui.theme")

    ThemeController::ThemeController(QWidget& window, QuickFilterController& quickFilterController,
                                     QToolButton& messageSortButton,
                                     std::function<void()> paletteApplied, QObject* parent)
        : QObject(parent), m_window(window), m_quickFilterController(quickFilterController),
          m_messageSortButton(messageSortButton), m_paletteApplied(std::move(paletteApplied))
    {
        m_darkModeAction = new QAction(QIcon::fromTheme(QStringLiteral("contrast")),
                                       i18nc("@action:button", "Dark Mode"), this);
        m_darkModeAction->setCheckable(true);
        m_darkModeAction->setToolTip(i18nc("@info:tooltip", "Toggle dark mode"));
        connect(m_darkModeAction, &QAction::toggled, this, &ThemeController::setDarkModeEnabled);
        connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this,
                [this]
                {
                    updateDarkModeAction();
                    scheduleApplicationPaletteRefresh();
                });
        qApp->installEventFilter(this);
        m_window.installEventFilter(this);
        updateDarkModeAction();
    }

    ThemeController::~ThemeController()
    {
        if (qApp != nullptr)
            qApp->removeEventFilter(this);
        m_window.removeEventFilter(this);
    }

    QAction& ThemeController::darkModeAction() const
    {
        return *m_darkModeAction;
    }

    void ThemeController::setThemedActions(std::vector<ThemedAction> actions)
    {
        m_themedActions = std::move(actions);
        updatePaletteDependentIcons();
    }

    void ThemeController::setDarkModeEnabled(const bool enabled)
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        QGuiApplication::styleHints()->setColorScheme(enabled ? Qt::ColorScheme::Dark
                                                              : Qt::ColorScheme::Light);
#else
#if KCONFIGWIDGETS_VERSION >= QT_VERSION_CHECK(6, 6, 0)
        auto* colorSchemeManager = KColorSchemeManager::instance();
#else
        KColorSchemeManager localColorSchemeManager;
        auto* colorSchemeManager = &localColorSchemeManager;
#endif
        colorSchemeManager->setAutosaveChanges(false);
        const auto scheme = colorSchemeManager->indexForScheme(
            enabled ? QStringLiteral("Breeze Dark") : QStringLiteral("Breeze Light"));
        if (scheme.isValid())
            colorSchemeManager->activateScheme(scheme);
        else
            qCWarning(logGuiTheme) << "Unable to locate Breeze color scheme for dark mode toggle";
#endif
    }

    void ThemeController::updateDarkModeAction()
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        const auto colorScheme = QGuiApplication::styleHints()->colorScheme();
        const bool enabled =
            colorScheme == Qt::ColorScheme::Dark ||
            (colorScheme == Qt::ColorScheme::Unknown &&
             m_window.palette().color(QPalette::Active, QPalette::Window).lightness() < 128);
#else
        const bool enabled =
            m_window.palette().color(QPalette::Active, QPalette::Window).lightness() < 128;
#endif
        const QSignalBlocker blocker{m_darkModeAction};
        m_darkModeAction->setChecked(enabled);
    }

    void ThemeController::scheduleApplicationPaletteRefresh()
    {
        if (m_paletteRefreshPending)
            return;
        m_paletteRefreshPending = true;
        QTimer::singleShot(0, this,
                           [this]
                           {
                               m_paletteRefreshPending = false;
                               applyApplicationPalette();
                           });
    }

    void ThemeController::applyApplicationPalette()
    {
        const auto descendants = m_window.findChildren<QWidget*>();
        for (auto widget = descendants.crbegin(); widget != descendants.crend(); ++widget)
            (*widget)->setPalette(QPalette{});
        m_window.setPalette(QPalette{});

        updateDarkModeAction();
        updatePaletteDependentIcons();
        if (m_paletteApplied)
            m_paletteApplied();
        m_window.update();
    }

    void ThemeController::updatePaletteDependentIcons()
    {
        const auto iconColor = m_window.palette().color(QPalette::Active, QPalette::Text);
        for (const auto& target : m_themedActions)
        {
            if (target.action != nullptr)
                target.action->setIcon(javelin::gui::themedSvgIcon(target.resourcePath, iconColor));
        }
        m_quickFilterController.updateIcons(iconColor);
        m_messageSortButton.setIcon(javelin::gui::themedSvgIcon(
            QStringLiteral(":/icons/thunderbird-icons/display-options.svg"), iconColor));
    }

    bool ThemeController::eventFilter(QObject* watched, QEvent* event)
    {
        if ((watched == qApp || watched == &m_window) &&
            event->type() == QEvent::ApplicationPaletteChange)
            scheduleApplicationPaletteRefresh();
        return QObject::eventFilter(watched, event);
    }
} // namespace javelin::gui::shell
