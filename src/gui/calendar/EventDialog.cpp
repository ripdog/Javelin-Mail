#include "gui/calendar/EventDialog.h"
#include "gui/calendar/RecurrenceDialog.h"
#include "gui/widgets/EmailAddressLineEdit.h"

#include "jmap/calendar/CalendarEventEditing.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QDateTimeEdit>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTimeZone>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>

namespace
{
    QComboBox* createTimeEditor(QWidget* parent, const QString& accessibleName)
    {
        auto* editor = new QComboBox(parent);
        editor->setEditable(true);
        editor->setInsertPolicy(QComboBox::NoInsert);
        editor->setMaxVisibleItems(12);
        editor->setAccessibleName(accessibleName);
        editor->lineEdit()->setPlaceholderText(QStringLiteral("HH:mm"));
        for (auto time = QTime{0, 0}; time.isValid(); time = time.addSecs(15 * 60))
        {
            editor->addItem(time.toString(QStringLiteral("HH:mm")));
            if (time == QTime{23, 45})
                break;
        }
        return editor;
    }

    std::optional<QTime> enteredTime(const QComboBox* editor)
    {
        const auto text = editor->currentText().trimmed();
        auto time = QTime::fromString(text, QStringLiteral("HH:mm"));
        if (!time.isValid())
            time = QTime::fromString(text, QStringLiteral("H:mm"));
        return time.isValid() ? std::optional{time} : std::nullopt;
    }

    std::optional<QDateTime> enteredDateTime(const QDateEdit* date, const QComboBox* time)
    {
        const auto parsedTime = enteredTime(time);
        if (!date->date().isValid() || !parsedTime.has_value())
            return std::nullopt;
        return QDateTime{date->date(), *parsedTime};
    }

    void setEnteredDateTime(QDateEdit* date, QComboBox* time, const QDateTime& value)
    {
        date->setDate(value.date());
        time->setCurrentText(value.time().toString(QStringLiteral("HH:mm")));
    }

    std::optional<qint64> durationSeconds(const QString& value)
    {
        static const QRegularExpression expression{QStringLiteral(
            "^P(?:(\\d+)W)?(?:(\\d+)D)?(?:T(?:(\\d+)H)?(?:(\\d+)M)?(?:(\\d+)S)?)?$")};
        const auto match = expression.match(value);
        if (!match.hasMatch())
            return std::nullopt;
        const auto number = [&match](const int capture)
        { return match.captured(capture).isEmpty() ? 0LL : match.captured(capture).toLongLong(); };
        const auto seconds = number(1) * 7 * 24 * 60 * 60 + number(2) * 24 * 60 * 60 +
                             number(3) * 60 * 60 + number(4) * 60 + number(5);
        return seconds > 0 ? std::optional{seconds} : std::nullopt;
    }

    std::optional<qint64> signedDurationSeconds(const QString& value)
    {
        static const QRegularExpression expression{QStringLiteral(
            "^([+-])?P(?:(\\d+)W)?(?:(\\d+)D)?(?:T(?:(\\d+)H)?(?:(\\d+)M)?(?:(\\d+)S)?)?$")};
        const auto match = expression.match(value);
        if (!match.hasMatch())
            return std::nullopt;
        const auto number = [&match](const int capture)
        { return match.captured(capture).isEmpty() ? 0LL : match.captured(capture).toLongLong(); };
        const auto seconds = number(2) * 7 * 24 * 60 * 60 + number(3) * 24 * 60 * 60 +
                             number(4) * 60 * 60 + number(5) * 60 + number(6);
        return match.captured(1) == QStringLiteral("-") ? -seconds : seconds;
    }

    QString calendarKey(const std::string& accountId, const std::string& calendarId)
    {
        return QString::fromStdString(accountId) + QLatin1Char('\n') +
               QString::fromStdString(calendarId);
    }

    struct EditableDuration
    {
        int amount = 10;
        int unitSeconds = 60;
        bool negative = true;
    };

    EditableDuration editableDuration(const std::optional<javelin::jmap::calendar::Duration>& value)
    {
        if (!value)
            return {};
        const auto parsed = signedDurationSeconds(QString::fromStdString(value->value));
        if (!parsed)
            return {};
        const auto absolute = std::abs(*parsed);
        for (const int unit : {7 * 86400, 86400, 3600, 60, 1})
            if (absolute % unit == 0 && absolute / unit <= 10000)
                return {.amount = static_cast<int>(absolute / unit),
                        .unitSeconds = unit,
                        .negative = *parsed < 0};
        return {.amount = static_cast<int>(std::min<qint64>(absolute, 10000)),
                .unitSeconds = 1,
                .negative = *parsed < 0};
    }

