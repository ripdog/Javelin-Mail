#pragma once

#include <QColor>
#include <QDate>
#include <QDateTime>
#include <QDialog>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

class QLabel;
class QPushButton;
class QScrollArea;
class QShowEvent;
class QToolButton;
class QVBoxLayout;
class QWidget;

namespace javelin::gui::calendar
{
    struct DayAgendaEventKey
    {
        QString accountId;
        QString eventId;
        QString recurrenceId;

        bool operator==(const DayAgendaEventKey&) const = default;
    };

    struct DayAgendaEvent
    {
        DayAgendaEventKey key;
        QString title;
        QString calendarName;
        QColor color;
        QDateTime start;
        QDateTime end;
        bool allDay = false;
        bool recurring = false;
        bool editable = false;
        bool invitation = false;
        QString organizer;
        QString location;
        QString description;
        QStringList attendees;
    };

    class DayAgendaDialog final : public QDialog
    {
        Q_OBJECT

      public:
        explicit DayAgendaDialog(QWidget* parent = nullptr);

        void setDay(QDate date, std::vector<DayAgendaEvent> events,
                    std::optional<DayAgendaEventKey> selectedEvent = std::nullopt);
        [[nodiscard]] QDate date() const;
        [[nodiscard]] std::optional<DayAgendaEventKey> selectedEvent() const;

      Q_SIGNALS:
        void dayChanged(QDate date);
        void newEventRequested(QDate date);
        void editRequested(QString accountId, QString eventId, QString recurrenceId);

      protected:
        void showEvent(QShowEvent* event) override;

      private:
        void requestDay(QDate date);
        void rebuildEvents();
        void selectEvent(const DayAgendaEventKey& key, bool focusButton = false);
        void clearDetails();
        void updateDetails(const DayAgendaEvent& event);
        void rebuildTabOrder();
        void scheduleInitialScroll();
        [[nodiscard]] const DayAgendaEvent* eventForKey(const DayAgendaEventKey& key) const;

        QDate m_date;
        std::vector<DayAgendaEvent> m_events;
        std::optional<DayAgendaEventKey> m_selectedEvent;
        QLabel* m_dateLabel = nullptr;
        QToolButton* m_previousDay = nullptr;
        QToolButton* m_nextDay = nullptr;
        QPushButton* m_newEvent = nullptr;
        QWidget* m_allDayPanel = nullptr;
        QVBoxLayout* m_allDayLayout = nullptr;
        QScrollArea* m_timelineScroll = nullptr;
        QWidget* m_timeline = nullptr;
        QScrollArea* m_detailsScroll = nullptr;
        QLabel* m_detailsTitle = nullptr;
        QLabel* m_detailsWhen = nullptr;
        QLabel* m_detailsCalendar = nullptr;
        QLabel* m_detailsLocation = nullptr;
        QLabel* m_detailsOrganizer = nullptr;
        QLabel* m_detailsAttendees = nullptr;
        QLabel* m_detailsDescription = nullptr;
        QPushButton* m_edit = nullptr;
        QPushButton* m_close = nullptr;
        std::vector<QToolButton*> m_eventButtons;
        bool m_initialScrollPending = true;
    };
} // namespace javelin::gui::calendar
