#include "gui/calendar/EventDialog.h"
#include "gui/calendar/RecurrenceDialog.h"
#include "gui/widgets/EmailAddressLineEdit.h"

#include "jmap/calendar/CalendarEventEditing.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardItemModel>
#include <QTimeZone>
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

    QString calendarKey(const std::string& accountId, const std::string& calendarId)
    {
        return QString::fromStdString(accountId) + QLatin1Char('\n') +
               QString::fromStdString(calendarId);
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
            auto simpleRule = javelin::jmap::calendar::RecurrenceRule{};
            simpleRule.frequency = rule.frequency;
            const auto isSimple = rule == simpleRule;
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

        m_calendarEdited = false;
        m_timeZoneEdited = false;
        m_recurrenceEdited = false;
        m_attendeesEdited = false;
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
