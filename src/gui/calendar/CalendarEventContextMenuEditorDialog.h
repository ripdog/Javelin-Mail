#pragma once

#include <QDialog>

#include <QString>

#include <vector>

class KActionCollection;
class KActionSelector;
class QListWidgetItem;

namespace javelin::gui::settings
{
    class GuiSettings;
}

namespace javelin::gui::calendar
{
    class CalendarEventContextMenuEditorDialog final : public QDialog
    {
        Q_OBJECT

      public:
        CalendarEventContextMenuEditorDialog(javelin::gui::settings::GuiSettings& settings,
                                             KActionCollection& actions, QWidget* parent = nullptr);

      private:
        void populate(const std::vector<QString>& layout);
        void addSeparator();
        void save();
        [[nodiscard]] std::vector<QString> selectedLayout() const;
        [[nodiscard]] QListWidgetItem* itemForId(const QString& id) const;

        javelin::gui::settings::GuiSettings& m_settings;
        KActionCollection& m_actions;
        KActionSelector* m_selector = nullptr;
    };
} // namespace javelin::gui::calendar
