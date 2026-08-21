#include "gui/calendar/DayAgendaDialog.h"
#include "gui/accessibility/AccessibleFactory.h"
#include "gui/calendar/CalendarEventButton.h"

#include <KLocalizedString>

#include <QAccessible>
#include <QAccessibleWidget>
#include <QButtonGroup>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <limits>
#include <utility>

namespace javelin::gui::calendar
{
    namespace
    {
        constexpr int PixelsPerHour = 64;
        constexpr int QuarterHourPixels = PixelsPerHour / 4;
        constexpr int TimelineHeight = 24 * PixelsPerHour + 1;
        constexpr int TimeGutterHorizontalPadding = 12;
        constexpr int EventGap = 3;

        [[nodiscard]] int timeGutterWidth(const QFontMetrics& metrics)
        {
            const QLocale locale;
            int widest = 0;
            for (int hour = 0; hour < 24; ++hour)
            {
                widest = std::max(widest, metrics.horizontalAdvance(locale.toString(
                                              QTime{hour, 0}, QLocale::ShortFormat)));
            }
            return widest + TimeGutterHorizontalPadding;
        }

        [[nodiscard]] int minuteOfDay(const QDateTime& value, const QDate& day, const bool end)
        {
            if (value.date() < day)
                return 0;
            if (value.date() > day)
                return 24 * 60;
            if (end && value.time() == QTime{0, 0})
                return 0;
            return value.time().hour() * 60 + value.time().minute();
        }

        struct TimedPlacement
        {
            const DayAgendaEvent* event = nullptr;
            int column = 0;
            int columns = 1;
        };

        [[nodiscard]] std::vector<TimedPlacement>
        timedPlacements(const std::vector<const DayAgendaEvent*>& source, const QDate& day)
        {
            auto events = source;
            std::ranges::sort(events,
                              [](const auto* left, const auto* right)
                              {
                                  if (left->start != right->start)
                                      return left->start < right->start;
                                  if (left->end != right->end)
                                      return left->end < right->end;
                                  return left->title.localeAwareCompare(right->title) < 0;
                              });

            std::vector<TimedPlacement> result;
            result.reserve(events.size());
            std::size_t first = 0;
            while (first < events.size())
            {
                auto clusterEnd = events[first]->end;
                std::size_t last = first + 1;
                while (last < events.size() && events[last]->start < clusterEnd)
                {
                    clusterEnd = std::max(clusterEnd, events[last]->end);
                    ++last;
                }

                std::vector<int> columnEnds;
                const auto resultStart = result.size();
                for (std::size_t index = first; index < last; ++index)
                {
                    const auto startMinute = minuteOfDay(events[index]->start, day, false);
                    auto endMinute = minuteOfDay(events[index]->end, day, true);
                    endMinute = std::max(endMinute, startMinute + 1);
                    int column = 0;
                    for (; column < static_cast<int>(columnEnds.size()); ++column)
                    {
                        if (columnEnds[static_cast<std::size_t>(column)] <= startMinute)
                            break;
                    }
                    if (column == static_cast<int>(columnEnds.size()))
                        columnEnds.push_back(endMinute);
                    else
                        columnEnds[static_cast<std::size_t>(column)] = endMinute;
                    result.push_back({.event = events[index], .column = column, .columns = 1});
                }
                const auto columns = std::max(1, static_cast<int>(columnEnds.size()));
                for (auto index = resultStart; index < result.size(); ++index)
                    result[index].columns = columns;
                first = last;
            }
            return result;
        }

        [[nodiscard]] QString eventAccessibleName(const DayAgendaEvent& event, const QDate& day)
        {
            QString name;
            if (event.allDay)
                name = i18nc("accessible all-day calendar event", "All day, %1", event.title);
            else
            {
                const auto lastDate =
                    event.end.time() == QTime{0, 0} && event.end.date() > event.start.date()
                        ? event.end.date().addDays(-1)
                        : event.end.date();
                const auto beginsBefore = event.start.date() < day;
                const auto endsAfter = lastDate > day;
                if (beginsBefore && endsAfter)
                {
                    name = i18nc("accessible calendar event spanning the whole day",
                                 "%1, continues from previous day and to next day", event.title);
                }
                else if (beginsBefore)
                {
                    name = i18nc("accessible calendar event continuing into this day",
                                 "Until %1, %2, continues from previous day",
                                 QLocale{}.toString(event.end.time(), QLocale::ShortFormat),
                                 event.title);
                }
                else if (endsAfter)
                {
                    name = i18nc("accessible calendar event continuing beyond this day",
                                 "%1 onward, %2, continues to next day",
                                 QLocale{}.toString(event.start.time(), QLocale::ShortFormat),
                                 event.title);
                }
                else
                {
                    name = i18nc("accessible timed calendar event", "%1 to %2, %3",
                                 QLocale{}.toString(event.start.time(), QLocale::ShortFormat),
                                 QLocale{}.toString(event.end.time(), QLocale::ShortFormat),
                                 event.title);
                }
            }
            if (event.recurring)
                name += i18nc("accessible recurring calendar event suffix", ", recurring");
            return name;
        }

        class DayAgendaDetailsPane final : public QScrollArea
        {
          public:
            using QScrollArea::QScrollArea;

            void setControllingButton(CalendarEventButton* button)
            {
                m_controllingButton = button;
            }

            [[nodiscard]] CalendarEventButton* controllingButton() const
            {
                return m_controllingButton;
            }

          private:
            QPointer<CalendarEventButton> m_controllingButton;
        };

