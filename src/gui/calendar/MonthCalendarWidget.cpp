#include "gui/calendar/MonthCalendarWidget.h"
#include "gui/accessibility/AccessibleFactory.h"
#include "gui/calendar/CalendarEventButton.h"
#include "gui/calendar/MonthCalendarLayout.h"
#include "gui/settings/WorkspaceSettingsPort.h"

#include <KLocalizedString>

#include <QAccessible>
#include <QAccessibleWidget>
#include <QActionGroup>
#include <QApplication>
#include <QColorDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFocusEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace javelin::gui::calendar
{
    namespace
    {
        [[nodiscard]] QIcon colorSwatch(const QColor& color)
        {
            QPixmap swatch{12, 12};
            swatch.fill(color);
            return QIcon{swatch};
        }

        QDate eventLastDate(const MonthEvent& event)
        {
            return monthEventLastDate(event.start, event.end);
        }

        [[nodiscard]] QString eventAccessibleName(const MonthEvent& event, const QDate& cellDate)
        {
            QString name = event.title;
            if (!event.allDay && cellDate == event.start.date())
            {
                name.prepend(QLocale{}.toString(event.start.time(), QLocale::ShortFormat) +
                             QStringLiteral(" "));
            }

            QStringList states;
            if (cellDate > event.start.date())
                states.push_back(
                    i18nc("@info accessible calendar event", "continues from previous day"));
            if (cellDate < eventLastDate(event))
                states.push_back(i18nc("@info accessible calendar event", "continues to next day"));
            if (event.recurring)
                states.push_back(i18nc("@info accessible calendar event", "recurring"));
            if (!states.isEmpty())
            {
                name = i18nc("@item accessible calendar event with state", "%1, %2", name,
                             states.join(QStringLiteral(", ")));
            }
            return name;
        }

    } // namespace

    class DayCellWidget final : public QWidget
    {
      public:
        explicit DayCellWidget(const int gridIndex, QWidget* parent = nullptr)
            : QWidget(parent), m_gridIndex(gridIndex)
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

        void setDate(const QDate& date, const bool adjacent, const bool selected,
                     const QLocale& locale)
        {
            m_date = date;
            m_selected = selected;
            m_today = date == QDate::currentDate();
            const auto weekday = locale.dayName(date.dayOfWeek(), QLocale::LongFormat);
            m_accessibleDateLabel = adjacent
                                        ? i18nc("@item accessible adjacent-month calendar date",
                                                "%1 %2 %3", weekday, QString::number(date.day()),
                                                locale.monthName(date.month(), QLocale::LongFormat))
                                        : i18nc("@item accessible calendar date", "%1 %2", weekday,
                                                QString::number(date.day()));
            m_day->setText(QString::number(date.day()));
            const auto border = m_today ? QStringLiteral("2px solid palette(highlight)")
                                        : QStringLiteral("1px solid palette(mid)");
            const auto background = selected ? QStringLiteral("palette(alternate-base)")
                                             : QStringLiteral("palette(base)");
            const auto text = adjacent
                                  ? palette().color(QPalette::Active, QPalette::PlaceholderText)
                                  : palette().color(QPalette::Active, QPalette::Text);
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

        void setEventCount(const int count)
        {
            if (m_eventCount == count)
                return;
            m_eventCount = count;
            if (QAccessible::isActive())
            {
                QAccessibleEvent event{this, QAccessible::NameChanged};
                QAccessible::updateAccessibility(&event);
            }
        }

        void addEvent(const MonthEvent& event, const QDate& cellDate,
                      std::function<void()> activated)
        {
            const auto segment =
                monthEventSegment(event.title, event.start, event.end, event.allDay, cellDate);
            auto* chip = new CalendarEventButton(this);
            chip->setEventPresentation(
                segment.label + (event.recurring ? QStringLiteral(" ↻") : QString{}),
                eventAccessibleName(event, cellDate), event.color, segment.begins, segment.ends);
            chip->setToolTip(event.title);
            QObject::connect(chip, &QToolButton::clicked, chip, std::move(activated));
            m_layout->insertWidget(m_layout->count() - 1, chip);
        }

        void addOverflow(const int count, std::function<void()> activated)
        {
            m_overflow = count;
            auto* button = new QToolButton(this);
            button->setText(i18nc("@action:button additional calendar events", "+%1 more", count));
            button->setAccessibleName(i18ncp("@action:button accessible additional calendar events",
                                             "Show %1 more event", "Show %1 more events", count));
            button->setAutoRaise(true);
            QObject::connect(button, &QToolButton::clicked, button, std::move(activated));
            m_layout->insertWidget(m_layout->count() - 1, button);
        }

        [[nodiscard]] QString accessibleName() const
        {
            QStringList parts{m_accessibleDateLabel};
            if (m_today)
                parts.push_back(i18nc("@info accessible calendar date state", "Today"));
            if (m_eventCount > 0)
                parts.push_back(i18np("%1 event", "%1 events", m_eventCount));
            return parts.join(QStringLiteral(", "));
        }

        [[nodiscard]] QDate date() const
        {
            return m_date;
        }

        [[nodiscard]] int gridIndex() const
        {
            return m_gridIndex;
        }

        [[nodiscard]] bool selected() const
        {
            return m_selected;
        }

        [[nodiscard]] int overflow() const
        {
            return m_overflow;
        }

        [[nodiscard]] std::size_t visibleEventCount(const std::size_t eventCount) const
        {
            const auto margins = m_layout->contentsMargins();
            return monthCellVisibleEventCount(
                height(), m_day->sizeHint().height(), fontMetrics().height() + 6,
                margins.top() + margins.bottom(), m_layout->spacing(), eventCount);
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
        QString m_accessibleDateLabel;
        QLabel* m_day = nullptr;
        QVBoxLayout* m_layout = nullptr;
        int m_gridIndex = 0;
        int m_eventCount = 0;
        int m_overflow = 0;
        bool m_selected = false;
        bool m_today = false;
    };

    namespace
    {
        [[nodiscard]] QList<QToolButton*> directEventButtons(const DayCellWidget* cell)
        {
            return cell->findChildren<QToolButton*>(QString{}, Qt::FindDirectChildrenOnly);
        }

        class AccessibleCalendarHeader final : public QAccessibleWidget
        {
          public:
            explicit AccessibleCalendarHeader(QLabel* label)
                : QAccessibleWidget(label, QAccessible::ColumnHeader)
            {
            }
        };
    } // namespace

    class AccessibleMonthCalendar final : public QAccessibleWidget,
                                          public QAccessibleTableInterface,
                                          public QAccessibleSelectionInterface
    {
      public:
        explicit AccessibleMonthCalendar(MonthCalendarWidget* calendar)
            : QAccessibleWidget(calendar, QAccessible::Table)
        {
        }

        [[nodiscard]] QAccessibleInterface* childAt(const int x, const int y) const override
        {
            for (int index = 0; index < childCount(); ++index)
            {
                auto* candidate = child(index);
                if (candidate != nullptr && candidate->rect().contains(x, y))
                    return candidate;
            }
            return nullptr;
        }

        [[nodiscard]] QAccessibleInterface* focusChild() const override
        {
            auto* calendar = calendarWidget();
            if (calendar == nullptr)
                return nullptr;

            if (calendar->hasFocus())
            {
                if (auto* cell = calendar->cellForDate(calendar->m_selectedDate); cell != nullptr)
                    return QAccessible::queryAccessibleInterface(cell);
            }

            auto* focus = QApplication::focusWidget();
            for (auto* cell : calendar->m_cells)
            {
                if (focus == cell || (focus != nullptr && cell->isAncestorOf(focus)))
                    return QAccessible::queryAccessibleInterface(cell);
            }
            return nullptr;
        }

        [[nodiscard]] int childCount() const override
        {
            return 49;
        }

        [[nodiscard]] int indexOfChild(const QAccessibleInterface* candidate) const override
        {
            if (candidate == nullptr)
                return -1;
            auto* calendar = calendarWidget();
            if (calendar == nullptr)
                return -1;
            const auto* object = candidate->object();
            for (int column = 0; column < 7; ++column)
            {
                if (calendar->m_weekdayHeaders[static_cast<std::size_t>(column)] == object)
                    return column;
            }
            for (int index = 0; index < 42; ++index)
            {
                if (calendar->m_cells[static_cast<std::size_t>(index)] == object)
                    return 7 + index;
            }
            return -1;
        }

        [[nodiscard]] QAccessibleInterface* child(const int index) const override
        {
            auto* calendar = calendarWidget();
            if (calendar == nullptr)
                return nullptr;
            if (index >= 0 && index < 7)
            {
                return QAccessible::queryAccessibleInterface(
                    calendar->m_weekdayHeaders[static_cast<std::size_t>(index)]);
            }
            if (index >= 7 && index < 49)
            {
                return QAccessible::queryAccessibleInterface(
                    calendar->m_cells[static_cast<std::size_t>(index - 7)]);
            }
            return nullptr;
        }

        [[nodiscard]] QString text(const QAccessible::Text type) const override
        {
            if (type == QAccessible::Name)
            {
                auto* calendar = calendarWidget();
                return calendar != nullptr ? calendar->m_title->text() : QString{};
            }
            return QAccessibleWidget::text(type);
        }

        [[nodiscard]] void* interface_cast(const QAccessible::InterfaceType type) override
        {
            if (type == QAccessible::TableInterface)
                return static_cast<QAccessibleTableInterface*>(this);
            if (type == QAccessible::SelectionInterface)
                return static_cast<QAccessibleSelectionInterface*>(this);
            return QAccessibleWidget::interface_cast(type);
        }

        [[nodiscard]] QAccessibleInterface* caption() const override
        {
            return nullptr;
        }

        [[nodiscard]] QAccessibleInterface* summary() const override
        {
            return nullptr;
        }

        [[nodiscard]] QAccessibleInterface* cellAt(const int row, const int column) const override
        {
            if (row < 0 || row >= rowCount() || column < 0 || column >= columnCount())
                return nullptr;
            return child(7 + row * 7 + column);
        }

        [[nodiscard]] int selectedCellCount() const override
        {
            return static_cast<int>(selectedCells().size());
        }

        [[nodiscard]] QList<QAccessibleInterface*> selectedCells() const override
        {
            QList<QAccessibleInterface*> result;
            auto* calendar = calendarWidget();
            if (calendar == nullptr)
                return result;
            if (auto* cell = calendar->cellForDate(calendar->m_selectedDate); cell != nullptr)
                result.push_back(QAccessible::queryAccessibleInterface(cell));
            return result;
        }

        [[nodiscard]] QString columnDescription(const int column) const override
        {
            auto* calendar = calendarWidget();
            if (calendar == nullptr || column < 0 || column >= 7)
                return {};
            return calendar->m_weekdayHeaders[static_cast<std::size_t>(column)]->accessibleName();
        }

        [[nodiscard]] QString rowDescription(const int row) const override
        {
            Q_UNUSED(row);
            return {};
        }

        [[nodiscard]] int selectedColumnCount() const override
        {
            return 0;
        }

        [[nodiscard]] int selectedRowCount() const override
        {
            return 0;
        }

        [[nodiscard]] int columnCount() const override
        {
            return 7;
        }

        [[nodiscard]] int rowCount() const override
        {
            return 6;
        }

        [[nodiscard]] QList<int> selectedColumns() const override
        {
            return {};
        }

        [[nodiscard]] QList<int> selectedRows() const override
        {
            return {};
        }

        [[nodiscard]] bool isColumnSelected(const int column) const override
        {
            Q_UNUSED(column);
            return false;
        }

        [[nodiscard]] bool isRowSelected(const int row) const override
        {
            Q_UNUSED(row);
            return false;
        }

        bool selectRow(const int row) override
        {
            Q_UNUSED(row);
            return false;
        }

        bool selectColumn(const int column) override
        {
            Q_UNUSED(column);
            return false;
        }

        bool unselectRow(const int row) override
        {
            Q_UNUSED(row);
            return false;
        }

        bool unselectColumn(const int column) override
        {
            Q_UNUSED(column);
            return false;
        }

        void modelChange(QAccessibleTableModelChangeEvent* event) override
        {
            Q_UNUSED(event);
        }

        [[nodiscard]] int selectedItemCount() const override
        {
            return selectedCellCount();
        }

        [[nodiscard]] QList<QAccessibleInterface*> selectedItems() const override
        {
            return selectedCells();
        }

        [[nodiscard]] bool isSelected(QAccessibleInterface* childItem) const override
        {
            return childItem != nullptr && childItem->tableCellInterface() != nullptr &&
                   childItem->tableCellInterface()->table() == this &&
                   childItem->tableCellInterface()->isSelected();
        }

        bool select(QAccessibleInterface* childItem) override
        {
            auto* calendar = calendarWidget();
            if (calendar == nullptr || childItem == nullptr)
                return false;
            auto* cell = accessibility::namedObject<DayCellWidget, QWidget>(
                childItem->object(), QLatin1StringView{"calendarDayCell"});
            if (cell == nullptr)
                return false;
            calendar->selectDate(cell->date());
            return true;
        }

        bool unselect(QAccessibleInterface* childItem) override
        {
            Q_UNUSED(childItem);
            return false;
        }

        bool selectAll() override
        {
            return false;
        }

        bool clear() override
        {
            return false;
        }

      private:
        [[nodiscard]] MonthCalendarWidget* calendarWidget() const
        {
            return qobject_cast<MonthCalendarWidget*>(object());
        }
    };

    class AccessibleDayCell final : public QAccessibleWidget, public QAccessibleTableCellInterface
    {
      public:
        explicit AccessibleDayCell(DayCellWidget* cell) : QAccessibleWidget(cell, QAccessible::Cell)
        {
        }

        [[nodiscard]] QAccessibleInterface* childAt(const int x, const int y) const override
        {
            auto* cell = cellWidget();
            if (cell == nullptr)
                return nullptr;
            for (auto* button : directEventButtons(cell))
            {
                auto* candidate = QAccessible::queryAccessibleInterface(button);
                if (candidate != nullptr && candidate->rect().contains(x, y))
                    return candidate;
            }
            return nullptr;
        }

        [[nodiscard]] QAccessibleInterface* focusChild() const override
        {
            auto* cell = cellWidget();
            auto* focus = QApplication::focusWidget();
            if (cell == nullptr || focus == nullptr || !cell->isAncestorOf(focus))
                return nullptr;
            return QAccessible::queryAccessibleInterface(focus);
        }

        [[nodiscard]] int childCount() const override
        {
            auto* cell = cellWidget();
            return cell != nullptr ? static_cast<int>(directEventButtons(cell).size()) : 0;
        }

        [[nodiscard]] int indexOfChild(const QAccessibleInterface* candidate) const override
        {
            auto* cell = cellWidget();
            if (cell == nullptr || candidate == nullptr)
                return -1;
            const auto buttons = directEventButtons(cell);
            return static_cast<int>(
                buttons.indexOf(qobject_cast<QToolButton*>(candidate->object())));
        }

        [[nodiscard]] QAccessibleInterface* child(const int index) const override
        {
            auto* cell = cellWidget();
            if (cell == nullptr)
                return nullptr;
            const auto buttons = directEventButtons(cell);
            if (index < 0 || index >= buttons.size())
                return nullptr;
            return QAccessible::queryAccessibleInterface(buttons[index]);
        }

        [[nodiscard]] QString text(const QAccessible::Text type) const override
        {
            if (type == QAccessible::Name)
            {
                auto* cell = cellWidget();
                return cell != nullptr ? cell->accessibleName() : QString{};
            }
            return QAccessibleWidget::text(type);
        }

        [[nodiscard]] QAccessible::State state() const override
        {
            auto result = QAccessibleWidget::state();
            auto* cell = cellWidget();
            auto* calendar = calendarWidget();
            if (cell == nullptr || calendar == nullptr)
                return result;
            result.selectable = true;
            result.selected = cell->selected();
            result.focusable = true;
            result.focused = cell->selected() && calendar->hasFocus();
            result.readOnly = true;
            return result;
        }

        [[nodiscard]] void* interface_cast(const QAccessible::InterfaceType type) override
        {
            if (type == QAccessible::TableCellInterface)
                return static_cast<QAccessibleTableCellInterface*>(this);
            return QAccessibleWidget::interface_cast(type);
        }

        [[nodiscard]] QStringList actionNames() const override
        {
            return {QAccessibleActionInterface::setFocusAction()};
        }

        void doAction(const QString& actionName) override
        {
            if (actionName != QAccessibleActionInterface::setFocusAction())
                return;
            auto* cell = cellWidget();
            auto* calendar = calendarWidget();
            if (cell == nullptr || calendar == nullptr)
                return;
            calendar->selectDate(cell->date());
            calendar->setFocus(Qt::OtherFocusReason);
        }

        [[nodiscard]] QStringList keyBindingsForAction(const QString& actionName) const override
        {
            Q_UNUSED(actionName);
            return {};
        }

        [[nodiscard]] bool isSelected() const override
        {
            auto* cell = cellWidget();
            return cell != nullptr && cell->selected();
        }

        [[nodiscard]] QList<QAccessibleInterface*> columnHeaderCells() const override
        {
            QList<QAccessibleInterface*> result;
            auto* cell = cellWidget();
            auto* calendar = calendarWidget();
            if (cell == nullptr || calendar == nullptr)
                return result;
            result.push_back(QAccessible::queryAccessibleInterface(
                calendar->m_weekdayHeaders[static_cast<std::size_t>(columnIndex())]));
            return result;
        }

        [[nodiscard]] QList<QAccessibleInterface*> rowHeaderCells() const override
        {
            return {};
        }

        [[nodiscard]] int columnIndex() const override
        {
            auto* cell = cellWidget();
            return cell != nullptr ? cell->gridIndex() % 7 : -1;
        }

        [[nodiscard]] int rowIndex() const override
        {
            auto* cell = cellWidget();
            return cell != nullptr ? cell->gridIndex() / 7 : -1;
        }

        [[nodiscard]] int columnExtent() const override
        {
            return 1;
        }

        [[nodiscard]] int rowExtent() const override
        {
            return 1;
        }

        [[nodiscard]] QAccessibleInterface* table() const override
        {
            auto* calendar = calendarWidget();
            return calendar != nullptr ? QAccessible::queryAccessibleInterface(calendar) : nullptr;
        }

      private:
        [[nodiscard]] DayCellWidget* cellWidget() const
        {
            return accessibility::namedObject<DayCellWidget, QWidget>(
                object(), QLatin1StringView{"calendarDayCell"});
        }

        [[nodiscard]] MonthCalendarWidget* calendarWidget() const
        {
            auto* cell = cellWidget();
            return cell != nullptr ? qobject_cast<MonthCalendarWidget*>(cell->parentWidget())
                                   : nullptr;
        }
    };

    namespace
    {
        [[nodiscard]] QAccessibleInterface* calendarAccessibleFactory(const QString& key,
                                                                      QObject* object)
        {
            if (auto* calendar = accessibility::factoryObject<MonthCalendarWidget>(key, object);
                calendar != nullptr)
                return new AccessibleMonthCalendar(calendar);
            if (auto* cell = accessibility::namedFactoryObject<DayCellWidget, QWidget>(
                    key, object, QLatin1StringView{"calendarDayCell"});
                cell != nullptr)
                return new AccessibleDayCell(cell);
            if (auto* label = accessibility::namedFactoryObject<QLabel, QLabel>(
                    key, object, QLatin1StringView{"calendarWeekdayHeader"});
                label != nullptr)
                return new AccessibleCalendarHeader(label);
            return nullptr;
        }

        void ensureCalendarAccessibilityFactoryInstalled()
        {
            static const bool installed = []
            {
                QAccessible::installFactory(calendarAccessibleFactory);
                return true;
            }();
            Q_UNUSED(installed);
        }
    } // namespace

    MonthCalendarWidget::MonthCalendarWidget(
        javelin::gui::settings::WorkspaceSettingsPort& settings, QWidget* parent)
        : QWidget(parent), m_settings(settings), m_locale(QLocale{}),
          m_displayedMonth(QDate::currentDate()), m_selectedDate(QDate::currentDate())
    {
        ensureCalendarAccessibilityFactoryInstalled();
        setFocusPolicy(Qt::StrongFocus);
        auto* outer = new QVBoxLayout(this);
        m_title = new QLabel(this);
        m_title->setAlignment(Qt::AlignCenter);
        outer->addWidget(m_title);
        m_calendarMenu = new QMenu(this);
        reloadCalendarColors();
        static_cast<void>(m_settings.connectWorkspaceChanged(this,
                                                             [this]
                                                             {
                                                                 reloadCalendarColors();
                                                                 applyCalendarColors();
                                                                 rebuildCalendarMenu();
                                                                 rebuildEvents();
                                                             }));
        m_grid = new QGridLayout;
        m_grid->setSpacing(0);
        for (int column = 0; column < 7; ++column)
        {
            m_grid->setColumnStretch(column, 1);
            m_weekdayHeaders[static_cast<std::size_t>(column)] = new QLabel(this);
            m_weekdayHeaders[static_cast<std::size_t>(column)]->setObjectName(
                QStringLiteral("calendarWeekdayHeader"));
            m_weekdayHeaders[static_cast<std::size_t>(column)]->setAlignment(Qt::AlignCenter);
            m_weekdayHeaders[static_cast<std::size_t>(column)]->setSizePolicy(
                QSizePolicy::Ignored, QSizePolicy::Preferred);
            m_grid->addWidget(m_weekdayHeaders[static_cast<std::size_t>(column)], 0, column);
        }
        for (int index = 0; index < 42; ++index)
        {
            auto* cell = new DayCellWidget(index, this);
            cell->clicked = [this](const QDate& date)
            {
                selectDate(date);
                Q_EMIT dayAgendaRequested(date, {}, {}, {});
            };
            m_cells[static_cast<std::size_t>(index)] = cell;
            m_grid->addWidget(cell, 1 + index / 7, index % 7);
        }
        outer->addLayout(m_grid, 1);
        rebuildDates();
    }

    void MonthCalendarWidget::reloadCalendarColors()
    {
        m_customCalendarColors.clear();
        for (const auto& overrideValue : m_settings.workspaceSettings().calendarColorOverrides)
        {
            const QColor color{overrideValue.color};
            if (color.isValid())
                m_customCalendarColors.insert_or_assign(overrideValue.calendarId.toStdString(),
                                                        color);
        }
    }

    void MonthCalendarWidget::setLocale(const QLocale& locale)
    {
        m_locale = locale;
        rebuildDates();
        notifyAccessibilityGridChanged();
    }

    void MonthCalendarWidget::setDisplayedMonth(const QDate& month)
    {
        if (!month.isValid())
            return;

        const QDate targetMonth{month.year(), month.month(), 1};
        const bool monthChanged = targetMonth != m_displayedMonth;
        const auto previousSelection = m_selectedDate;
        m_displayedMonth = targetMonth;
        if (monthChanged && (m_selectedDate.year() != targetMonth.year() ||
                             m_selectedDate.month() != targetMonth.month()))
        {
            m_selectedDate = QDate{targetMonth.year(), targetMonth.month(),
                                   std::min(m_selectedDate.day(), targetMonth.daysInMonth())};
        }
        rebuildDates();
        if (monthChanged)
            notifyAccessibilityGridChanged();
        if (m_selectedDate != previousSelection)
            Q_EMIT selectionChanged(m_selectedDate);
    }

    void MonthCalendarWidget::setEvents(std::vector<MonthEvent> events)
    {
        m_events = std::move(events);
        applyCalendarColors();
        rebuildEvents();
    }

    void MonthCalendarWidget::setCalendars(std::vector<CalendarDisplay> calendars)
    {
        m_calendars = std::move(calendars);
        applyCalendarColors();
        rebuildCalendarMenu();
        rebuildEvents();
    }

    void MonthCalendarWidget::setCalendarAccounts(std::vector<CalendarAccountDisplay> accounts)
    {
        m_calendarAccounts = std::move(accounts);
    }

    void MonthCalendarWidget::applicationPaletteChanged()
    {
        for (int index = 0; index < 42; ++index)
        {
            const auto date = cellDate(index);
            m_cells[static_cast<std::size_t>(index)]->setDate(
                date, date.month() != m_displayedMonth.month(), date == m_selectedDate, m_locale);
        }
        applyCalendarColors();
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
        return palette().color(QPalette::Active, QPalette::Highlight);
    }

    void MonthCalendarWidget::applyCalendarColors()
    {
        for (auto& event : m_events)
            event.color = effectiveCalendarColor(event.calendarId);
    }

    void MonthCalendarWidget::rebuildCalendarMenu()
    {
        m_calendarMenu->clear();
        for (const auto& account : m_calendarAccounts)
        {
            m_calendarMenu->addSection(account.name);
            for (const auto& calendar : m_calendars)
            {
                if (calendar.accountId != account.id)
                    continue;
                auto* action = m_calendarMenu->addAction(
                    colorSwatch(effectiveCalendarColor(calendar.id)), calendar.name);
                action->setCheckable(true);
                action->setChecked(calendar.subscribed);
                connect(action, &QAction::toggled, this,
                        [this, action, id = calendar.id](const bool subscribed)
                        {
                            action->setEnabled(false);
                            Q_EMIT calendarSubscriptionChanged(QString::fromStdString(id),
                                                               subscribed);
                        });
            }
        }
        auto* destinations = m_calendarMenu->addMenu(i18n("Default for New Events"));
        auto* destinationGroup = new QActionGroup(destinations);
        destinationGroup->setExclusive(true);
        for (const auto& account : m_calendarAccounts)
        {
            bool accountAdded = false;
            for (const auto& calendar : m_calendars)
            {
                if (calendar.accountId != account.id || !calendar.subscribed)
                    continue;
                if (!accountAdded)
                {
                    destinations->addSection(account.name);
                    accountAdded = true;
                }
                auto* action = destinations->addAction(calendar.name);
                action->setCheckable(true);
                action->setChecked(calendar.defaultDestination);
                action->setEnabled(calendar.writable);
                destinationGroup->addAction(action);
                connect(action, &QAction::triggered, this, [this, id = calendar.id]
                        { Q_EMIT defaultCalendarChanged(QString::fromStdString(id)); });
            }
        }
        if (!m_calendars.empty())
            m_calendarMenu->addSeparator();
        auto* manage = m_calendarMenu->addAction(QIcon::fromTheme(QStringLiteral("configure")),
                                                 i18n("Manage Calendars…"));
        connect(manage, &QAction::triggered, this, &MonthCalendarWidget::manageCalendars);
    }

    void MonthCalendarWidget::manageCalendars()
    {
        QDialog dialog{this};
        dialog.setWindowTitle(i18n("Manage Calendars"));
        dialog.resize(480, 360);
        auto* layout = new QVBoxLayout(&dialog);
        auto* list = new QListWidget(&dialog);
        auto pendingColors = m_customCalendarColors;
        for (const auto& calendar : m_calendars)
        {
            const auto label = m_calendarAccounts.size() > 1
                                   ? i18nc("@item calendar and account", "%1 — %2", calendar.name,
                                           calendar.accountName)
                                   : calendar.name;
            auto* item =
                new QListWidgetItem(colorSwatch(effectiveCalendarColor(calendar.id)), label, list);
            item->setData(Qt::UserRole, QString::fromStdString(calendar.id));
            item->setData(Qt::UserRole + 1, calendar.deletable);
        }
        layout->addWidget(list);
        auto* colorButtons = new QHBoxLayout();
        auto* addCalendar = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")),
                                            i18nc("@action:button", "Add…"), &dialog);
        addCalendar->setEnabled(!m_calendarAccounts.empty());
        auto* deleteCalendar = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-delete")),
                                               i18nc("@action:button", "Delete"), &dialog);
        auto* chooseColor = new QPushButton(i18nc("@action:button", "Choose Color…"), &dialog);
        auto* resetColor = new QPushButton(i18nc("@action:button", "Use Calendar Color"), &dialog);
        deleteCalendar->setEnabled(false);
        chooseColor->setEnabled(false);
        resetColor->setEnabled(false);
        colorButtons->addWidget(addCalendar);
        colorButtons->addWidget(deleteCalendar);
        colorButtons->addSpacing(12);
        colorButtons->addWidget(chooseColor);
        colorButtons->addWidget(resetColor);
        colorButtons->addStretch(1);
        layout->addLayout(colorButtons);
        connect(
            list, &QListWidget::currentItemChanged, &dialog,
            [chooseColor, resetColor, deleteCalendar](QListWidgetItem* current, QListWidgetItem*)
            {
                chooseColor->setEnabled(current != nullptr);
                resetColor->setEnabled(current != nullptr);
                deleteCalendar->setEnabled(current != nullptr &&
                                           current->data(Qt::UserRole + 1).toBool());
            });
        connect(addCalendar, &QPushButton::clicked, &dialog,
                [this, &dialog]
                {
                    if (m_calendarAccounts.empty())
                        return;
                    bool accepted = false;
                    const auto name =
                        QInputDialog::getText(&dialog, i18n("Create Calendar"), i18n("Name:"),
                                              QLineEdit::Normal, {}, &accepted);
                    if (!accepted)
                        return;
                    if (name.trimmed().isEmpty())
                    {
                        QMessageBox::warning(&dialog, i18n("Create Calendar"),
                                             i18n("Enter a calendar name."));
                        return;
                    }
                    std::size_t accountIndex = 0;
                    if (m_calendarAccounts.size() > 1)
                    {
                        QStringList names;
                        for (const auto& account : m_calendarAccounts)
                            names.push_back(account.name);
                        const auto selected =
                            QInputDialog::getItem(&dialog, i18n("Create Calendar"),
                                                  i18n("Account:"), names, 0, false, &accepted);
                        if (!accepted)
                            return;
                        const auto found = std::ranges::find(names, selected);
                        accountIndex =
                            static_cast<std::size_t>(std::distance(names.begin(), found));
                    }
                    const auto color = QColorDialog::getColor(palette().color(QPalette::Highlight),
                                                              &dialog, i18n("Calendar Color"));
                    if (!color.isValid())
                        return;
                    Q_EMIT calendarCreationRequested(
                        QString::fromStdString(m_calendarAccounts[accountIndex].id), name.trimmed(),
                        color.name());
                    dialog.accept();
                });
        connect(deleteCalendar, &QPushButton::clicked, &dialog,
                [this, list, &dialog]
                {
                    auto* item = list->currentItem();
                    if (item == nullptr)
                        return;
                    QMessageBox prompt{
                        QMessageBox::Warning,
                        i18n("Delete Calendar"),
                        i18n("Delete “%1”? Events that belong only to this calendar will also be "
                             "deleted. This cannot be undone.",
                             item->text()),
                        QMessageBox::NoButton,
                        &dialog,
                    };
                    auto* deleteButton = prompt.addButton(i18nc("@action:button", "Delete"),
                                                          QMessageBox::DestructiveRole);
                    prompt.addButton(QMessageBox::Cancel);
                    prompt.exec();
                    if (prompt.clickedButton() != deleteButton)
                        return;
                    Q_EMIT calendarDeletionRequested(item->data(Qt::UserRole).toString());
                    dialog.accept();
                });
        connect(chooseColor, &QPushButton::clicked, &dialog,
                [this, list, &dialog, &pendingColors]
                {
                    auto* item = list->currentItem();
                    if (item == nullptr)
                        return;
                    const auto id = item->data(Qt::UserRole).toString().toStdString();
                    const auto color = QColorDialog::getColor(effectiveCalendarColor(id), &dialog,
                                                              i18n("Calendar Color"));
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

        auto workspace = m_settings.workspaceSettings();
        workspace.calendarColorOverrides.clear();
        workspace.calendarColorOverrides.reserve(pendingColors.size());
        for (const auto& [id, color] : pendingColors)
        {
            workspace.calendarColorOverrides.push_back(
                {.calendarId = QString::fromStdString(id), .color = color.name(QColor::HexRgb)});
        }
        if (const auto error = m_settings.updateWorkspace(std::move(workspace)))
        {
            QMessageBox::warning(
                this, i18n("Manage Calendars"),
                i18n("The calendar colours could not be saved.\n\n%1", error->detail));
            return;
        }
        m_customCalendarColors = std::move(pendingColors);
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

    std::vector<MonthEvent> MonthCalendarWidget::eventsForDate(const QDate& date) const
    {
        std::vector<MonthEvent> result;
        for (const auto& event : m_events)
        {
            if (event.start.date() <= date && eventLastDate(event) >= date)
                result.push_back(event);
        }
        return result;
    }

    QColor MonthCalendarWidget::calendarColor(const QString& calendarId) const
    {
        return effectiveCalendarColor(calendarId.toStdString());
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
        const auto previousSelection = m_selectedDate;
        const auto previousMonth = m_displayedMonth;
        m_selectedDate = QDate::currentDate();
        m_displayedMonth = QDate{m_selectedDate.year(), m_selectedDate.month(), 1};
        rebuildDates();
        if (m_displayedMonth != previousMonth)
            notifyAccessibilityGridChanged();
        else if (m_selectedDate != previousSelection)
            notifyAccessibilitySelectionChanged();
        if (m_selectedDate != previousSelection)
            Q_EMIT selectionChanged(m_selectedDate);
    }

    void MonthCalendarWidget::setSelectedDateFromAgenda(const QDate& date)
    {
        selectDate(date);
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
            showPreviousMonth();
            return;
        }
        else if (event->key() == Qt::Key_PageDown)
        {
            showNextMonth();
            return;
        }
        else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
        {
            Q_EMIT dayAgendaRequested(m_selectedDate, {}, {}, {});
            return;
        }
        else
        {
            QWidget::keyPressEvent(event);
            return;
        }
        selectDate(m_selectedDate.addDays(days));
    }

    void MonthCalendarWidget::focusInEvent(QFocusEvent* event)
    {
        QWidget::focusInEvent(event);
        if (!QAccessible::isActive())
            return;
        if (auto* cell = cellForDate(m_selectedDate); cell != nullptr)
        {
            QAccessibleEvent focusEvent{cell, QAccessible::Focus};
            QAccessible::updateAccessibility(&focusEvent);
        }
    }

    void MonthCalendarWidget::resizeEvent(QResizeEvent* event)
    {
        QWidget::resizeEvent(event);
        rebuildEvents();
        scheduleEventRebuild();
    }

    void MonthCalendarWidget::changeEvent(QEvent* event)
    {
        QWidget::changeEvent(event);
        if (event->type() == QEvent::FontChange || event->type() == QEvent::ApplicationFontChange ||
            event->type() == QEvent::PaletteChange ||
            event->type() == QEvent::ApplicationPaletteChange ||
            event->type() == QEvent::StyleChange)
            scheduleEventRebuild();
    }

    void MonthCalendarWidget::scheduleEventRebuild()
    {
        if (m_eventRebuildPending)
            return;
        m_eventRebuildPending = true;
        QTimer::singleShot(0, this,
                           [this]
                           {
                               m_eventRebuildPending = false;
                               rebuildEvents();
                           });
    }

    void MonthCalendarWidget::rebuildDates()
    {
        m_title->setText(m_locale.toString(m_displayedMonth, QStringLiteral("MMMM yyyy")));
        const auto firstDay = static_cast<int>(m_locale.firstDayOfWeek());
        for (int column = 0; column < 7; ++column)
        {
            const auto day = ((firstDay - 1 + column) % 7) + 1;
            auto* header = m_weekdayHeaders[static_cast<std::size_t>(column)];
            header->setText(m_locale.dayName(day, QLocale::ShortFormat));
            header->setAccessibleName(m_locale.dayName(day, QLocale::LongFormat));
        }
        for (int index = 0; index < 42; ++index)
        {
            const auto date = cellDate(index);
            m_cells[static_cast<std::size_t>(index)]->setDate(
                date, date.month() != m_displayedMonth.month(), date == m_selectedDate, m_locale);
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
                if (event.start.date() <= date && eventLastDate(event) >= date)
                    matching.push_back(&event);
            }
            m_cells[static_cast<std::size_t>(index)]->setEventCount(
                static_cast<int>(matching.size()));
            std::ranges::sort(matching,
                              [](const auto* left, const auto* right)
                              {
                                  if (left->allDay != right->allDay)
                                      return left->allDay;
                                  const auto leftMultiDay =
                                      eventLastDate(*left) > left->start.date();
                                  const auto rightMultiDay =
                                      eventLastDate(*right) > right->start.date();
                                  if (leftMultiDay != rightMultiDay)
                                      return leftMultiDay;
                                  if (left->start != right->start)
                                      return left->start < right->start;
                                  return left->title.localeAwareCompare(right->title) < 0;
                              });
            const auto visible =
                m_cells[static_cast<std::size_t>(index)]->visibleEventCount(matching.size());
            for (std::size_t eventIndex = 0; eventIndex < visible; ++eventIndex)
            {
                const auto* event = matching[eventIndex];
                m_cells[static_cast<std::size_t>(index)]->addEvent(
                    *event, date,
                    [this, event, date]
                    {
                        selectDate(date);
                        Q_EMIT dayAgendaRequested(
                            date, QString::fromStdString(event->accountId),
                            QString::fromStdString(event->eventId),
                            QString::fromStdString(event->recurrenceId.value_or(std::string{})));
                    });
            }
            if (matching.size() > visible)
            {
                const auto overflow = static_cast<int>(matching.size() - visible);
                m_cells[static_cast<std::size_t>(index)]->addOverflow(overflow,
                                                                      [this, date]
                                                                      {
                                                                          selectDate(date);
                                                                          Q_EMIT dayAgendaRequested(
                                                                              date, {}, {}, {});
                                                                      });
            }
        }
    }

    void MonthCalendarWidget::selectDate(const QDate& date)
    {
        if (!date.isValid() || date == m_selectedDate)
            return;

        const auto gridChanged = date < visibleStart() || date >= visibleEnd();
        m_selectedDate = date;
        if (gridChanged)
        {
            m_displayedMonth = QDate{date.year(), date.month(), 1};
            rebuildDates();
            notifyAccessibilityGridChanged();
        }
        else
        {
            for (int index = 0; index < 42; ++index)
            {
                const auto cellDate = this->cellDate(index);
                m_cells[static_cast<std::size_t>(index)]->setDate(
                    cellDate, cellDate.month() != m_displayedMonth.month(),
                    cellDate == m_selectedDate, m_locale);
            }
            notifyAccessibilitySelectionChanged();
        }
        Q_EMIT selectionChanged(date);
    }

    DayCellWidget* MonthCalendarWidget::cellForDate(const QDate& date) const
    {
        for (auto* cell : m_cells)
        {
            if (cell->date() == date)
                return cell;
        }
        return nullptr;
    }

    void MonthCalendarWidget::notifyAccessibilityGridChanged()
    {
        if (!QAccessible::isActive())
            return;

        QAccessibleEvent nameChanged{this, QAccessible::NameChanged};
        QAccessible::updateAccessibility(&nameChanged);
        QAccessibleTableModelChangeEvent modelChanged{this,
                                                      QAccessibleTableModelChangeEvent::ModelReset};
        QAccessible::updateAccessibility(&modelChanged);
        if (hasFocus())
        {
            if (auto* cell = cellForDate(m_selectedDate); cell != nullptr)
            {
                QAccessibleEvent focusChanged{cell, QAccessible::Focus};
                QAccessible::updateAccessibility(&focusChanged);
            }
        }
    }

    void MonthCalendarWidget::notifyAccessibilitySelectionChanged()
    {
        if (!QAccessible::isActive())
            return;

        QAccessibleEvent selectionChanged{this, QAccessible::SelectionWithin};
        QAccessible::updateAccessibility(&selectionChanged);
        if (hasFocus())
        {
            if (auto* cell = cellForDate(m_selectedDate); cell != nullptr)
            {
                QAccessibleEvent focusChanged{cell, QAccessible::Focus};
                QAccessible::updateAccessibility(&focusChanged);
            }
        }
    }

} // namespace javelin::gui::calendar
