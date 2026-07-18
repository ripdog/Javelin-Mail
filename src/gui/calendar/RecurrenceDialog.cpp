#include "gui/calendar/RecurrenceDialog.h"

#include <QComboBox>
#include <QDateTimeEdit>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>

namespace
{
    using javelin::jmap::calendar::RecurrenceFrequency;
    using javelin::jmap::calendar::RecurrenceSkip;
    using javelin::jmap::calendar::Weekday;

    template <typename T>
    std::optional<std::vector<T>> integerList(const QString& text, const qint64 minimum,
                                              const qint64 maximum, const bool allowZero,
                                              QString& error, const QString& field)
    {
        std::vector<T> result;
        const auto trimmed = text.trimmed();
        if (trimmed.isEmpty())
            return result;
        for (const auto& token : trimmed.split(QLatin1Char(','), Qt::KeepEmptyParts))
        {
            bool ok = false;
            const auto value = token.trimmed().toLongLong(&ok);
            if (!ok || value < minimum || value > maximum || (!allowZero && value == 0))
            {
                error = QStringLiteral("%1 contains an invalid value.").arg(field);
                return std::nullopt;
            }
            result.push_back(static_cast<T>(value));
        }
        return result;
    }

    QString joined(const auto& values)
    {
        QStringList result;
        for (const auto& value : values)
            if constexpr (std::is_same_v<std::remove_cvref_t<decltype(value)>, std::string>)
                result.push_back(QString::fromStdString(value));
            else
                result.push_back(QString::number(value));
        return result.join(QStringLiteral(", "));
    }

    void addWeekdays(QComboBox* combo, const bool includeDefault)
    {
        if (includeDefault)
            combo->addItem(QStringLiteral("Default (Monday)"), -1);
        combo->addItem(QStringLiteral("Monday"), static_cast<int>(Weekday::Monday));
        combo->addItem(QStringLiteral("Tuesday"), static_cast<int>(Weekday::Tuesday));
        combo->addItem(QStringLiteral("Wednesday"), static_cast<int>(Weekday::Wednesday));
        combo->addItem(QStringLiteral("Thursday"), static_cast<int>(Weekday::Thursday));
        combo->addItem(QStringLiteral("Friday"), static_cast<int>(Weekday::Friday));
        combo->addItem(QStringLiteral("Saturday"), static_cast<int>(Weekday::Saturday));
        combo->addItem(QStringLiteral("Sunday"), static_cast<int>(Weekday::Sunday));
    }
} // namespace

