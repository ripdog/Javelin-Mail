#include "gui/calendar/MonthCalendarWidget.h"
#include "gui/calendar/MonthCalendarLayout.h"

#include <QActionGroup>
#include <QColorDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>

namespace javelin::gui::calendar
{
    namespace
    {
        [[nodiscard]] double relativeLuminance(const QColor& color)
        {
            const auto channel = [](const double value)
            {
                const auto normalized = value / 255.0;
                return normalized <= 0.04045 ? normalized / 12.92
                                             : std::pow((normalized + 0.055) / 1.055, 2.4);
            };
            return 0.2126 * channel(color.red()) + 0.7152 * channel(color.green()) +
                   0.0722 * channel(color.blue());
        }

        [[nodiscard]] double contrastRatio(const QColor& left, const QColor& right)
        {
            const auto lighter = std::max(relativeLuminance(left), relativeLuminance(right));
            const auto darker = std::min(relativeLuminance(left), relativeLuminance(right));
            return (lighter + 0.05) / (darker + 0.05);
        }

        [[nodiscard]] QIcon colorSwatch(const QColor& color)
        {
            QPixmap swatch{12, 12};
            swatch.fill(color);
            return QIcon{swatch};
        }

        class EventChip final : public QToolButton
        {
          public:
            EventChip(const MonthEvent& event, QWidget* parent) : QToolButton(parent)
            {
                setText((event.allDay ? QString{}
                                      : event.start.time().toString(QStringLiteral("HH:mm "))) +
                        event.title + (event.recurring ? QStringLiteral(" ↻") : QString{}));
                setToolTip(event.title);
                setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
                setAutoRaise(true);
                const auto color =
                    event.color.isValid() ? event.color : palette().color(QPalette::Highlight);
                const auto text = palette().color(QPalette::Text);
                const auto base = palette().color(QPalette::Base);
                const auto foreground =
                    contrastRatio(color, text) >= contrastRatio(color, base) ? text : base;
                setStyleSheet(
                    QStringLiteral("QToolButton { background: %1; color: %2; border-radius: 3px; "
                                   "padding: 1px 4px; text-align: left; }")
                        .arg(color.name(QColor::HexArgb), foreground.name(QColor::HexArgb)));
            }
        };

        QDate eventLastDate(const MonthEvent& event)
        {
            if (event.end.time() == QTime{0, 0} && event.end.date() > event.start.date())
                return event.end.date().addDays(-1);
            return event.end.date();
        }
    } // namespace

    class DayCellWidget final : public QWidget
    {
      public:
        explicit DayCellWidget(QWidget* parent = nullptr) : QWidget(parent)
        {
            setMinimumSize(90, 76);
            setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
            setObjectName(QStringLiteral("calendarDayCell"));
            m_layout = new QVBoxLayout(this);
            m_layout->setContentsMargins(4, 3, 4, 3);
            m_layout->setSpacing(2);
            m_day = new QLabel(this);
            m_layout->addWidget(m_day);
            m_layout->addStretch();
        }

        void setDate(const QDate& date, const bool adjacent, const bool selected)
        {
            m_date = date;
            m_day->setText(QString::number(date.day()));
            const auto border = date == QDate::currentDate()
                                    ? QStringLiteral("2px solid palette(highlight)")
                                    : QStringLiteral("1px solid palette(mid)");
            const auto background = selected ? QStringLiteral("palette(alternate-base)")
                                             : QStringLiteral("palette(base)");
            const auto text = adjacent ? palette().color(QPalette::PlaceholderText)
                                       : palette().color(QPalette::Text);
            auto dayPalette = m_day->palette();
            dayPalette.setColor(QPalette::WindowText, text);
            m_day->setPalette(dayPalette);
            setStyleSheet(QStringLiteral("#calendarDayCell { border: %1; background: %2; }")
                              .arg(border, background));
        }

        void clearEvents()
        {
            while (m_layout->count() > 2)
            {
                auto* item = m_layout->takeAt(1);
                delete item->widget();
                delete item;
            }
            m_overflow = 0;
        }

        void addEvent(const MonthEvent& event, std::function<void()> activated)
        {
            auto* chip = new EventChip(event, this);
            QObject::connect(chip, &QToolButton::clicked, chip, std::move(activated));
            m_layout->insertWidget(m_layout->count() - 1, chip);
        }