        [[nodiscard]] CalendarEventButton* createAgendaEventButton(const DayAgendaEvent& event,
                                                                   const QDate& date,
                                                                   QWidget* detailsTarget,
                                                                   QWidget* parent)
        {
            auto* button = new CalendarEventButton(parent);
            button->setObjectName(QStringLiteral("dayAgendaEventButton"));
            button->setCheckable(true);
            button->setCalendarEventPresentation(event.title, event.start, event.end, event.allDay,
                                                 event.recurring, date,
                                                 eventAccessibleName(event, date), event.color);
            button->setControlledWidget(detailsTarget);
            button->setProperty("agendaAccountId", event.key.accountId);
            button->setProperty("agendaEventId", event.key.eventId);
            button->setProperty("agendaRecurrenceId", event.key.recurrenceId);
            button->show();
            return button;
        }

        class DayTimelineWidget final : public QWidget
        {
          public:
            explicit DayTimelineWidget(QWidget* parent = nullptr) : QWidget(parent)
            {
                setObjectName(QStringLiteral("dayAgendaTimeline"));
                setMinimumHeight(TimelineHeight);
                setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            }

            void setEvents(
                const QDate& date, const std::vector<DayAgendaEvent>& events,
                QWidget* detailsTarget, std::function<void(const DayAgendaEventKey&)> selected,
                std::function<void(const QDateTime&, const QDateTime&)> create,
                std::function<void(const QPoint&, const DayAgendaEventKey&)> contextMenuRequested)
            {
                m_date = date;
                m_selected = std::move(selected);
                m_create = std::move(create);
                delete std::exchange(m_newEventPreview, nullptr);
                m_pressedBlock.reset();
                for (auto* button : std::exchange(m_buttons, {}))
                    delete button;

                std::vector<const DayAgendaEvent*> timed;
                for (const auto& event : events)
                    if (!event.allDay)
                        timed.push_back(&event);
                setAccessibleName(
                    i18n("Timed schedule for %1", QLocale{}.toString(date, QLocale::LongFormat)));
                setAccessibleDescription(i18ncp("accessible timed calendar event count",
                                                "%1 timed event from midnight to midnight",
                                                "%1 timed events from midnight to midnight",
                                                static_cast<int>(timed.size())));
                if (QAccessible::isActive())
                {
                    QAccessibleEvent nameChanged{this, QAccessible::NameChanged};
                    QAccessible::updateAccessibility(&nameChanged);
                    QAccessibleEvent descriptionChanged{this, QAccessible::DescriptionChanged};
                    QAccessible::updateAccessibility(&descriptionChanged);
                }
                for (const auto& placement : timedPlacements(timed, date))
                {
                    auto* button =
                        createAgendaEventButton(*placement.event, date, detailsTarget, this);
                    button->setProperty("agendaColumn", placement.column);
                    button->setProperty("agendaColumns", placement.columns);
                    button->setProperty("agendaStartMinute",
                                        minuteOfDay(placement.event->start, date, false));
                    const auto endMinute = minuteOfDay(placement.event->end, date, true);
                    button->setProperty("agendaEndMinute", endMinute);
                    QObject::connect(button, &QToolButton::clicked, button,
                                     [this, key = placement.event->key]
                                     {
                                         if (m_selected)
                                             m_selected(key);
                                     });
                    QObject::connect(
                        button, &CalendarEventButton::contextMenuRequested, button,
                        [key = placement.event->key, contextMenuRequested](const QPoint& globalPos)
                        { contextMenuRequested(globalPos, key); });
                    m_buttons.push_back(button);
                }
                layoutButtons();
                update();
            }

            [[nodiscard]] const std::vector<CalendarEventButton*>& buttons() const
            {
                return m_buttons;
            }

          protected:
            void paintEvent(QPaintEvent* event) override
            {
                Q_UNUSED(event);
                QPainter painter{this};
                painter.fillRect(rect(), palette().color(QPalette::Base));
                const auto pale = palette().color(QPalette::Midlight);
                const auto dark = palette().color(QPalette::Mid);
                const auto text = palette().color(QPalette::Text);
                const auto gutterWidth = timeGutterWidth(fontMetrics());
                for (int quarter = 0; quarter <= 24 * 4; ++quarter)
                {
                    const auto y = quarter * QuarterHourPixels;
                    painter.setPen(quarter % 4 == 0 ? dark : pale);
                    painter.drawLine(gutterWidth, y, width(), y);
                    if (quarter < 24 * 4 && quarter % 4 == 0)
                    {
                        painter.setPen(text);
                        const auto hour = quarter / 4;
                        painter.drawText(QRect{4, y + 2, gutterWidth - TimeGutterHorizontalPadding,
                                               fontMetrics().height()},
                                         Qt::AlignRight | Qt::AlignTop,
                                         QLocale{}.toString(QTime{hour, 0}, QLocale::ShortFormat));
                    }
                }
            }

            void resizeEvent(QResizeEvent* event) override
            {
                QWidget::resizeEvent(event);
                layoutButtons();
                layoutNewEventPreview();
            }

            void mousePressEvent(QMouseEvent* event) override
            {
                if (event->button() != Qt::LeftButton ||
                    event->position().x() < timeGutterWidth(fontMetrics()))
                {
                    QWidget::mousePressEvent(event);
                    return;
                }

                m_pressedBlock = blockAt(event->position().y());
                m_dragged = false;
                showNewEventPreview(*m_pressedBlock, *m_pressedBlock + 3);
                event->accept();
            }

