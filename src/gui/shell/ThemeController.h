#pragma once

#include <QObject>
#include <QString>

#include <functional>
#include <vector>

class QAction;
class QEvent;
class QToolButton;
class QWidget;

namespace javelin::gui::shell
{
    class QuickFilterController;

    struct ThemedAction
    {
        QAction* action = nullptr;
        QString resourcePath;
    };

    class ThemeController final : public QObject
    {
        Q_OBJECT

      public:
        ThemeController(QWidget& window, QuickFilterController& quickFilterController,
                        QToolButton& messageSortButton, std::function<void()> paletteApplied,
                        QObject* parent = nullptr);
        ~ThemeController() override;

        [[nodiscard]] QAction& darkModeAction() const;
        void setThemedActions(std::vector<ThemedAction> actions);

      protected:
        bool eventFilter(QObject* watched, QEvent* event) override;

      private:
        void setDarkModeEnabled(bool enabled);
        void updateDarkModeAction();
        void scheduleApplicationPaletteRefresh();
        void applyApplicationPalette();
        void updatePaletteDependentIcons();

        QWidget& m_window;
        QuickFilterController& m_quickFilterController;
        QToolButton& m_messageSortButton;
        std::function<void()> m_paletteApplied;
        QAction* m_darkModeAction = nullptr;
        std::vector<ThemedAction> m_themedActions;
        bool m_paletteRefreshPending = false;
    };
} // namespace javelin::gui::shell