    std::string signedDuration(const int amount, const int unitSeconds, const bool negative)
    {
        const auto prefix = negative && amount > 0 ? QStringLiteral("-") : QString{};
        const auto value = unitSeconds == 7 * 86400
                               ? QStringLiteral("%1P%2W").arg(prefix).arg(amount)
                           : unitSeconds == 86400 ? QStringLiteral("%1P%2D").arg(prefix).arg(amount)
                           : unitSeconds == 3600 ? QStringLiteral("%1PT%2H").arg(prefix).arg(amount)
                           : unitSeconds == 60 ? QStringLiteral("%1PT%2M").arg(prefix).arg(amount)
                                               : QStringLiteral("%1PT%2S").arg(prefix).arg(amount);
        return value.toStdString();
    }
} // namespace

namespace javelin::gui::calendar
{
    EventDialog::EventDialog(std::vector<javelin::jmap::calendar::Calendar> calendars,
                             QWidget* parent)
        : QDialog(parent), m_calendars(std::move(calendars))
    {
        setWindowTitle(QStringLiteral("Calendar event"));
        setModal(true);
        auto* layout = new QFormLayout(this);
        m_title = new QLineEdit(this);
        m_calendar = new QComboBox(this);
        for (const auto& calendar : m_calendars)
        {
            const auto writable = calendar.myRights.mayWriteAll || calendar.myRights.mayWriteOwn;
            m_calendar->addItem(
                writable
                    ? QString::fromStdString(calendar.name)
                    : QStringLiteral("%1 (read-only)").arg(QString::fromStdString(calendar.name)),
                calendarKey(calendar.accountId, calendar.id));
            if (!writable)
            {
                if (auto* model = qobject_cast<QStandardItemModel*>(m_calendar->model()))
                    model->item(m_calendar->count() - 1)->setEnabled(false);
            }
        }
        m_allDay = new QCheckBox(QStringLiteral("All day"), this);
        auto* startRow = new QWidget(this);
        auto* startLayout = new QHBoxLayout(startRow);
        startLayout->setContentsMargins(0, 0, 0, 0);
        m_startDate = new QDateEdit(startRow);
        m_startDate->setCalendarPopup(true);
        m_startTime = createTimeEditor(startRow, QStringLiteral("Start time"));
        startLayout->addWidget(m_startDate, 1);
        startLayout->addWidget(m_startTime);
        auto* endRow = new QWidget(this);
        auto* endLayout = new QHBoxLayout(endRow);
        endLayout->setContentsMargins(0, 0, 0, 0);
        m_endDate = new QDateEdit(endRow);
        m_endDate->setCalendarPopup(true);
        m_endTime = createTimeEditor(endRow, QStringLiteral("End time"));
        endLayout->addWidget(m_endDate, 1);
        endLayout->addWidget(m_endTime);
        const auto now = QDateTime::currentDateTime();
        setEnteredDateTime(m_startDate, m_startTime, now);
        setEnteredDateTime(m_endDate, m_endTime, now.addSecs(3600));
        m_timeZone = new QComboBox(this);
        const auto zones = QTimeZone::availableTimeZoneIds();
        for (const auto& zone : zones)
            m_timeZone->addItem(QString::fromUtf8(zone));
        m_timeZone->setCurrentText(QString::fromUtf8(QTimeZone::systemTimeZoneId()));
        m_description = new QPlainTextEdit(this);
        m_location = new QLineEdit(this);
        m_recurrence = new QComboBox(this);
        m_recurrence->addItem(QStringLiteral("Does not repeat"), QStringLiteral("none"));
        m_recurrence->addItem(QStringLiteral("Daily"), QStringLiteral("daily"));
        m_recurrence->addItem(QStringLiteral("Weekly"), QStringLiteral("weekly"));
        m_recurrence->addItem(QStringLiteral("Monthly"), QStringLiteral("monthly"));
        m_recurrence->addItem(QStringLiteral("Yearly"), QStringLiteral("yearly"));
        m_recurrence->addItem(QStringLiteral("Custom…"), QStringLiteral("custom"));
        auto* recurrenceRow = new QWidget(this);
        auto* recurrenceLayout = new QHBoxLayout(recurrenceRow);
        recurrenceLayout->setContentsMargins(0, 0, 0, 0);
        m_customizeRecurrence = new QPushButton(QStringLiteral("Customize…"), recurrenceRow);
        recurrenceLayout->addWidget(m_recurrence, 1);
        recurrenceLayout->addWidget(m_customizeRecurrence);

        m_attendees = new QWidget(this);
        m_attendeeRowsLayout = new QVBoxLayout(m_attendees);
        m_attendeeRowsLayout->setContentsMargins(0, 0, 0, 0);
        auto* addAttendee = new QPushButton(QStringLiteral("+ Add attendee"), m_attendees);
        addAttendee->setObjectName(QStringLiteral("addAttendee"));
        m_attendeeRowsLayout->addWidget(addAttendee, 0, Qt::AlignLeft);
        connect(addAttendee, &QPushButton::clicked, this,
                [this]()
                {
                    addAttendeeRow();
                    m_attendeesEdited = true;
                });
        addAttendeeRow();
        m_error = new QLabel(this);
        m_error->setStyleSheet(QStringLiteral("color: palette(link-visited);"));
        m_error->setWordWrap(true);
        layout->addRow(QStringLiteral("Title"), m_title);
        layout->addRow(QStringLiteral("Calendar"), m_calendar);
        layout->addRow(QString{}, m_allDay);
        layout->addRow(QStringLiteral("Start"), startRow);
        layout->addRow(QStringLiteral("End"), endRow);
        layout->addRow(QStringLiteral("Time zone"), m_timeZone);
        layout->addRow(QStringLiteral("Location"), m_location);
        layout->addRow(QStringLiteral("Description"), m_description);
        layout->addRow(QStringLiteral("Recurrence"), recurrenceRow);
        layout->addRow(QStringLiteral("Attendees"), m_attendees);
        layout->addRow(m_error);
        auto* buttons =
            new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
        m_delete = buttons->addButton(QStringLiteral("Delete"), QDialogButtonBox::DestructiveRole);
        m_delete->setVisible(false);
        layout->addRow(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, &EventDialog::validateAndAccept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(m_delete, &QPushButton::clicked, this,
                [this]() { done(EventDialog::DeleteRequested); });
        connect(m_allDay, &QCheckBox::toggled, this, &EventDialog::updateAllDayMode);
        connect(m_startDate, &QDateEdit::editingFinished, this, &EventDialog::updateAutomaticEnd);
        connect(m_startTime->lineEdit(), &QLineEdit::editingFinished, this,
                &EventDialog::updateAutomaticEnd);
        connect(m_startTime, &QComboBox::activated, this, &EventDialog::updateAutomaticEnd);
        connect(m_endDate, &QDateEdit::editingFinished, this, &EventDialog::markEndEdited);
        connect(m_endTime->lineEdit(), &QLineEdit::editingFinished, this,
                &EventDialog::markEndEdited);
        connect(m_endTime, &QComboBox::activated, this, &EventDialog::markEndEdited);
        connect(m_calendar, &QComboBox::activated, this, [this]() { m_calendarEdited = true; });
        connect(m_timeZone, &QComboBox::activated, this, [this]() { m_timeZoneEdited = true; });
        connect(m_recurrence, &QComboBox::activated, this,
                [this]()
                {
                    m_recurrenceEdited = true;
                    updateRecurrenceControls();
                    if (m_recurrence->currentData().toString() == QStringLiteral("custom"))
                        editCustomRecurrence();
                    else
                        m_committedRecurrenceKey = m_recurrence->currentData().toString();
                });
        connect(m_customizeRecurrence, &QPushButton::clicked, this,
                &EventDialog::editCustomRecurrence);

        m_alerts = new QWidget(this);
        m_alertRowsLayout = new QVBoxLayout(m_alerts);
        m_alertRowsLayout->setContentsMargins(0, 0, 0, 0);
        m_useDefaultAlerts =
            new QCheckBox(QStringLiteral("Use calendar default notifications"), m_alerts);
        m_alertRowsLayout->addWidget(m_useDefaultAlerts);
        m_addAlert = new QPushButton(QStringLiteral("+ Add notification"), m_alerts);
        m_addAlert->setObjectName(QStringLiteral("addNotification"));
        m_alertRowsLayout->addWidget(m_addAlert, 0, Qt::AlignLeft);
        connect(m_addAlert, &QPushButton::clicked, this,
                [this]()
                {
                    addAlertRow();
                    m_alertsEdited = true;
                });
        connect(m_useDefaultAlerts, &QCheckBox::toggled, this,
                [this](const bool checked)
                {
                    m_alertsEdited = true;
                    m_addAlert->setEnabled(!checked);
                    for (auto& row : m_alertRows)
                        row.container->setEnabled(!checked);
                });

        layout->insertRow(layout->rowCount() - 2, QStringLiteral("Notifications"), m_alerts);
        updateRecurrenceControls();
    }

    void EventDialog::setEvent(const javelin::jmap::calendar::CalendarEvent& event)
    {
        m_event = event;
        m_delete->setVisible(!event.id.empty());
        m_title->setText(QString::fromStdString(event.title));
        for (const auto& [calendarId, present] : event.calendarIds)
            if (present)
            {
                const auto index = m_calendar->findData(calendarKey(event.accountId, calendarId));
                if (index >= 0)
                {
                    m_calendar->setCurrentIndex(index);
                    break;
                }
            }
        m_allDay->setChecked(event.showWithoutTime);
        const auto start =
            QDateTime::fromString(QString::fromStdString(event.start.value), Qt::ISODate);
        if (start.isValid())
        {
            setEnteredDateTime(m_startDate, m_startTime, start);
            const auto seconds = durationSeconds(QString::fromStdString(event.duration.value));
            setEnteredDateTime(
                m_endDate, m_endTime,
                start.addSecs(seconds.value_or(event.showWithoutTime ? 86400 : 3600)));
        }
        m_endEdited = false;
        if (event.timeZone)
            m_timeZone->setCurrentText(QString::fromStdString(event.timeZone->value));
        m_description->setPlainText(event.description ? QString::fromStdString(*event.description)
                                                      : QString{});
        m_location->setText(event.location ? QString::fromStdString(*event.location) : QString{});

        auto recurrenceKey = QStringLiteral("none");
        if (event.recurrenceRule)
        {
            const auto& rule = *event.recurrenceRule;
            const bool isSimple =
                rule.interval == 1 && !rule.rscale.has_value() && !rule.skip.has_value() &&
                !rule.firstDayOfWeek.has_value() && rule.byDay.empty() && rule.byMonthDay.empty() &&
                rule.byMonth.empty() && rule.byYearDay.empty() && rule.byWeekNo.empty() &&
                rule.byHour.empty() && rule.byMinute.empty() && rule.bySecond.empty() &&
                rule.bySetPosition.empty() && !rule.count.has_value() && !rule.until.has_value();
            if (isSimple)
            {
                switch (rule.frequency)
                {
                case javelin::jmap::calendar::RecurrenceFrequency::Daily:
                    recurrenceKey = QStringLiteral("daily");
                    break;
                case javelin::jmap::calendar::RecurrenceFrequency::Weekly:
                    recurrenceKey = QStringLiteral("weekly");
                    break;
                case javelin::jmap::calendar::RecurrenceFrequency::Monthly:
                    recurrenceKey = QStringLiteral("monthly");
                    break;
                case javelin::jmap::calendar::RecurrenceFrequency::Yearly:
                    recurrenceKey = QStringLiteral("yearly");
                    break;
                default:
                    recurrenceKey = QStringLiteral("custom");
                    break;
                }
            }
            else
            {
                recurrenceKey = QStringLiteral("custom");
            }
        }
        m_recurrence->setCurrentIndex(m_recurrence->findData(recurrenceKey));
        m_customRecurrence = event.recurrenceRule;
        m_committedRecurrenceKey = recurrenceKey;
        updateRecurrenceControls();

        clearAttendeeRows();
        for (const auto& address :
             javelin::jmap::calendar::editableAttendeeAddresses(event.attendees))
            addAttendeeRow(QString::fromStdString(address));
        if (m_attendeeRows.empty())
            addAttendeeRow();

        clearAlertRows();
        for (const auto& [id, alert] : event.alerts)
        {
            Q_UNUSED(id);
            if (alert.action == "display")
                addAlertRow(alert);
        }
        m_useDefaultAlerts->setChecked(event.useDefaultAlerts);
        m_addAlert->setEnabled(!event.useDefaultAlerts);
        for (auto& row : m_alertRows)
            row.container->setEnabled(!event.useDefaultAlerts);

        m_calendarEdited = false;
        m_timeZoneEdited = false;
        m_recurrenceEdited = false;
        m_attendeesEdited = false;
        m_alertsEdited = false;
    }

    void EventDialog::setOccurrenceMode(const bool occurrenceMode)
    {
        setWindowTitle(occurrenceMode ? QStringLiteral("Edit occurrence")
                                      : QStringLiteral("Calendar event"));
        m_calendar->setEnabled(!occurrenceMode);
        m_allDay->setEnabled(!occurrenceMode);
        m_timeZone->setEnabled(!occurrenceMode && !m_allDay->isChecked());
        m_description->setEnabled(!occurrenceMode);
        m_location->setEnabled(!occurrenceMode);
        m_recurrence->setEnabled(!occurrenceMode);
        m_customizeRecurrence->setEnabled(
            !occurrenceMode && m_recurrence->currentData().toString() != QStringLiteral("none"));
        m_attendees->setEnabled(!occurrenceMode);
        m_alerts->setEnabled(!occurrenceMode);
    }

    javelin::jmap::calendar::CalendarEvent EventDialog::eventDocument() const
    {
        auto result = m_event;
        result.title = m_title->text().trimmed().toStdString();
        if (m_calendarEdited || result.calendarIds.empty())
        {
            const auto key = m_calendar->currentData().toString();
            const auto separator = key.indexOf(QLatin1Char('\n'));
            if (separator > 0 && separator < key.size() - 1)
            {
                result.accountId = key.first(separator).toStdString();
                result.calendarIds.clear();
                result.calendarIds.emplace(key.sliced(separator + 1).toStdString(), true);
            }
        }
        result.showWithoutTime = m_allDay->isChecked();
        const auto start = enteredDateTime(m_startDate, m_startTime).value();
        const auto end = enteredDateTime(m_endDate, m_endTime).value();
        result.start.value = start.toString(Qt::ISODate).toStdString();
        result.duration.value =
            (result.showWithoutTime ? QStringLiteral("P%1D").arg(start.date().daysTo(end.date()))
                                    : QStringLiteral("PT%1S").arg(start.secsTo(end)))
                .toStdString();
        if (m_timeZoneEdited || (result.id.empty() && !result.timeZone))
            result.timeZone = javelin::jmap::calendar::TimeZoneId{
                .value = m_timeZone->currentText().toStdString()};
        const auto description = m_description->toPlainText().trimmed();
        result.description =
            description.isEmpty() ? std::nullopt : std::optional{description.toStdString()};
        const auto location = m_location->text().trimmed();
        result.location = location.isEmpty() ? std::nullopt : std::optional{location.toStdString()};
        if (m_recurrenceEdited)
        {
            const auto recurrenceKey = m_recurrence->currentData().toString();
            if (recurrenceKey == QStringLiteral("none"))
                result.recurrenceRule = std::nullopt;
            else if (recurrenceKey == QStringLiteral("custom"))
                result.recurrenceRule = m_customRecurrence;
            else
            {
                auto rule = javelin::jmap::calendar::RecurrenceRule{};
                rule.frequency = recurrenceKey == QStringLiteral("daily")
                                     ? javelin::jmap::calendar::RecurrenceFrequency::Daily
                                 : recurrenceKey == QStringLiteral("weekly")
                                     ? javelin::jmap::calendar::RecurrenceFrequency::Weekly
                                 : recurrenceKey == QStringLiteral("monthly")
                                     ? javelin::jmap::calendar::RecurrenceFrequency::Monthly
                                     : javelin::jmap::calendar::RecurrenceFrequency::Yearly;
                result.recurrenceRule = std::move(rule);
            }
        }
        if (m_attendeesEdited)
        {
            std::vector<std::string> addresses;
            for (const auto& row : m_attendeeRows)
                if (const auto address = row.editor->text().trimmed(); !address.isEmpty())
                    addresses.push_back(address.toStdString());
            result.attendees =
                javelin::jmap::calendar::reconcileEditableAttendees(result.attendees, addresses);
        }
        if (m_alertsEdited)
        {
            result.useDefaultAlerts = m_useDefaultAlerts->isChecked();
            for (auto alert = result.alerts.begin(); alert != result.alerts.end();)
            {
                if (alert->second.action == "display")
                    alert = result.alerts.erase(alert);
                else
                    ++alert;
            }
            for (const auto& row : m_alertRows)
            {
                auto alert = row.original;
                alert.id = row.id;
                alert.action = "display";
                alert.acknowledged = row.edited ? std::nullopt : alert.acknowledged;
                if (row.triggerKind->currentData().toString() == QStringLiteral("absolute"))
                {
                    alert.triggerKind = javelin::jmap::calendar::AlertTriggerKind::Absolute;
                    alert.relativeTo = "start";
                    alert.offset = std::nullopt;
                    alert.when = javelin::jmap::calendar::UtcInstant{
                        .value = row.absoluteTime->dateTime()
                                     .toUTC()
                                     .toString(Qt::ISODateWithMs)
                                     .toStdString()};
                }
                else
                {
                    const auto relation = row.relation->currentData().toString();
                    alert.triggerKind = javelin::jmap::calendar::AlertTriggerKind::Offset;
                    alert.relativeTo = relation.endsWith(QStringLiteral("end")) ? "end" : "start";
                    alert.offset = javelin::jmap::calendar::Duration{
                        .value =
                            signedDuration(row.amount->value(), row.unit->currentData().toInt(),
                                           relation.startsWith(QStringLiteral("before")))};
                    alert.when = std::nullopt;
                }
                result.alerts.emplace(alert.id, std::move(alert));
            }
        }
        return result;
    }

    void EventDialog::addAttendeeRow(const QString& address)
    {
        AttendeeRow row;
        row.container = new QWidget(m_attendees);
        auto* layout = new QHBoxLayout(row.container);
        layout->setContentsMargins(0, 0, 0, 0);
        row.editor = new javelin::gui::widgets::EmailAddressLineEdit(false, row.container);
        row.editor->setText(address);
        row.editor->setPlaceholderText(QStringLiteral("name@example.com"));
        row.remove = new QPushButton(QStringLiteral("−"), row.container);
        row.remove->setAccessibleName(QStringLiteral("Remove attendee"));
        row.remove->setFixedWidth(row.remove->sizeHint().height() + 8);
        layout->addWidget(row.editor, 1);
        layout->addWidget(row.remove);
        auto* container = row.container;
        connect(row.editor, &QLineEdit::textEdited, this, [this]() { m_attendeesEdited = true; });
        connect(row.remove, &QPushButton::clicked, this,
                [this, container]()
                {
                    removeAttendeeRow(container);
                    m_attendeesEdited = true;
                });
        m_attendeeRowsLayout->insertWidget(std::max(0, m_attendeeRowsLayout->count() - 1),
                                           row.container);
        m_attendeeRows.push_back(row);
    }

    void EventDialog::removeAttendeeRow(QWidget* row)
    {
        const auto found = std::ranges::find(m_attendeeRows, row, &AttendeeRow::container);
        if (found == m_attendeeRows.end())
            return;
        found->container->deleteLater();
        m_attendeeRows.erase(found);
        if (m_attendeeRows.empty())
            addAttendeeRow();
    }

    void EventDialog::clearAttendeeRows()
    {
        for (const auto& row : m_attendeeRows)
            delete row.container;
        m_attendeeRows.clear();
    }

    void
    EventDialog::addAlertRow(const std::optional<javelin::jmap::calendar::Alert>& existingAlert)
    {
        AlertRow row;
        row.container = new QWidget(m_alerts);
        row.id = existingAlert && !existingAlert->id.empty()
                     ? existingAlert->id
                     : QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        row.original = existingAlert.value_or(javelin::jmap::calendar::Alert{
            .id = row.id,
            .action = "display",
            .triggerKind = javelin::jmap::calendar::AlertTriggerKind::Offset,
            .relativeTo = "start",
            .offset = javelin::jmap::calendar::Duration{.value = "-PT10M"},
            .when = std::nullopt,
            .acknowledged = std::nullopt});
        auto* layout = new QHBoxLayout(row.container);
        layout->setContentsMargins(0, 0, 0, 0);
        row.triggerKind = new QComboBox(row.container);
        row.triggerKind->addItem(QStringLiteral("Relative"), QStringLiteral("offset"));
        row.triggerKind->addItem(QStringLiteral("At date/time"), QStringLiteral("absolute"));
        row.amount = new QSpinBox(row.container);
        row.amount->setRange(0, 10000);
        row.unit = new QComboBox(row.container);
        row.unit->addItem(QStringLiteral("minutes"), 60);
        row.unit->addItem(QStringLiteral("hours"), 3600);
        row.unit->addItem(QStringLiteral("days"), 86400);
        row.unit->addItem(QStringLiteral("weeks"), 7 * 86400);
        row.unit->addItem(QStringLiteral("seconds"), 1);
        row.relation = new QComboBox(row.container);
        row.relation->addItem(QStringLiteral("before event starts"),
                              QStringLiteral("before-start"));
        row.relation->addItem(QStringLiteral("after event starts"), QStringLiteral("after-start"));
        row.relation->addItem(QStringLiteral("before event ends"), QStringLiteral("before-end"));
        row.relation->addItem(QStringLiteral("after event ends"), QStringLiteral("after-end"));
        row.absoluteTime =
            new QDateTimeEdit(QDateTime::currentDateTime().addSecs(600), row.container);
        row.absoluteTime->setCalendarPopup(true);
        row.absoluteTime->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm"));
        row.remove = new QPushButton(QStringLiteral("−"), row.container);
        row.remove->setAccessibleName(QStringLiteral("Remove notification"));
        row.remove->setFixedWidth(row.remove->sizeHint().height() + 8);

        if (row.original.triggerKind == javelin::jmap::calendar::AlertTriggerKind::Absolute)
        {
            row.triggerKind->setCurrentIndex(row.triggerKind->findData(QStringLiteral("absolute")));
            if (row.original.when)
            {
                const auto when = QDateTime::fromString(
                    QString::fromStdString(row.original.when->value), Qt::ISODateWithMs);
                if (when.isValid())
                    row.absoluteTime->setDateTime(when.toLocalTime());
            }
        }
        else
        {
            const auto duration = editableDuration(row.original.offset);
            row.amount->setValue(duration.amount);
            row.unit->setCurrentIndex(row.unit->findData(duration.unitSeconds));
            const auto relation = QStringLiteral("%1-%2").arg(
                duration.negative ? QStringLiteral("before") : QStringLiteral("after"),
                row.original.relativeTo == "end" ? QStringLiteral("end") : QStringLiteral("start"));
            row.relation->setCurrentIndex(row.relation->findData(relation));
        }

        layout->addWidget(row.triggerKind);
        layout->addWidget(row.amount);
        layout->addWidget(row.unit);
        layout->addWidget(row.relation, 1);
        layout->addWidget(row.absoluteTime, 1);
        layout->addWidget(row.remove);
        auto* container = row.container;
        m_alertRows.push_back(row);
        auto& stored = m_alertRows.back();
        const auto changed = [this, container]() { markAlertEdited(container); };
        connect(stored.triggerKind, &QComboBox::currentIndexChanged, this,
                [this, container, changed]()
                {
                    markAlertEdited(container);
                    const auto found =
                        std::ranges::find(m_alertRows, container, &AlertRow::container);
                    if (found != m_alertRows.end())
                        updateAlertRow(*found);
                    changed();
                });
        connect(stored.amount, &QSpinBox::valueChanged, this, changed);
        connect(stored.unit, &QComboBox::currentIndexChanged, this, changed);
        connect(stored.relation, &QComboBox::currentIndexChanged, this, changed);
        connect(stored.absoluteTime, &QDateTimeEdit::dateTimeChanged, this, changed);
        connect(stored.remove, &QPushButton::clicked, this,
                [this, container]()
                {
                    removeAlertRow(container);
                    m_alertsEdited = true;
                });
        updateAlertRow(stored);
        m_alertRowsLayout->insertWidget(std::max(1, m_alertRowsLayout->count() - 1),
                                        stored.container);
    }

    void EventDialog::removeAlertRow(QWidget* row)
    {
        const auto found = std::ranges::find(m_alertRows, row, &AlertRow::container);
        if (found == m_alertRows.end())
            return;
        found->container->deleteLater();
        m_alertRows.erase(found);
    }

    void EventDialog::clearAlertRows()
    {
        for (const auto& row : m_alertRows)
            delete row.container;
        m_alertRows.clear();
    }

    void EventDialog::updateAlertRow(AlertRow& row)
    {
        const bool absolute =
            row.triggerKind->currentData().toString() == QStringLiteral("absolute");
        row.amount->setVisible(!absolute);
        row.unit->setVisible(!absolute);
        row.relation->setVisible(!absolute);
        row.absoluteTime->setVisible(absolute);
    }

    void EventDialog::markAlertEdited(QWidget* row)
    {
        const auto found = std::ranges::find(m_alertRows, row, &AlertRow::container);
        if (found != m_alertRows.end())
            found->edited = true;
        m_alertsEdited = true;
    }

    void EventDialog::updateRecurrenceControls()
    {
        m_customizeRecurrence->setEnabled(m_recurrence->isEnabled() &&
                                          m_recurrence->currentData().toString() !=
                                              QStringLiteral("none"));
    }

    void EventDialog::editCustomRecurrence()
    {
        const auto previousKey = m_committedRecurrenceKey;
        auto initial = m_customRecurrence.value_or(javelin::jmap::calendar::RecurrenceRule{});
        const auto currentKey = m_recurrence->currentData().toString();
        const auto sourceKey = currentKey == QStringLiteral("custom") ? previousKey : currentKey;
        if (sourceKey != QStringLiteral("none") && sourceKey != QStringLiteral("custom"))
        {
            initial = javelin::jmap::calendar::RecurrenceRule{};
            initial.frequency = sourceKey == QStringLiteral("daily")
                                    ? javelin::jmap::calendar::RecurrenceFrequency::Daily
                                : sourceKey == QStringLiteral("weekly")
                                    ? javelin::jmap::calendar::RecurrenceFrequency::Weekly
                                : sourceKey == QStringLiteral("monthly")
                                    ? javelin::jmap::calendar::RecurrenceFrequency::Monthly
                                    : javelin::jmap::calendar::RecurrenceFrequency::Yearly;
        }
        RecurrenceDialog dialog{this};
        const auto storedStart =
            QDateTime::fromString(QString::fromStdString(m_event.start.value), Qt::ISODate);
        dialog.setEventStart(enteredDateTime(m_startDate, m_startTime).value_or(storedStart));
        dialog.setRule(initial);
        if (dialog.exec() != QDialog::Accepted)
        {
            m_recurrence->setCurrentIndex(m_recurrence->findData(previousKey));
            updateRecurrenceControls();
            return;
        }
        m_customRecurrence = dialog.rule();
        m_recurrence->setCurrentIndex(m_recurrence->findData(QStringLiteral("custom")));
        m_committedRecurrenceKey = QStringLiteral("custom");
        m_recurrenceEdited = true;
        updateRecurrenceControls();
    }

    void EventDialog::showMutationError(const QString& message)
    {
        m_error->setText(message);
    }

    void EventDialog::validateAndAccept()
    {
        if (m_title->text().trimmed().isEmpty())
        {
            showMutationError(QStringLiteral("Enter a title."));
            return;
        }
        if (m_calendar->currentIndex() < 0)
        {
            showMutationError(QStringLiteral("Choose a writable calendar."));
            return;
        }
        if (!(m_calendar->model()->flags(
                  m_calendar->model()->index(m_calendar->currentIndex(), 0)) &
              Qt::ItemIsEnabled))
        {
            showMutationError(QStringLiteral("Choose a writable calendar."));
            return;
        }
        const auto start = enteredDateTime(m_startDate, m_startTime);
        const auto end = enteredDateTime(m_endDate, m_endTime);
        if (!start.has_value() || !end.has_value())
        {
            showMutationError(QStringLiteral("Enter start and end times as HH:mm."));
            return;
        }
        if (*end <= *start)
        {
            showMutationError(QStringLiteral("The end must be after the start."));
            return;
        }
        m_error->clear();
        accept();
    }

    void EventDialog::updateAllDayMode(const bool allDay)
    {
        m_startTime->setVisible(!allDay);
        m_endTime->setVisible(!allDay);
        m_timeZone->setEnabled(!allDay);
        if (allDay)
        {
            m_startTime->setCurrentText(QStringLiteral("00:00"));
            m_endTime->setCurrentText(QStringLiteral("00:00"));
            if (m_endDate->date() <= m_startDate->date())
                m_endDate->setDate(m_startDate->date().addDays(1));
        }
    }

    void EventDialog::updateAutomaticEnd()
    {
        const auto start = enteredDateTime(m_startDate, m_startTime);
        if (m_endEdited || !start.has_value())
            return;
        setEnteredDateTime(m_endDate, m_endTime,
                           m_allDay->isChecked() ? QDateTime{start->date().addDays(1), QTime{0, 0}}
                                                 : start->addSecs(3600));
    }

    void EventDialog::markEndEdited()
    {
        m_endEdited = true;
    }
} // namespace javelin::gui::calendar