            void mouseMoveEvent(QMouseEvent* event) override
            {
                if (!m_pressedBlock || !event->buttons().testFlag(Qt::LeftButton))
                {
                    QWidget::mouseMoveEvent(event);
                    return;
                }

                m_dragged = true;
                const auto currentBlock = blockAt(event->position().y());
                showNewEventPreview(std::min(*m_pressedBlock, currentBlock),
                                    std::max(*m_pressedBlock, currentBlock));
                event->accept();
            }

            void mouseReleaseEvent(QMouseEvent* event) override
            {
                if (event->button() != Qt::LeftButton || !m_pressedBlock)
                {
                    QWidget::mouseReleaseEvent(event);
                    return;
                }

                const auto releasedBlock = blockAt(event->position().y());
                const auto firstBlock =
                    m_dragged ? std::min(*m_pressedBlock, releasedBlock) : *m_pressedBlock;
                const auto lastBlock =
                    m_dragged ? std::max(*m_pressedBlock, releasedBlock) : *m_pressedBlock + 3;
                const auto start = QDateTime{m_date, QTime{0, 0}}.addSecs(firstBlock * 15 * 60);
                const auto end = QDateTime{m_date, QTime{0, 0}}.addSecs((lastBlock + 1) * 15 * 60);
                delete std::exchange(m_newEventPreview, nullptr);
                m_pressedBlock.reset();
                if (m_create)
                    m_create(start, end);
                event->accept();
            }

          private:
            [[nodiscard]] static int blockAt(const qreal y)
            {
                return std::clamp(static_cast<int>(y) / QuarterHourPixels, 0, 24 * 4 - 1);
            }

            void showNewEventPreview(const int firstBlock, const int lastBlock)
            {
                if (m_newEventPreview == nullptr)
                {
                    m_newEventPreview = new CalendarEventButton(this);
                    m_newEventPreview->setObjectName(QStringLiteral("dayAgendaNewEventButton"));
                    m_newEventPreview->setFocusPolicy(Qt::NoFocus);
                    m_newEventPreview->setAttribute(Qt::WA_TransparentForMouseEvents);
                    m_newEventPreview->setEventPresentation(i18n("New Event"), i18n("New Event"),
                                                            palette().color(QPalette::Highlight));
                    m_newEventPreview->show();
                    m_newEventPreview->raise();
                }
                m_newEventPreview->setProperty("agendaStartMinute", firstBlock * 15);
                m_newEventPreview->setProperty("agendaEndMinute", (lastBlock + 1) * 15);
                layoutNewEventPreview();
            }

            void layoutNewEventPreview()
            {
                if (m_newEventPreview == nullptr)
                    return;
                const auto gutterWidth = timeGutterWidth(fontMetrics());
                const auto startMinute = std::clamp(
                    m_newEventPreview->property("agendaStartMinute").toInt(), 0, 24 * 60);
                const auto endMinute =
                    std::clamp(m_newEventPreview->property("agendaEndMinute").toInt(), 0, 24 * 60);
                const auto y = startMinute * PixelsPerHour / 60 + 1;
                const auto height =
                    std::max(22, (endMinute - startMinute) * PixelsPerHour / 60 - 2);
                m_newEventPreview->setGeometry(gutterWidth + EventGap, y,
                                               std::max(24, width() - gutterWidth - 2 * EventGap),
                                               height);
            }

            void layoutButtons()
            {
                const auto gutterWidth = timeGutterWidth(fontMetrics());
                const auto availableWidth = std::max(1, width() - gutterWidth - EventGap);
                for (auto* button : m_buttons)
                {
                    const auto columns = std::max(1, button->property("agendaColumns").toInt());
                    const auto column = button->property("agendaColumn").toInt();
                    const auto columnWidth = availableWidth / columns;
                    const auto startMinute =
                        std::clamp(button->property("agendaStartMinute").toInt(), 0, 24 * 60);
                    auto endMinute =
                        std::clamp(button->property("agendaEndMinute").toInt(), 0, 24 * 60);
                    if (endMinute <= startMinute)
                        endMinute = std::min(24 * 60, startMinute + 15);
                    const auto x = gutterWidth + column * columnWidth + EventGap;
                    const auto y = startMinute * PixelsPerHour / 60 + 1;
                    const auto height =
                        std::max(22, (endMinute - startMinute) * PixelsPerHour / 60 - 2);
                    button->setGeometry(x, y, std::max(24, columnWidth - EventGap), height);
                }
            }

            QDate m_date;
            std::function<void(const DayAgendaEventKey&)> m_selected;
            std::function<void(const QDateTime&, const QDateTime&)> m_create;
            std::vector<CalendarEventButton*> m_buttons;
            CalendarEventButton* m_newEventPreview = nullptr;
            std::optional<int> m_pressedBlock;
            bool m_dragged = false;
        };

        class AccessibleDayTimeline final : public QAccessibleWidget
        {
          public:
            explicit AccessibleDayTimeline(DayTimelineWidget* timeline)
                : QAccessibleWidget(timeline, QAccessible::Pane)
            {
            }
        };

        class AccessibleDayAgendaDetails final : public QAccessibleWidget
        {
          public:
            explicit AccessibleDayAgendaDetails(DayAgendaDetailsPane* details)
                : QAccessibleWidget(details, QAccessible::Pane)
            {
            }