        void addOverflow(const int count, std::function<void()> activated)
        {
            m_overflow = count;
            auto* button = new QToolButton(this);
            button->setText(QStringLiteral("+%1 more").arg(count));
            button->setAutoRaise(true);
            QObject::connect(button, &QToolButton::clicked, button, std::move(activated));
            m_layout->insertWidget(m_layout->count() - 1, button);
        }

        [[nodiscard]] int overflow() const
        {
            return m_overflow;
        }
        std::function<void(const QDate&)> clicked;

      protected:
        void mousePressEvent(QMouseEvent* event) override
        {
            if (event->button() == Qt::LeftButton && clicked)
                clicked(m_date);
            QWidget::mousePressEvent(event);
        }

      private:
        QDate m_date;
        QLabel* m_day = nullptr;
        QVBoxLayout* m_layout = nullptr;
        int m_overflow = 0;
    };

    MonthCalendarWidget::MonthCalendarWidget(QWidget* parent)
        : QWidget(parent), m_locale(QLocale{}), m_displayedMonth(QDate::currentDate()),
          m_selectedDate(QDate::currentDate())
    {
        setFocusPolicy(Qt::StrongFocus);
        auto* outer = new QVBoxLayout(this);
        m_title = new QLabel(this);
        m_title->setAlignment(Qt::AlignCenter);
        outer->addWidget(m_title);
        m_calendarMenu = new QMenu(this);
        const auto storedColors =
            QSettings{}.value(QStringLiteral("calendar/colorOverrides")).toMap();
        for (auto it = storedColors.cbegin(); it != storedColors.cend(); ++it)
        {
            const auto color = it.value().value<QColor>();
            if (color.isValid())
                m_customCalendarColors.emplace(it.key().toStdString(), color);
        }
        m_grid = new QGridLayout;
        m_grid->setSpacing(0);
        for (int column = 0; column < 7; ++column)
        {
            m_grid->setColumnStretch(column, 1);
            m_weekdayHeaders[static_cast<std::size_t>(column)] = new QLabel(this);
            m_weekdayHeaders[static_cast<std::size_t>(column)]->setAlignment(Qt::AlignCenter);
            m_weekdayHeaders[static_cast<std::size_t>(column)]->setSizePolicy(
                QSizePolicy::Ignored, QSizePolicy::Preferred);
            m_grid->addWidget(m_weekdayHeaders[static_cast<std::size_t>(column)], 0, column);
        }
        for (int index = 0; index < 42; ++index)
        {
            auto* cell = new DayCellWidget(this);
            cell->clicked = [this](const QDate& date) { selectDate(date, true); };
            m_cells[static_cast<std::size_t>(index)] = cell;
            m_grid->addWidget(cell, 1 + index / 7, index % 7);
        }
        outer->addLayout(m_grid, 1);
        connect(this, &MonthCalendarWidget::dayAgendaRequested, this,
                &MonthCalendarWidget::showDayAgenda);
        rebuildDates();
    }

    void MonthCalendarWidget::setLocale(const QLocale& locale)
    {
        m_locale = locale;
        rebuildDates();
    }

    void MonthCalendarWidget::setDisplayedMonth(const QDate& month)
    {
        if (!month.isValid())
            return;
        m_displayedMonth = QDate{month.year(), month.month(), 1};
        rebuildDates();
    }

    void MonthCalendarWidget::setEvents(std::vector<MonthEvent> events)
    {
        m_events = std::move(events);
        applyCalendarColors();
        rebuildEvents();
    }

    void MonthCalendarWidget::setCalendars(std::vector<CalendarDisplay> calendars)
    {
        const auto previousHiddenCalendars = m_hiddenCalendars;
        m_calendars = std::move(calendars);
        m_hiddenCalendars.clear();
        std::vector<std::string> knownCalendars;
        knownCalendars.reserve(m_calendars.size());
        for (const auto& calendar : m_calendars)
        {
            const auto wasHidden = std::ranges::find(previousHiddenCalendars, calendar.id) !=
                                   previousHiddenCalendars.end();
            const auto wasKnown =
                std::ranges::find(m_knownCalendars, calendar.id) != m_knownCalendars.end();
            const auto isVisible = wasKnown ? !wasHidden : calendar.visible;
            knownCalendars.push_back(calendar.id);
            if (!isVisible)
                m_hiddenCalendars.push_back(calendar.id);
        }
        m_knownCalendars = std::move(knownCalendars);
        applyCalendarColors();
        rebuildCalendarMenu();
        rebuildEvents();
    }