namespace javelin::gui::calendar
{
    RecurrenceDialog::RecurrenceDialog(QWidget* parent) : QDialog(parent)
    {
        setWindowTitle(QStringLiteral("Custom repeat pattern"));
        setModal(true);
        resize(560, 680);

        auto* outer = new QVBoxLayout(this);
        auto* scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        auto* content = new QWidget(scroll);
        auto* form = new QFormLayout(content);

        m_frequency = new QComboBox(content);
        m_frequency->setObjectName(QStringLiteral("recurrenceFrequency"));
        m_frequency->addItem(QStringLiteral("Yearly"),
                             static_cast<int>(RecurrenceFrequency::Yearly));
        m_frequency->addItem(QStringLiteral("Monthly"),
                             static_cast<int>(RecurrenceFrequency::Monthly));
        m_frequency->addItem(QStringLiteral("Weekly"),
                             static_cast<int>(RecurrenceFrequency::Weekly));
        m_frequency->addItem(QStringLiteral("Daily"), static_cast<int>(RecurrenceFrequency::Daily));
        m_frequency->addItem(QStringLiteral("Hourly"),
                             static_cast<int>(RecurrenceFrequency::Hourly));
        m_frequency->addItem(QStringLiteral("Minutely"),
                             static_cast<int>(RecurrenceFrequency::Minutely));
        m_frequency->addItem(QStringLiteral("Secondly"),
                             static_cast<int>(RecurrenceFrequency::Secondly));
        m_interval = new QSpinBox(content);
        m_interval->setObjectName(QStringLiteral("recurrenceInterval"));
        m_interval->setRange(1, std::numeric_limits<int>::max());
        m_interval->setValue(1);
        m_rscale = new QLineEdit(content);
        m_rscale->setObjectName(QStringLiteral("recurrenceScale"));
        m_rscale->setPlaceholderText(QStringLiteral("gregorian"));
        m_skip = new QComboBox(content);
        m_skip->setObjectName(QStringLiteral("recurrenceSkip"));
        m_skip->addItem(QStringLiteral("Default (omit)"), -1);
        m_skip->addItem(QStringLiteral("Omit invalid dates"),
                        static_cast<int>(RecurrenceSkip::Omit));
        m_skip->addItem(QStringLiteral("Move backward"),
                        static_cast<int>(RecurrenceSkip::Backward));
        m_skip->addItem(QStringLiteral("Move forward"), static_cast<int>(RecurrenceSkip::Forward));
        m_firstDay = new QComboBox(content);
        m_firstDay->setObjectName(QStringLiteral("recurrenceFirstDay"));
        addWeekdays(m_firstDay, true);

        auto* days = new QWidget(content);
        auto* daysLayout = new QVBoxLayout(days);
        daysLayout->setContentsMargins(0, 0, 0, 0);
        m_dayRowsWidget = new QWidget(days);
        m_dayRowsLayout = new QVBoxLayout(m_dayRowsWidget);
        m_dayRowsLayout->setContentsMargins(0, 0, 0, 0);
        daysLayout->addWidget(m_dayRowsWidget);
        auto* addDay = new QPushButton(QStringLiteral("+ Add weekday"), days);
        addDay->setObjectName(QStringLiteral("addRecurrenceDay"));
        daysLayout->addWidget(addDay, 0, Qt::AlignLeft);
        connect(addDay, &QPushButton::clicked, this, [this]() { addDayRow(); });

        const auto listField = [content](const QString& placeholder)
        {
            auto* edit = new QLineEdit(content);
            edit->setPlaceholderText(placeholder);
            return edit;
        };
        m_byMonthDay = listField(QStringLiteral("e.g. 1, 15, -1"));
        m_byMonthDay->setObjectName(QStringLiteral("recurrenceByMonthDay"));
        m_byMonth = listField(QStringLiteral("e.g. 1, 6, 12 or 3L"));
        m_byYearDay = listField(QStringLiteral("e.g. 1, 100, -1"));
        m_byWeekNo = listField(QStringLiteral("e.g. 1, 26, -1"));
        m_byHour = listField(QStringLiteral("0–23, comma separated"));
        m_byMinute = listField(QStringLiteral("0–59, comma separated"));
        m_bySecond = listField(QStringLiteral("0–60, comma separated"));
        m_bySetPosition = listField(QStringLiteral("e.g. 1, 2, -1"));

        m_endMode = new QComboBox(content);
        m_endMode->setObjectName(QStringLiteral("recurrenceEndMode"));
        m_endMode->addItem(QStringLiteral("Never"), QStringLiteral("never"));
        m_endMode->addItem(QStringLiteral("After a number of occurrences"),
                           QStringLiteral("count"));
        m_endMode->addItem(QStringLiteral("On or before a date and time"), QStringLiteral("until"));
        m_count = new QSpinBox(content);
        m_count->setObjectName(QStringLiteral("recurrenceCount"));
        m_count->setRange(1, std::numeric_limits<int>::max());
        m_until = new QDateTimeEdit(QDateTime::currentDateTime().addYears(1), content);
        m_until->setCalendarPopup(true);
        m_until->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss"));

        form->addRow(QStringLiteral("Frequency"), m_frequency);
        form->addRow(QStringLiteral("Every"), m_interval);
        form->addRow(QStringLiteral("Calendar scale"), m_rscale);
        form->addRow(QStringLiteral("Invalid dates"), m_skip);
        form->addRow(QStringLiteral("Week starts"), m_firstDay);
        form->addRow(QStringLiteral("Weekdays"), days);
        form->addRow(QStringLiteral("Days of month"), m_byMonthDay);
        form->addRow(QStringLiteral("Months"), m_byMonth);
        form->addRow(QStringLiteral("Days of year"), m_byYearDay);
        form->addRow(QStringLiteral("Weeks of year"), m_byWeekNo);
        form->addRow(QStringLiteral("Hours"), m_byHour);
        form->addRow(QStringLiteral("Minutes"), m_byMinute);
        form->addRow(QStringLiteral("Seconds"), m_bySecond);
        form->addRow(QStringLiteral("Positions in set"), m_bySetPosition);
        form->addRow(QStringLiteral("Ends"), m_endMode);
        form->addRow(QStringLiteral("Occurrence count"), m_count);
        form->addRow(QStringLiteral("Until"), m_until);
        scroll->setWidget(content);
        outer->addWidget(scroll);

        m_error = new QLabel(this);
        m_error->setWordWrap(true);
        m_error->setStyleSheet(QStringLiteral("color: palette(link-visited);"));
        outer->addWidget(m_error);
        auto* buttons =
            new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
        outer->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, &RecurrenceDialog::validateAndAccept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(m_endMode, &QComboBox::currentIndexChanged, this, &RecurrenceDialog::updateEndMode);
        updateEndMode();
    }