            [[nodiscard]] QList<std::pair<QAccessibleInterface*, QAccessible::Relation>>
            relations(const QAccessible::Relation match = QAccessible::AllRelations) const override
            {
                auto result = QAccessibleWidget::relations(match);
                const auto* details = detailsPane();
                if (details == nullptr || details->controllingButton() == nullptr ||
                    !match.testFlag(QAccessible::Controlled))
                    return result;
                if (auto* controller =
                        QAccessible::queryAccessibleInterface(details->controllingButton());
                    controller != nullptr)
                    result.push_back({controller, QAccessible::Controlled});
                return result;
            }

          private:
            [[nodiscard]] DayAgendaDetailsPane* detailsPane() const
            {
                return accessibility::namedObject<DayAgendaDetailsPane, QScrollArea>(
                    object(), QLatin1StringView{"dayAgendaDetailsPane"});
            }
        };

        [[nodiscard]] QAccessibleInterface* dayAgendaAccessibleFactory(const QString& key,
                                                                       QObject* object)
        {
            if (auto* timeline = accessibility::namedFactoryObject<DayTimelineWidget, QWidget>(
                    key, object, QLatin1StringView{"dayAgendaTimeline"});
                timeline != nullptr)
                return new AccessibleDayTimeline(timeline);
            if (auto* details =
                    accessibility::namedFactoryObject<DayAgendaDetailsPane, QScrollArea>(
                        key, object, QLatin1StringView{"dayAgendaDetailsPane"});
                details != nullptr)
                return new AccessibleDayAgendaDetails(details);
            return nullptr;
        }

        void ensureDayAgendaAccessibilityFactoryInstalled()
        {
            static const bool installed = []
            {
                QAccessible::installFactory(dayAgendaAccessibleFactory);
                return true;
            }();
            Q_UNUSED(installed);
        }

        [[nodiscard]] bool buttonMatches(const CalendarEventButton* button,
                                         const DayAgendaEventKey& key)
        {
            return button != nullptr &&
                   button->property("agendaAccountId").toString() == key.accountId &&
                   button->property("agendaEventId").toString() == key.eventId &&
                   button->property("agendaRecurrenceId").toString() == key.recurrenceId;
        }

        void setDetailLabel(QLabel* label, const QString& prefix, const QString& value)
        {
            label->setVisible(!value.isEmpty());
            label->setText(value.isEmpty()
                               ? QString{}
                               : i18nc("calendar event detail", "%1: %2", prefix, value));
        }

        [[nodiscard]] bool hasCalendarResponse(const QString& participationStatus)
        {
            return !participationStatus.isEmpty() &&
                   participationStatus != QStringLiteral("needs-action");
        }

        [[nodiscard]] QString calendarResponseLabel(const QString& participationStatus)
        {
            if (participationStatus == QStringLiteral("accepted"))
                return i18nc("calendar RSVP status", "accepted");
            if (participationStatus == QStringLiteral("tentative"))
                return i18nc("calendar RSVP status", "tentative");
            if (participationStatus == QStringLiteral("declined"))
                return i18nc("calendar RSVP status", "declined");
            return participationStatus;
        }
    } // namespace