    void MonthCalendarWidget::setHiddenCalendars(std::vector<std::string> calendarIds)
    {
        m_hiddenCalendars = std::move(calendarIds);
        rebuildCalendarMenu();
        rebuildEvents();
    }

    QColor MonthCalendarWidget::effectiveCalendarColor(const std::string& calendarId) const
    {
        if (const auto custom = m_customCalendarColors.find(calendarId);
            custom != m_customCalendarColors.end())
            return custom->second;
        const auto calendar = std::ranges::find(m_calendars, calendarId, &CalendarDisplay::id);
        if (calendar != m_calendars.end() && calendar->color.isValid())
            return calendar->color;
        return palette().color(QPalette::Highlight);
    }

    void MonthCalendarWidget::applyCalendarColors()
    {
        for (auto& event : m_events)
            event.color = effectiveCalendarColor(event.calendarId);
    }

    void MonthCalendarWidget::rebuildCalendarMenu()
    {
        m_calendarMenu->clear();
        for (const auto& calendar : m_calendars)
        {
            auto* action = m_calendarMenu->addAction(
                colorSwatch(effectiveCalendarColor(calendar.id)), calendar.name);
            action->setCheckable(true);
            action->setChecked(std::ranges::find(m_hiddenCalendars, calendar.id) ==
                               m_hiddenCalendars.end());
            connect(action, &QAction::toggled, this,
                    [this, id = calendar.id](const bool visible)
                    {
                        if (visible)
                            std::erase(m_hiddenCalendars, id);
                        else if (std::ranges::find(m_hiddenCalendars, id) ==
                                 m_hiddenCalendars.end())
                            m_hiddenCalendars.push_back(id);
                        rebuildEvents();
                        Q_EMIT calendarVisibilityChanged(QString::fromStdString(id), visible);
                    });
        }
        auto* destinations = m_calendarMenu->addMenu(QStringLiteral("Default for New Events"));
        auto* destinationGroup = new QActionGroup(destinations);
        destinationGroup->setExclusive(true);
        for (const auto& calendar : m_calendars)
        {
            auto* action = destinations->addAction(calendar.name);
            action->setCheckable(true);
            action->setChecked(calendar.defaultDestination);
            action->setEnabled(calendar.writable);
            destinationGroup->addAction(action);
            connect(action, &QAction::triggered, this, [this, id = calendar.id]
                    { Q_EMIT defaultCalendarChanged(QString::fromStdString(id)); });
        }
        if (!m_calendars.empty())
            m_calendarMenu->addSeparator();
        auto* manage = m_calendarMenu->addAction(QIcon::fromTheme(QStringLiteral("configure")),
                                                 QStringLiteral("Manage Calendars…"));
        connect(manage, &QAction::triggered, this, &MonthCalendarWidget::manageCalendars);
    }

