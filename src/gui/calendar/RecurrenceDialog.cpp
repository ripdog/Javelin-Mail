#include "gui/calendar/RecurrenceDialog.h"

#include <KLocalizedString>

#include <QButtonGroup>
#include <QComboBox>
#include <QDateEdit>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <limits>

namespace
{
    using javelin::gui::calendar::FriendlyMonthlyMode;
    using javelin::gui::calendar::FriendlyRecurrenceEnd;
    using javelin::gui::calendar::FriendlyRecurrenceFrequency;

    QString ordinalName(const int ordinal)
    {
        switch (ordinal)
        {
        case 1:
            return i18nc("@item ordinal in a monthly recurrence", "first");
        case 2:
            return i18nc("@item ordinal in a monthly recurrence", "second");
        case 3:
            return i18nc("@item ordinal in a monthly recurrence", "third");
        case 4:
            return i18nc("@item ordinal in a monthly recurrence", "fourth");
        default:
            return i18nc("@item ordinal in a monthly recurrence", "last");
        }
    }
} // namespace

namespace javelin::gui::calendar
{
    RecurrenceDialog::RecurrenceDialog(QWidget* parent) : QDialog(parent)
    {
        setWindowTitle(i18n("Custom recurrence"));
        setModal(true);
        setMinimumWidth(420);

        auto* outer = new QVBoxLayout(this);
        outer->setSpacing(16);

        m_unsupported = new QLabel(this);
        m_unsupported->setWordWrap(true);
        m_unsupported->setText(
            i18n("This event uses an uncommon repeat pattern. Done replaces it with the pattern "
                 "below; Cancel leaves it unchanged."));
        m_unsupported->setStyleSheet(QStringLiteral("color: palette(link-visited);"));
        m_unsupported->hide();
        outer->addWidget(m_unsupported);

        auto* repeatRow = new QHBoxLayout;
        repeatRow->addWidget(new QLabel(i18n("Repeat every"), this));
        m_interval = new QSpinBox(this);
        m_interval->setObjectName(QStringLiteral("recurrenceInterval"));
        m_interval->setAccessibleName(i18n("Repeat interval"));
        m_interval->setRange(1, std::numeric_limits<int>::max());
        m_interval->setValue(1);
        repeatRow->addWidget(m_interval);
        m_frequency = new QComboBox(this);
        m_frequency->setObjectName(QStringLiteral("recurrenceFrequency"));
        m_frequency->setAccessibleName(i18n("Repeat interval unit"));
        m_frequency->addItem(i18nc("@item recurrence interval unit", "day"),
                             static_cast<int>(FriendlyRecurrenceFrequency::Day));
        m_frequency->addItem(i18nc("@item recurrence interval unit", "week"),
                             static_cast<int>(FriendlyRecurrenceFrequency::Week));
        m_frequency->addItem(i18nc("@item recurrence interval unit", "month"),
                             static_cast<int>(FriendlyRecurrenceFrequency::Month));
        m_frequency->addItem(i18nc("@item recurrence interval unit", "year"),
                             static_cast<int>(FriendlyRecurrenceFrequency::Year));
        repeatRow->addWidget(m_frequency);
        repeatRow->addStretch(1);
        outer->addLayout(repeatRow);

        m_weeklyControls = new QWidget(this);
        auto* weeklyLayout = new QVBoxLayout(m_weeklyControls);
        weeklyLayout->setContentsMargins(0, 0, 0, 0);
        weeklyLayout->addWidget(new QLabel(i18n("Repeat on"), m_weeklyControls));
        auto* dayButtons = new QHBoxLayout;
        const auto locale = QLocale{};
        const auto firstDay = locale.firstDayOfWeek();
        for (int offset = 0; offset < 7; ++offset)
        {
            const auto qtDay =
                static_cast<Qt::DayOfWeek>((static_cast<int>(firstDay) - 1 + offset) % 7 + 1);
            auto* button = new QToolButton(m_weeklyControls);
            button->setCheckable(true);
            button->setText(locale.standaloneDayName(qtDay, QLocale::NarrowFormat));
            button->setToolTip(locale.standaloneDayName(qtDay, QLocale::LongFormat));
            button->setAccessibleName(i18n("Repeat on %1", button->toolTip()));
            button->setFixedSize(32, 32);
            dayButtons->addWidget(button);
            m_weekdays.emplace_back(weekday(qtDay), button);
        }
        dayButtons->addStretch(1);
        weeklyLayout->addLayout(dayButtons);
        outer->addWidget(m_weeklyControls);

        m_monthlyMode = new QComboBox(this);
        m_monthlyMode->setObjectName(QStringLiteral("recurrenceMonthlyMode"));
        m_monthlyMode->setAccessibleName(i18n("Monthly repeat pattern"));
        outer->addWidget(m_monthlyMode);

        outer->addWidget(new QLabel(i18n("Ends"), this));
        auto* endGroup = new QButtonGroup(this);
        m_never = new QRadioButton(i18nc("@option:radio recurrence end", "Never"), this);
        m_onDate = new QRadioButton(i18nc("@option:radio recurrence end", "On"), this);
        m_afterCount = new QRadioButton(i18nc("@option:radio recurrence end", "After"), this);
        endGroup->addButton(m_never);
        endGroup->addButton(m_onDate);
        endGroup->addButton(m_afterCount);
        m_never->setChecked(true);
        outer->addWidget(m_never);
        auto* onRow = new QHBoxLayout;
        onRow->addWidget(m_onDate);
        m_untilDate = new QDateEdit(QDate::currentDate().addYears(1), this);
        m_untilDate->setObjectName(QStringLiteral("recurrenceUntilDate"));
        m_untilDate->setAccessibleName(i18n("Repeat until date"));
        m_untilDate->setCalendarPopup(true);
        m_untilDate->setDisplayFormat(QLocale{}.dateFormat(QLocale::ShortFormat));
        onRow->addWidget(m_untilDate);
        onRow->addStretch(1);
        outer->addLayout(onRow);
        auto* countRow = new QHBoxLayout;
        countRow->addWidget(m_afterCount);
        m_count = new QSpinBox(this);
        m_count->setObjectName(QStringLiteral("recurrenceCount"));
        m_count->setAccessibleName(i18n("Number of occurrences"));
        m_count->setRange(1, std::numeric_limits<int>::max());
        m_count->setValue(10);
        m_count->setSuffix(i18nc("@item recurrence count suffix", " occurrences"));
        countRow->addWidget(m_count);
        countRow->addStretch(1);
        outer->addLayout(countRow);

        m_error = new QLabel(this);
        m_error->setWordWrap(true);
        m_error->setStyleSheet(QStringLiteral("color: palette(link-visited);"));
        outer->addWidget(m_error);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
        auto* done =
            buttons->addButton(i18nc("@action:button", "Done"), QDialogButtonBox::AcceptRole);
        done->setDefault(true);
        outer->addWidget(buttons);
        connect(done, &QPushButton::clicked, this, &RecurrenceDialog::validateAndAccept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(m_frequency, &QComboBox::currentIndexChanged, this,
                &RecurrenceDialog::updateFrequencyControls);
        connect(m_never, &QRadioButton::toggled, this, &RecurrenceDialog::updateEndControls);
        connect(m_onDate, &QRadioButton::toggled, this, &RecurrenceDialog::updateEndControls);
        connect(m_afterCount, &QRadioButton::toggled, this, &RecurrenceDialog::updateEndControls);

        setEventStart(QDateTime::currentDateTime());
        updateFrequencyControls();
        updateEndControls();
    }

    void RecurrenceDialog::setEventStart(const QDateTime& start)
    {
        m_eventStart = start.isValid() ? start : QDateTime::currentDateTime();
        m_untilDate->setMinimumDate(m_eventStart.date());
        m_untilDate->setDate(m_eventStart.date().addYears(1));
        updateMonthlyChoices();
    }

    void RecurrenceDialog::setRule(const javelin::jmap::calendar::RecurrenceRule& value)
    {
        m_rule = value;
        const auto valuePattern = friendlyRecurrencePattern(value, m_eventStart);
        m_firstDayOfWeek = valuePattern.firstDayOfWeek;
        m_interval->setValue(static_cast<int>(std::min<std::uint32_t>(
            valuePattern.interval, static_cast<std::uint32_t>(std::numeric_limits<int>::max()))));
        m_frequency->setCurrentIndex(
            m_frequency->findData(static_cast<int>(valuePattern.frequency)));
        for (const auto& [day, button] : m_weekdays)
            button->setChecked(std::ranges::find(valuePattern.weekdays, day) !=
                               valuePattern.weekdays.end());
        m_monthlyMode->setCurrentIndex(
            m_monthlyMode->findData(static_cast<int>(valuePattern.monthlyMode)));
        switch (valuePattern.end)
        {
        case FriendlyRecurrenceEnd::Never:
            m_never->setChecked(true);
            break;
        case FriendlyRecurrenceEnd::OnDate:
            m_onDate->setChecked(true);
            if (valuePattern.untilDate)
                m_untilDate->setDate(*valuePattern.untilDate);
            break;
        case FriendlyRecurrenceEnd::AfterCount:
            m_afterCount->setChecked(true);
            m_count->setValue(static_cast<int>(std::min<std::uint32_t>(
                valuePattern.count, static_cast<std::uint32_t>(std::numeric_limits<int>::max()))));
            break;
        }
        m_unsupported->setVisible(valuePattern.replacesUnsupportedRule);
        updateFrequencyControls();
        updateEndControls();
    }

    FriendlyRecurrencePattern RecurrenceDialog::pattern() const
    {
        FriendlyRecurrencePattern result;
        result.frequency =
            static_cast<FriendlyRecurrenceFrequency>(m_frequency->currentData().toInt());
        result.interval = static_cast<std::uint32_t>(m_interval->value());
        result.firstDayOfWeek = m_firstDayOfWeek;
        for (const auto& [day, button] : m_weekdays)
            if (button->isChecked())
                result.weekdays.push_back(day);
        result.monthlyMode = static_cast<FriendlyMonthlyMode>(m_monthlyMode->currentData().toInt());
        if (m_onDate->isChecked())
        {
            result.end = FriendlyRecurrenceEnd::OnDate;
            result.untilDate = m_untilDate->date();
        }
        else if (m_afterCount->isChecked())
        {
            result.end = FriendlyRecurrenceEnd::AfterCount;
            result.count = static_cast<std::uint32_t>(m_count->value());
        }
        return result;
    }

    javelin::jmap::calendar::RecurrenceRule RecurrenceDialog::rule() const
    {
        return m_rule;
    }

    void RecurrenceDialog::validateAndAccept()
    {
        const auto value = pattern();
        if (value.frequency == FriendlyRecurrenceFrequency::Week && value.weekdays.empty())
        {
            m_error->setText(i18n("Choose at least one weekday."));
            return;
        }
        if (value.end == FriendlyRecurrenceEnd::OnDate &&
            (!value.untilDate || *value.untilDate < m_eventStart.date()))
        {
            m_error->setText(i18n("Choose an end date on or after the event date."));
            return;
        }
        m_rule = recurrenceRule(value, m_eventStart);
        m_error->clear();
        accept();
    }

    void RecurrenceDialog::updateFrequencyControls()
    {
        const auto frequency =
            static_cast<FriendlyRecurrenceFrequency>(m_frequency->currentData().toInt());
        m_weeklyControls->setVisible(frequency == FriendlyRecurrenceFrequency::Week);
        m_monthlyMode->setVisible(frequency == FriendlyRecurrenceFrequency::Month);
    }

    void RecurrenceDialog::updateEndControls()
    {
        m_untilDate->setEnabled(m_onDate->isChecked());
        m_count->setEnabled(m_afterCount->isChecked());
    }

    void RecurrenceDialog::updateMonthlyChoices()
    {
        const auto previous = m_monthlyMode->currentData();
        m_monthlyMode->clear();
        const auto date = m_eventStart.date();
        m_monthlyMode->addItem(i18n("Monthly on day %1", date.day()),
                               static_cast<int>(FriendlyMonthlyMode::DayOfMonth));
        const auto dayName = QLocale{}.standaloneDayName(
            static_cast<Qt::DayOfWeek>(date.dayOfWeek()), QLocale::LongFormat);
        m_monthlyMode->addItem(
            i18n("Monthly on the %1 %2", ordinalName(ordinalWeekday(date)), dayName),
            static_cast<int>(FriendlyMonthlyMode::OrdinalWeekday));
        const auto index = m_monthlyMode->findData(previous);
        m_monthlyMode->setCurrentIndex(index >= 0 ? index : 0);
    }
} // namespace javelin::gui::calendar