    DayAgendaDialog::DayAgendaDialog(QWidget* parent) : QDialog(parent)
    {
        ensureDayAgendaAccessibilityFactoryInstalled();
        setWindowTitle(i18n("Day Agenda"));
        setModal(true);
        resize(980, 720);

        auto* outer = new QVBoxLayout(this);
        auto* header = new QHBoxLayout;
        m_previousDay = new QToolButton(this);
        m_previousDay->setIcon(QIcon::fromTheme(QStringLiteral("go-previous")));
        m_previousDay->setAccessibleName(i18n("Previous day"));
        m_previousDay->setToolTip(i18n("Previous day"));
        header->addWidget(m_previousDay);
        m_dateLabel = new QLabel(this);
        m_dateLabel->setAlignment(Qt::AlignCenter);
        auto dateFont = m_dateLabel->font();
        dateFont.setBold(true);
        m_dateLabel->setFont(dateFont);
        header->addWidget(m_dateLabel, 1);
        m_nextDay = new QToolButton(this);
        m_nextDay->setIcon(QIcon::fromTheme(QStringLiteral("go-next")));
        m_nextDay->setAccessibleName(i18n("Next day"));
        m_nextDay->setToolTip(i18n("Next day"));
        header->addWidget(m_nextDay);
        m_newEvent = new QPushButton(QIcon::fromTheme(QStringLiteral("appointment-new")),
                                     i18n("New Event"), this);
        header->addWidget(m_newEvent);
        outer->addLayout(header);

        auto* splitter = new QSplitter(Qt::Horizontal, this);
        auto* schedule = new QWidget(splitter);
        auto* scheduleLayout = new QVBoxLayout(schedule);
        scheduleLayout->setContentsMargins(0, 0, 0, 0);
        m_allDayPanel = new QWidget(schedule);
        m_allDayPanel->setAccessibleName(i18n("All-day events"));
        m_allDayLayout = new QVBoxLayout(m_allDayPanel);
        m_allDayLayout->setContentsMargins(4, 0, 4, 4);
        auto* allDayLabel = new QLabel(i18n("All day"), m_allDayPanel);
        auto allDayFont = allDayLabel->font();
        allDayFont.setBold(true);
        allDayLabel->setFont(allDayFont);
        m_allDayLayout->addWidget(allDayLabel);
        scheduleLayout->addWidget(m_allDayPanel);

        m_timelineScroll = new QScrollArea(schedule);
        m_timelineScroll->setObjectName(QStringLiteral("dayAgendaTimelineScroll"));
        m_timelineScroll->setFocusPolicy(Qt::NoFocus);
        m_timelineScroll->setWidgetResizable(true);
        m_timeline = new DayTimelineWidget(m_timelineScroll);
        m_timelineScroll->setWidget(m_timeline);
        connect(m_timelineScroll->verticalScrollBar(), &QScrollBar::rangeChanged, this,
                [this](const int, const int maximum)
                {
                    if (m_initialScrollPending && maximum > 0 && isVisible())
                        scheduleInitialScroll();
                });
        scheduleLayout->addWidget(m_timelineScroll, 1);

        m_detailsScroll = new DayAgendaDetailsPane(splitter);
        m_detailsScroll->setObjectName(QStringLiteral("dayAgendaDetailsPane"));
        m_detailsScroll->setAccessibleName(i18n("Event details"));
        m_detailsScroll->setWidgetResizable(true);
        m_detailsScroll->setMinimumWidth(300);
        auto* details = new QWidget(m_detailsScroll);
        auto* detailsLayout = new QVBoxLayout(details);
        detailsLayout->setAlignment(Qt::AlignTop);
        m_detailsTitle = new QLabel(details);
        m_detailsTitle->setObjectName(QStringLiteral("dayAgendaDetailsTitle"));
        m_detailsTitle->setWordWrap(true);
        auto titleFont = m_detailsTitle->font();
        titleFont.setBold(true);
        titleFont.setPointSizeF(titleFont.pointSizeF() * 1.25);
        m_detailsTitle->setFont(titleFont);
        detailsLayout->addWidget(m_detailsTitle);
        m_detailsWhen = new QLabel(details);
        m_detailsCalendar = new QLabel(details);
        m_detailsLocation = new QLabel(details);
        m_detailsOrganizer = new QLabel(details);
        m_detailsAttendees = new QLabel(details);
        m_detailsDescription = new QLabel(details);
        for (auto* label : {m_detailsWhen, m_detailsCalendar, m_detailsLocation, m_detailsOrganizer,
                            m_detailsAttendees, m_detailsDescription})
        {
            label->setWordWrap(true);
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
            detailsLayout->addWidget(label);
        }
        m_responseLabel = new QLabel(i18n("Your response"), details);
        m_responseLabel->setObjectName(QStringLiteral("dayAgendaResponseLabel"));
        auto responseLabelFont = m_responseLabel->font();
        responseLabelFont.setBold(true);
        m_responseLabel->setFont(responseLabelFont);
        detailsLayout->addWidget(m_responseLabel);
        m_changeResponse = new QCheckBox(i18n("Change response"), details);
        m_changeResponse->setObjectName(QStringLiteral("dayAgendaChangeResponse"));
        detailsLayout->addWidget(m_changeResponse);
        auto* responseRow = new QHBoxLayout;
        m_responseButtons = new QButtonGroup(this);
        m_responseButtons->setExclusive(true);
        m_accept = new QPushButton(i18nc("@action:button calendar RSVP", "Accept"), details);
        m_accept->setObjectName(QStringLiteral("dayAgendaRsvpAccept"));
        m_tentative = new QPushButton(i18nc("@action:button calendar RSVP", "Tentative"), details);
        m_tentative->setObjectName(QStringLiteral("dayAgendaRsvpTentative"));
        m_decline = new QPushButton(i18nc("@action:button calendar RSVP", "Decline"), details);
        m_decline->setObjectName(QStringLiteral("dayAgendaRsvpDecline"));
        for (auto* button : {m_accept, m_tentative, m_decline})
        {
            button->setCheckable(true);
            m_responseButtons->addButton(button);
            responseRow->addWidget(button);
        }
        detailsLayout->addLayout(responseRow);
        m_responseSeriesNote = new QLabel(details);
        m_responseSeriesNote->setWordWrap(true);
        detailsLayout->addWidget(m_responseSeriesNote);
        m_responseError = new QLabel(details);
        m_responseError->setObjectName(QStringLiteral("dayAgendaResponseError"));
        m_responseError->setWordWrap(true);
        m_responseError->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                                 Qt::TextSelectableByKeyboard);
        detailsLayout->addWidget(m_responseError);
        m_edit = new QPushButton(QIcon::fromTheme(QStringLiteral("document-edit")), i18n("Edit"),
                                 details);
        m_edit->setObjectName(QStringLiteral("dayAgendaEditButton"));
        detailsLayout->addWidget(m_edit);
        detailsLayout->addStretch(1);
        m_detailsScroll->setWidget(details);
        splitter->addWidget(schedule);
        splitter->addWidget(m_detailsScroll);
        splitter->setStretchFactor(0, 3);
        splitter->setStretchFactor(1, 2);
        outer->addWidget(splitter, 1);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        m_close = buttons->button(QDialogButtonBox::Close);
        outer->addWidget(buttons);

