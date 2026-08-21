#pragma once

#include "jmap/calendar/CalendarTypes.h"

#include <QWidget>

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class QCheckBox;
class QComboBox;
class QPushButton;
class QSpinBox;
class QVBoxLayout;

namespace javelin::gui::calendar
{
    class CalendarNotificationEditor final : public QWidget
    {
      public:
        explicit CalendarNotificationEditor(bool allowCalendarDefaults, QWidget* parent = nullptr);

        void
        setAlerts(const std::unordered_map<std::string, javelin::jmap::calendar::Alert>& alerts);
        [[nodiscard]] std::unordered_map<std::string, javelin::jmap::calendar::Alert>
        displayAlerts() const;
        void setUseCalendarDefaults(bool enabled);
        [[nodiscard]] bool useCalendarDefaults() const;
        [[nodiscard]] bool edited() const;
        void resetEdited();

      private:
        struct AlertRow
        {
            QWidget* container = nullptr;
            std::string id;
            javelin::jmap::calendar::Alert original;
            QSpinBox* amount = nullptr;
            QComboBox* unit = nullptr;
            QComboBox* relation = nullptr;
            QPushButton* remove = nullptr;
            bool edited = false;
        };

        void addAlertRow(const std::optional<javelin::jmap::calendar::Alert>& alert = std::nullopt);
        void removeAlertRow(QWidget* row);
        void clearAlertRows();
        void markAlertEdited(QWidget* row);
        void updateEnabledState();

        QCheckBox* m_useCalendarDefaults = nullptr;
        QVBoxLayout* m_rowsLayout = nullptr;
        QPushButton* m_addAlert = nullptr;
        std::vector<AlertRow> m_rows;
        bool m_edited = false;
        bool m_loading = false;
    };
} // namespace javelin::gui::calendar