    void MonthCalendarWidget::manageCalendars()
    {
        QDialog dialog{this};
        dialog.setWindowTitle(QStringLiteral("Manage Calendars"));
        dialog.resize(480, 360);
        auto* layout = new QVBoxLayout(&dialog);
        auto* list = new QListWidget(&dialog);
        auto pendingColors = m_customCalendarColors;
        for (const auto& calendar : m_calendars)
        {
            auto* item = new QListWidgetItem(colorSwatch(effectiveCalendarColor(calendar.id)),
                                             calendar.name, list);
            item->setData(Qt::UserRole, QString::fromStdString(calendar.id));
        }
        layout->addWidget(list);
        auto* colorButtons = new QHBoxLayout();
        auto* chooseColor = new QPushButton(QStringLiteral("Choose Color…"), &dialog);
        auto* resetColor = new QPushButton(QStringLiteral("Use Calendar Color"), &dialog);
        chooseColor->setEnabled(false);
        resetColor->setEnabled(false);
        colorButtons->addWidget(chooseColor);
        colorButtons->addWidget(resetColor);
        colorButtons->addStretch(1);
        layout->addLayout(colorButtons);
        connect(list, &QListWidget::currentItemChanged, &dialog,
                [chooseColor, resetColor](QListWidgetItem* current, QListWidgetItem*)
                {
                    chooseColor->setEnabled(current != nullptr);
                    resetColor->setEnabled(current != nullptr);
                });
        connect(chooseColor, &QPushButton::clicked, &dialog,
                [this, list, &dialog, &pendingColors]
                {
                    auto* item = list->currentItem();
                    if (item == nullptr)
                        return;
                    const auto id = item->data(Qt::UserRole).toString().toStdString();
                    const auto color = QColorDialog::getColor(effectiveCalendarColor(id), &dialog,
                                                              QStringLiteral("Calendar Color"));
                    if (!color.isValid())
                        return;
                    pendingColors[id] = color;
                    item->setIcon(colorSwatch(color));
                });
        connect(resetColor, &QPushButton::clicked, &dialog,
                [this, list, &pendingColors]
                {
                    auto* item = list->currentItem();
                    if (item == nullptr)
                        return;
                    const auto id = item->data(Qt::UserRole).toString().toStdString();
                    pendingColors.erase(id);
                    const auto calendar = std::ranges::find(m_calendars, id, &CalendarDisplay::id);
                    const auto color = calendar != m_calendars.end() && calendar->color.isValid()
                                           ? calendar->color
                                           : palette().color(QPalette::Highlight);
                    item->setIcon(colorSwatch(color));
                });
        auto* buttons =
            new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);
        if (dialog.exec() != QDialog::Accepted)
            return;