        connect(m_previousDay, &QToolButton::clicked, this,
                [this] { requestDay(m_date.addDays(-1)); });
        connect(m_nextDay, &QToolButton::clicked, this, [this] { requestDay(m_date.addDays(1)); });
        connect(m_newEvent, &QPushButton::clicked, this,
                [this]
                {
                    const QDateTime start{m_date, QTime{9, 0}};
                    Q_EMIT newEventRequested(start, start.addSecs(60 * 60));
                });
        connect(m_edit, &QPushButton::clicked, this,
                [this]
                {
                    if (!m_selectedEvent)
                        return;
                    const auto* selected = eventForKey(*m_selectedEvent);
                    if (selected == nullptr || !selected->editable)
                        return;
                    Q_EMIT editRequested(m_selectedEvent->accountId, m_selectedEvent->eventId,
                                         m_selectedEvent->recurrenceId);
                });
        const auto connectResponse = [this](QPushButton* button, QString status)
        {
            connect(button, &QPushButton::clicked, this,
                    [this, status = std::move(status)]
                    {
                        if (!m_selectedEvent)
                            return;
                        const auto found =
                            std::ranges::find(m_events, *m_selectedEvent, &DayAgendaEvent::key);
                        if (found == m_events.end() || !found->rsvpAllowed ||
                            found->responseMutationPending)
                            return;
                        found->responseMutationPending = true;
                        found->responseError.clear();
                        updateDetails(*found);
                        Q_EMIT responseRequested(found->key.accountId, found->key.eventId,
                                                 found->rsvpRecurrenceId, status);
                    });
        };
        connectResponse(m_accept, QStringLiteral("accepted"));
        connectResponse(m_tentative, QStringLiteral("tentative"));
        connectResponse(m_decline, QStringLiteral("declined"));
        connect(m_changeResponse, &QCheckBox::toggled, this,
                [this]
                {
                    if (!m_selectedEvent)
                        return;
                    const auto* selected = eventForKey(*m_selectedEvent);
                    if (selected != nullptr)
                        updateDetails(*selected);
                });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        rebuildTabOrder();

        auto* previousShortcut = new QShortcut(QKeySequence{Qt::ALT | Qt::Key_Left}, this);
        connect(previousShortcut, &QShortcut::activated, this,
                [this] { requestDay(m_date.addDays(-1)); });
        auto* nextShortcut = new QShortcut(QKeySequence{Qt::ALT | Qt::Key_Right}, this);
        connect(nextShortcut, &QShortcut::activated, this,
                [this] { requestDay(m_date.addDays(1)); });