    void
    RecurrenceDialog::addDayRow(const std::optional<javelin::jmap::calendar::RecurrenceDay>& value)
    {
        DayRow row;
        row.container = new QWidget(m_dayRowsWidget);
        auto* layout = new QHBoxLayout(row.container);
        layout->setContentsMargins(0, 0, 0, 0);
        row.day = new QComboBox(row.container);
        addWeekdays(row.day, false);
        row.ordinal = new QSpinBox(row.container);
        row.ordinal->setRange(-366, 366);
        row.ordinal->setValue(0);
        row.ordinal->setToolTip(
            QStringLiteral("0 means every matching weekday; negative values count from the end."));
        row.remove = new QPushButton(QStringLiteral("−"), row.container);
        row.remove->setAccessibleName(QStringLiteral("Remove weekday"));
        row.remove->setFixedWidth(row.remove->sizeHint().height() + 8);
        layout->addWidget(row.day, 1);
        layout->addWidget(row.ordinal);
        layout->addWidget(row.remove);
        if (value)
        {
            row.day->setCurrentIndex(row.day->findData(static_cast<int>(value->day)));
            row.ordinal->setValue(value->nthOfPeriod.value_or(0));
        }
        auto* container = row.container;
        connect(row.remove, &QPushButton::clicked, this,
                [this, container]() { removeDayRow(container); });
        m_dayRowsLayout->addWidget(row.container);
        m_dayRows.push_back(row);
    }

    void RecurrenceDialog::removeDayRow(QWidget* row)
    {
        const auto found = std::ranges::find(m_dayRows, row, &DayRow::container);
        if (found == m_dayRows.end())
            return;
        found->container->deleteLater();
        m_dayRows.erase(found);
    }

    void RecurrenceDialog::clearDayRows()
    {
        for (const auto& row : m_dayRows)
            delete row.container;
        m_dayRows.clear();
    }

    void RecurrenceDialog::setRule(const javelin::jmap::calendar::RecurrenceRule& value)
    {
        m_rule = value;
        m_frequency->setCurrentIndex(m_frequency->findData(static_cast<int>(value.frequency)));
        m_interval->setValue(static_cast<int>(value.interval));
        m_rscale->setText(value.rscale ? QString::fromStdString(*value.rscale) : QString{});
        m_skip->setCurrentIndex(value.skip ? m_skip->findData(static_cast<int>(*value.skip)) : 0);
        m_firstDay->setCurrentIndex(
            value.firstDayOfWeek ? m_firstDay->findData(static_cast<int>(*value.firstDayOfWeek))
                                 : 0);
        clearDayRows();
        for (const auto& day : value.byDay)
            addDayRow(day);
        m_byMonthDay->setText(joined(value.byMonthDay));
        m_byMonth->setText(joined(value.byMonth));
        m_byYearDay->setText(joined(value.byYearDay));
        m_byWeekNo->setText(joined(value.byWeekNo));
        m_byHour->setText(joined(value.byHour));
        m_byMinute->setText(joined(value.byMinute));
        m_bySecond->setText(joined(value.bySecond));
        m_bySetPosition->setText(joined(value.bySetPosition));
        if (value.count)
        {
            m_endMode->setCurrentIndex(m_endMode->findData(QStringLiteral("count")));
            m_count->setValue(static_cast<int>(*value.count));
        }
        else if (value.until)
        {
            m_endMode->setCurrentIndex(m_endMode->findData(QStringLiteral("until")));
            const auto parsed =
                QDateTime::fromString(QString::fromStdString(value.until->value), Qt::ISODate);
            if (parsed.isValid())
                m_until->setDateTime(parsed);
        }
        else
        {
            m_endMode->setCurrentIndex(m_endMode->findData(QStringLiteral("never")));
        }
        updateEndMode();
    }