        m_customCalendarColors = std::move(pendingColors);
        QVariantMap storedColors;
        for (const auto& [id, color] : m_customCalendarColors)
            storedColors.insert(QString::fromStdString(id), color);
        QSettings{}.setValue(QStringLiteral("calendar/colorOverrides"), storedColors);
        applyCalendarColors();
        rebuildCalendarMenu();
        rebuildEvents();
    }

    QDate MonthCalendarWidget::displayedMonth() const
    {
        return m_displayedMonth;
    }
    QDate MonthCalendarWidget::selectedDate() const
    {
        return m_selectedDate;
    }
    QDate MonthCalendarWidget::visibleStart() const
    {
        return cellDate(0);
    }
    QDate MonthCalendarWidget::visibleEnd() const
    {
        return cellDate(41).addDays(1);
    }
    QDate MonthCalendarWidget::cellDate(const int index) const
    {
        if (index < 0 || index >= 42)
            return {};
        return monthGridCellDate(m_displayedMonth, m_locale, index);
    }
    int MonthCalendarWidget::cellCount() const
    {
        return 42;
    }
    int MonthCalendarWidget::overflowCount(const QDate& date) const
    {
        for (int index = 0; index < 42; ++index)
            if (cellDate(index) == date)
                return m_cells[static_cast<std::size_t>(index)]->overflow();
        return 0;
    }

    QMenu* MonthCalendarWidget::calendarMenu() const
    {
        return m_calendarMenu;
    }

    void MonthCalendarWidget::showPreviousMonth()
    {
        setDisplayedMonth(m_displayedMonth.addMonths(-1));
    }
    void MonthCalendarWidget::createEvent()
    {
        Q_EMIT emptyTimeActivated(m_selectedDate);
    }
    void MonthCalendarWidget::showNextMonth()
    {
        setDisplayedMonth(m_displayedMonth.addMonths(1));
    }
    void MonthCalendarWidget::showToday()
    {
        m_selectedDate = QDate::currentDate();
        setDisplayedMonth(m_selectedDate);
    }

    void MonthCalendarWidget::keyPressEvent(QKeyEvent* event)
    {
        int days = 0;
        if (event->key() == Qt::Key_Left)
            days = -1;
        else if (event->key() == Qt::Key_Right)
            days = 1;
        else if (event->key() == Qt::Key_Up)
            days = -7;
        else if (event->key() == Qt::Key_Down)
            days = 7;
        else if (event->key() == Qt::Key_PageUp)
        {
            setDisplayedMonth(m_displayedMonth.addMonths(-1));
            return;
        }
        else if (event->key() == Qt::Key_PageDown)
        {
            setDisplayedMonth(m_displayedMonth.addMonths(1));
            return;
        }
        else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
        {
            Q_EMIT emptyTimeActivated(m_selectedDate);
            return;
        }
        else
        {
            QWidget::keyPressEvent(event);
            return;
        }
        selectDate(m_selectedDate.addDays(days), false);
    }

    void MonthCalendarWidget::rebuildDates()
    {
        m_title->setText(m_locale.toString(m_displayedMonth, QStringLiteral("MMMM yyyy")));
        const auto firstDay = static_cast<int>(m_locale.firstDayOfWeek());
        for (int column = 0; column < 7; ++column)
        {
            const auto day = ((firstDay - 1 + column) % 7) + 1;
            m_weekdayHeaders[static_cast<std::size_t>(column)]->setText(
                m_locale.dayName(day, QLocale::ShortFormat));
        }
        for (int index = 0; index < 42; ++index)
        {
            const auto date = cellDate(index);
            m_cells[static_cast<std::size_t>(index)]->setDate(
                date, date.month() != m_displayedMonth.month(), date == m_selectedDate);
        }
        rebuildEvents();
        Q_EMIT visibleIntervalChanged(visibleStart(), visibleEnd());
    }

    void MonthCalendarWidget::rebuildEvents()
    {
        for (auto* cell : m_cells)
            cell->clearEvents();
        for (int index = 0; index < 42; ++index)
        {
            const auto date = cellDate(index);
            std::vector<const MonthEvent*> matching;
            for (const auto& event : m_events)
            {
                if (std::ranges::find(m_hiddenCalendars, event.calendarId) !=
                    m_hiddenCalendars.end())
                    continue;
                if (event.start.date() <= date && eventLastDate(event) >= date)
                    matching.push_back(&event);
            }
            std::ranges::sort(matching,
                              [](const auto* left, const auto* right)
                              {
                                  if (left->allDay != right->allDay)
                                      return left->allDay;
                                  if (left->start != right->start)
                                      return left->start < right->start;
                                  return left->title.localeAwareCompare(right->title) < 0;
                              });
            constexpr std::size_t capacity = 3;
            const auto visible = std::min(capacity, matching.size());
            for (std::size_t eventIndex = 0; eventIndex < visible; ++eventIndex)
            {
                const auto* event = matching[eventIndex];
                m_cells[static_cast<std::size_t>(index)]->addEvent(
                    *event,
                    [this, event]
                    {
                        Q_EMIT eventActivated(
                            QString::fromStdString(event->accountId),
                            QString::fromStdString(event->eventId),
                            QString::fromStdString(event->recurrenceId.value_or(std::string{})));
                    });
            }
            if (matching.size() > capacity)
            {
                const auto overflow = static_cast<int>(matching.size() - capacity);
                m_cells[static_cast<std::size_t>(index)]->addOverflow(
                    overflow, [this, date] { Q_EMIT dayAgendaRequested(date); });
            }
        }
    }

    void MonthCalendarWidget::selectDate(const QDate& date, const bool activate)
    {
        if (!date.isValid())
            return;
        m_selectedDate = date;
        if (date < visibleStart() || date >= visibleEnd())
            m_displayedMonth = QDate{date.year(), date.month(), 1};
        rebuildDates();
        Q_EMIT selectionChanged(date);
        if (activate)
            Q_EMIT emptyTimeActivated(date);
    }

    void MonthCalendarWidget::showDayAgenda(const QDate& date)
    {
        auto* dialog = new QDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(m_locale.toString(date, QLocale::LongFormat));
        dialog->setModal(true);
        auto* layout = new QVBoxLayout(dialog);
        for (const auto& event : m_events)
        {
            if (std::ranges::find(m_hiddenCalendars, event.calendarId) != m_hiddenCalendars.end() ||
                event.start.date() > date || eventLastDate(event) < date)
                continue;
            auto* chip = new EventChip(event, dialog);
            connect(chip, &QToolButton::clicked, dialog,
                    [this, dialog, accountId = event.accountId, eventId = event.eventId,
                     recurrenceId = event.recurrenceId]
                    {
                        dialog->close();
                        Q_EMIT eventActivated(
                            QString::fromStdString(accountId), QString::fromStdString(eventId),
                            QString::fromStdString(recurrenceId.value_or(std::string{})));
                    });
            layout->addWidget(chip);
        }
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
        connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
        layout->addWidget(buttons);
        dialog->open();
    }
} // namespace javelin::gui::calendar