        clearDetails();
    }

    void DayAgendaDialog::setDay(const QDate date, std::vector<DayAgendaEvent> events,
                                 std::optional<DayAgendaEventKey> selectedEvent)
    {
        const auto dayChanged = date != m_date;
        const auto previousSelection = m_selectedEvent;
        m_date = date;
        m_events = std::move(events);
        if (selectedEvent)
        {
            if (eventForKey(*selectedEvent) != nullptr)
                m_selectedEvent = std::move(selectedEvent);
            else
                m_selectedEvent.reset();
        }
        else if (dayChanged || (m_selectedEvent && eventForKey(*m_selectedEvent) == nullptr))
        {
            m_selectedEvent.reset();
        }
        const bool selectionChanged = previousSelection != m_selectedEvent;
        if (selectionChanged)
        {
            const QSignalBlocker blocker{m_changeResponse};
            m_changeResponse->setChecked(false);
            m_displayedParticipationStatus.clear();
        }
        updateDatePresentation();
        rebuildEvents();
        if (m_selectedEvent && eventForKey(*m_selectedEvent) != nullptr)
            selectEvent(*m_selectedEvent);
        else
            clearDetails();
        if (isVisible() && m_initialScrollPending)
            scheduleInitialScroll();
    }

    void DayAgendaDialog::setResponseMutationPending(const DayAgendaEventKey& eventKey,
                                                     const bool pending, QString error)
    {
        const auto found = std::ranges::find(m_events, eventKey, &DayAgendaEvent::key);
        if (found == m_events.end())
            return;
        found->responseMutationPending = pending;
        found->responseError = std::move(error);
        if (m_selectedEvent && *m_selectedEvent == eventKey)
            updateDetails(*found);
    }

    QDate DayAgendaDialog::date() const
    {
        return m_date;
    }

    std::optional<DayAgendaEventKey> DayAgendaDialog::selectedEvent() const
    {
        return m_selectedEvent;
    }

    void DayAgendaDialog::showEvent(QShowEvent* event)
    {
        QDialog::showEvent(event);
        scheduleInitialScroll();
    }

    void DayAgendaDialog::requestDay(const QDate date)
    {
        if (!date.isValid() || date == m_date)
            return;
        Q_EMIT dayChanged(date);
    }

    void DayAgendaDialog::updateDatePresentation()
    {
        const auto dateText = QLocale{}.toString(m_date, QLocale::LongFormat);
        m_dateLabel->setText(dateText);
        setWindowTitle(i18nc("@title:window", "Day Agenda — %1", dateText));
        setAccessibleName(i18n("Agenda for %1", dateText));
    }

    void DayAgendaDialog::rebuildEvents()
    {
        while (m_allDayLayout->count() > 1)
        {
            auto* item = m_allDayLayout->takeAt(1);
            delete item->widget();
            delete item;
        }
        m_eventButtons.clear();

        bool hasAllDay = false;
        for (const auto& event : m_events)
        {
            if (!event.allDay)
                continue;
            hasAllDay = true;
            auto* button = createAgendaEventButton(event, m_date, m_detailsScroll, m_allDayPanel);
            connect(button, &QToolButton::clicked, this,
                    [this, key = event.key] { selectEvent(key); });
            connect(button, &CalendarEventButton::contextMenuRequested, this,
                    [this, key = event.key](const QPoint& globalPos)
                    {
                        Q_EMIT eventContextMenuRequested(globalPos, key.accountId, key.eventId,
                                                         key.recurrenceId);
                    });
            m_allDayLayout->addWidget(button);
            m_eventButtons.push_back(button);
        }
        m_allDayPanel->setVisible(hasAllDay);

        auto* timeline = static_cast<DayTimelineWidget*>(m_timeline);
        timeline->setEvents(
            m_date, m_events, m_detailsScroll, [this](const DayAgendaEventKey& key)
            { selectEvent(key); }, [this](const QDateTime& start, const QDateTime& end)
            { Q_EMIT newEventRequested(start, end); },
            [this](const QPoint& globalPosition, const DayAgendaEventKey& key)
            {
                Q_EMIT eventContextMenuRequested(globalPosition, key.accountId, key.eventId,
                                                 key.recurrenceId);
            });
        for (auto* button : timeline->buttons())
            m_eventButtons.push_back(button);

        if (m_selectedEvent)
        {
            for (auto* button : m_eventButtons)
                button->setChecked(buttonMatches(button, *m_selectedEvent));
        }
        rebuildTabOrder();
    }

    void DayAgendaDialog::selectEvent(const DayAgendaEventKey& key, const bool focusButton)
    {
        const auto* selected = eventForKey(key);
        if (selected == nullptr)
            return;
        const bool selectionChanged = !m_selectedEvent || *m_selectedEvent != key;
        m_selectedEvent = key;
        if (selectionChanged)
        {
            const QSignalBlocker blocker{m_changeResponse};
            m_changeResponse->setChecked(false);
            m_displayedParticipationStatus.clear();
        }
        CalendarEventButton* controllingButton = nullptr;
        for (auto* button : m_eventButtons)
        {
            const auto matches = buttonMatches(button, key);
            button->setChecked(matches);
            if (matches)
            {
                controllingButton = button;
                if (focusButton)
                    button->setFocus(Qt::OtherFocusReason);
            }
        }
        static_cast<DayAgendaDetailsPane*>(m_detailsScroll)
            ->setControllingButton(controllingButton);
        updateDetails(*selected);
        if (QAccessible::isActive())
        {
            QAccessibleEvent relationChanged{m_detailsScroll, QAccessible::ObjectAttributeChanged};
            QAccessible::updateAccessibility(&relationChanged);
        }
    }

    void DayAgendaDialog::clearDetails()
    {
        static_cast<DayAgendaDetailsPane*>(m_detailsScroll)->setControllingButton(nullptr);
        m_detailsScroll->setAccessibleDescription(i18n("No event selected"));
        m_detailsTitle->setText(i18n("Select an event to see details"));
        for (auto* label : {m_detailsWhen, m_detailsCalendar, m_detailsLocation, m_detailsOrganizer,
                            m_detailsAttendees, m_detailsDescription})
        {
            label->clear();
            label->hide();
        }
        m_responseLabel->hide();
        {
            const QSignalBlocker blocker{m_changeResponse};
            m_changeResponse->setChecked(false);
        }
        m_changeResponse->hide();
        m_displayedParticipationStatus.clear();
        m_accept->hide();
        m_tentative->hide();
        m_decline->hide();
        m_responseSeriesNote->clear();
        m_responseSeriesNote->hide();
        m_responseError->clear();
        m_responseError->hide();
        m_edit->setEnabled(false);
        m_edit->hide();
        rebuildTabOrder();
        if (QAccessible::isActive())
        {
            QAccessibleEvent descriptionChanged{m_detailsScroll, QAccessible::DescriptionChanged};
            QAccessible::updateAccessibility(&descriptionChanged);
            QAccessibleEvent relationChanged{m_detailsScroll, QAccessible::ObjectAttributeChanged};
            QAccessible::updateAccessibility(&relationChanged);
        }
    }

    void DayAgendaDialog::updateDetails(const DayAgendaEvent& event)
    {
        m_detailsTitle->setText(event.title.isEmpty() ? i18n("Untitled event") : event.title);
        QString when;
        if (event.allDay)
            when = i18n("All day");
        else if (event.start.date() != event.end.date())
            when = i18nc("calendar event date and time range", "%1 – %2",
                         QLocale{}.toString(event.start, QLocale::ShortFormat),
                         QLocale{}.toString(event.end, QLocale::ShortFormat));
        else
            when = i18nc("calendar event time range", "%1 – %2",
                         QLocale{}.toString(event.start.time(), QLocale::ShortFormat),
                         QLocale{}.toString(event.end.time(), QLocale::ShortFormat));
        setDetailLabel(m_detailsWhen, i18n("When"), when);
        setDetailLabel(m_detailsCalendar, i18n("Calendar"), event.calendarName);
        setDetailLabel(m_detailsLocation, i18n("Location"), event.location);
        setDetailLabel(m_detailsOrganizer, i18n("Organizer"), event.organizer);
        setDetailLabel(m_detailsAttendees, i18n("Attendees"),
                       event.attendees.join(QStringLiteral(", ")));
        m_detailsDescription->setVisible(!event.description.isEmpty());
        m_detailsDescription->setText(event.description);

        const bool hasResponded =
            event.rsvpAllowed && hasCalendarResponse(event.participationStatus);
        if (m_displayedParticipationStatus != event.participationStatus)
        {
            const QSignalBlocker blocker{m_changeResponse};
            m_changeResponse->setChecked(false);
            m_displayedParticipationStatus = event.participationStatus;
        }
        const bool changingResponse = hasResponded && m_changeResponse->isChecked();
        const bool showResponseButtons = event.rsvpAllowed && (!hasResponded || changingResponse);
        m_responseLabel->setVisible(event.rsvpAllowed);
        m_responseLabel->setText(
            hasResponded
                ? i18n("Your response: %1", calendarResponseLabel(event.participationStatus))
                : i18n("Your response"));
        m_changeResponse->setVisible(hasResponded);
        m_changeResponse->setEnabled(hasResponded && !event.responseMutationPending);
        for (auto* button : {m_accept, m_tentative, m_decline})
        {
            button->setVisible(showResponseButtons);
            button->setEnabled(showResponseButtons && !event.responseMutationPending);
        }
        if (event.rsvpAllowed)
        {
            m_responseButtons->setExclusive(false);
            m_accept->setChecked(event.participationStatus == QStringLiteral("accepted"));
            m_tentative->setChecked(event.participationStatus == QStringLiteral("tentative"));
            m_decline->setChecked(event.participationStatus == QStringLiteral("declined"));
            m_responseButtons->setExclusive(true);
        }
        m_responseSeriesNote->setVisible(event.rsvpAllowed && event.recurring);
        m_responseSeriesNote->setText(
            event.rsvpAllowed && event.recurring
                ? (event.rsvpRecurrenceId.isEmpty()
                       ? i18n("Your response applies to the entire series.")
                       : i18n("Your response applies only to this occurrence."))
                : QString{});
        m_responseError->setVisible(event.rsvpAllowed && !event.responseError.isEmpty());
        m_responseError->setText(event.responseError);

        QStringList accessibleDetails{
            event.title.isEmpty() ? i18n("Untitled event") : event.title,
            i18n("When: %1", when),
        };
        if (!event.calendarName.isEmpty())
            accessibleDetails.push_back(i18n("Calendar: %1", event.calendarName));
        if (!event.location.isEmpty())
            accessibleDetails.push_back(i18n("Location: %1", event.location));
        if (!event.organizer.isEmpty())
            accessibleDetails.push_back(i18n("Organizer: %1", event.organizer));
        if (!event.attendees.isEmpty())
            accessibleDetails.push_back(
                i18n("Attendees: %1", event.attendees.join(QStringLiteral(", "))));
        if (!event.description.isEmpty())
            accessibleDetails.push_back(event.description);
        if (event.rsvpAllowed)
        {
            accessibleDetails.push_back(
                hasResponded
                    ? i18n("Your response: %1", calendarResponseLabel(event.participationStatus))
                    : i18n("Your response: No response"));
            if (event.recurring)
                accessibleDetails.push_back(
                    event.rsvpRecurrenceId.isEmpty()
                        ? i18n("The response applies to the entire series.")
                        : i18n("The response applies only to this occurrence."));
            if (!event.responseError.isEmpty())
                accessibleDetails.push_back(event.responseError);
        }
        m_detailsScroll->setAccessibleDescription(accessibleDetails.join(QLatin1Char('\n')));
        m_edit->setVisible(event.editable);
        m_edit->setEnabled(event.editable);
        rebuildTabOrder();
        if (QAccessible::isActive())
        {
            QAccessibleEvent descriptionChanged{m_detailsScroll, QAccessible::DescriptionChanged};
            QAccessible::updateAccessibility(&descriptionChanged);
        }
    }

    void DayAgendaDialog::rebuildTabOrder()
    {
        if (m_previousDay == nullptr || m_nextDay == nullptr || m_newEvent == nullptr ||
            m_detailsScroll == nullptr || m_close == nullptr)
            return;
        QWidget::setTabOrder(m_previousDay, m_nextDay);
        QWidget::setTabOrder(m_nextDay, m_newEvent);
        QWidget* previous = m_newEvent;
        for (auto* button : m_eventButtons)
        {
            QWidget::setTabOrder(previous, button);
            previous = button;
        }
        QWidget::setTabOrder(previous, m_detailsScroll);
        previous = m_detailsScroll;
        if (m_changeResponse->isVisible())
        {
            QWidget::setTabOrder(previous, m_changeResponse);
            previous = m_changeResponse;
        }
        for (auto* button : {m_accept, m_tentative, m_decline})
        {
            if (!button->isVisible())
                continue;
            QWidget::setTabOrder(previous, button);
            previous = button;
        }
        if (m_edit->isVisible())
        {
            QWidget::setTabOrder(previous, m_edit);
            previous = m_edit;
        }
        QWidget::setTabOrder(previous, m_close);
    }

    void DayAgendaDialog::scheduleInitialScroll()
    {
        QTimer::singleShot(
            0, this,
            [this]
            {
                if (!m_initialScrollPending || !isVisible())
                    return;

                auto* scrollBar = m_timelineScroll->verticalScrollBar();
                if (scrollBar->maximum() <= 0)
                {
                    if (m_timeline->height() > m_timelineScroll->viewport()->height())
                        return;
                    m_initialScrollPending = false;
                    return;
                }

                scrollBar->setValue(std::min(8 * PixelsPerHour, scrollBar->maximum()));
                if (!m_selectedEvent)
                {
                    m_initialScrollPending = false;
                    return;
                }
                for (auto* button : m_eventButtons)
                {
                    if (!buttonMatches(button, *m_selectedEvent))
                        continue;
                    if (m_timeline->isAncestorOf(button))
                        m_timelineScroll->ensureWidgetVisible(button, 0, 24);
                    button->setFocus(Qt::OtherFocusReason);
                    m_initialScrollPending = false;
                    return;
                }
                m_initialScrollPending = false;
            });
    }

    const DayAgendaEvent* DayAgendaDialog::eventForKey(const DayAgendaEventKey& key) const
    {
        const auto found = std::ranges::find(m_events, key, &DayAgendaEvent::key);
        return found == m_events.end() ? nullptr : &*found;
    }
} // namespace javelin::gui::calendar