    std::optional<javelin::jmap::calendar::RecurrenceRule>
    RecurrenceDialog::validatedRule(QString& error) const
    {
        auto result = javelin::jmap::calendar::RecurrenceRule{};
        result.frequency = static_cast<RecurrenceFrequency>(m_frequency->currentData().toInt());
        result.interval = static_cast<std::uint32_t>(m_interval->value());
        result.rscale = m_rscale->text().trimmed().isEmpty()
                            ? std::nullopt
                            : std::optional{m_rscale->text().trimmed().toStdString()};
        result.skip =
            m_skip->currentData().toInt() < 0
                ? std::nullopt
                : std::optional{static_cast<RecurrenceSkip>(m_skip->currentData().toInt())};
        result.firstDayOfWeek =
            m_firstDay->currentData().toInt() < 0
                ? std::nullopt
                : std::optional{static_cast<Weekday>(m_firstDay->currentData().toInt())};
        for (const auto& row : m_dayRows)
            result.byDay.push_back({.day = static_cast<Weekday>(row.day->currentData().toInt()),
                                    .nthOfPeriod = row.ordinal->value() == 0
                                                       ? std::nullopt
                                                       : std::optional{row.ordinal->value()}});

        const auto monthDay = integerList<std::int32_t>(m_byMonthDay->text(), -31, 31, false, error,
                                                        QStringLiteral("Days of month"));
        const auto yearDay = integerList<std::int32_t>(m_byYearDay->text(), -366, 366, false, error,
                                                       QStringLiteral("Days of year"));
        const auto weekNo = integerList<std::int32_t>(m_byWeekNo->text(), -53, 53, false, error,
                                                      QStringLiteral("Weeks of year"));
        const auto hour = integerList<std::uint32_t>(m_byHour->text(), 0, 23, true, error,
                                                     QStringLiteral("Hours"));
        const auto minute = integerList<std::uint32_t>(m_byMinute->text(), 0, 59, true, error,
                                                       QStringLiteral("Minutes"));
        const auto second = integerList<std::uint32_t>(m_bySecond->text(), 0, 60, true, error,
                                                       QStringLiteral("Seconds"));
        const auto position = integerList<std::int32_t>(
            m_bySetPosition->text(), std::numeric_limits<std::int32_t>::min(),
            std::numeric_limits<std::int32_t>::max(), false, error,
            QStringLiteral("Positions in set"));
        if (!monthDay || !yearDay || !weekNo || !hour || !minute || !second || !position)
            return std::nullopt;
        result.byMonthDay = *monthDay;
        result.byYearDay = *yearDay;
        result.byWeekNo = *weekNo;
        result.byHour = *hour;
        result.byMinute = *minute;
        result.bySecond = *second;
        result.bySetPosition = *position;

        static const QRegularExpression monthPattern{QStringLiteral(R"(^[1-9][0-9]*L?$)")};
        const auto months = m_byMonth->text().trimmed();
        if (!months.isEmpty())
            for (const auto& token : months.split(QLatin1Char(','), Qt::KeepEmptyParts))
            {
                const auto month = token.trimmed();
                if (!monthPattern.match(month).hasMatch())
                {
                    error = QStringLiteral("Months contains an invalid value.");
                    return std::nullopt;
                }
                result.byMonth.push_back(month.toStdString());
            }

        const auto endMode = m_endMode->currentData().toString();
        if (endMode == QStringLiteral("count"))
            result.count = static_cast<std::uint32_t>(m_count->value());
        else if (endMode == QStringLiteral("until"))
            result.until = javelin::jmap::calendar::LocalDateTime{
                .value = m_until->dateTime().toString(Qt::ISODate).toStdString()};
        return result;
    }

    javelin::jmap::calendar::RecurrenceRule RecurrenceDialog::rule() const
    {
        return m_rule;
    }

    void RecurrenceDialog::validateAndAccept()
    {
        QString error;
        const auto value = validatedRule(error);
        if (!value)
        {
            m_error->setText(error);
            return;
        }
        m_rule = *value;
        m_error->clear();
        accept();
    }

    void RecurrenceDialog::updateEndMode()
    {
        const auto mode = m_endMode->currentData().toString();
        m_count->setVisible(mode == QStringLiteral("count"));
        m_until->setVisible(mode == QStringLiteral("until"));
    }
} // namespace javelin::gui::calendar
