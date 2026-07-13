#include "gui/calendar/EventDialog.h"

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
#include <QTimeZone>

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
            if (calendar.myRights.mayWriteAll || calendar.myRights.mayWriteOwn)
                m_calendar->addItem(QString::fromStdString(calendar.name),
                                    QString::fromStdString(calendar.id));
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
        m_attendees = new QPlainTextEdit(this);
        m_attendees->setPlaceholderText(QStringLiteral("One email address per line"));
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
        layout->addRow(QStringLiteral("Recurrence"), m_recurrence);
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
        connect(m_recurrence, &QComboBox::activated, this, [this]() { m_recurrenceEdited = true; });
        connect(m_attendees, &QPlainTextEdit::textChanged, this,
                [this]() { m_attendeesEdited = true; });
    }

    void EventDialog::setEvent(const javelin::jmap::calendar::CalendarEvent& event)
    {
        m_event = event;
        m_delete->setVisible(!event.id.empty());
        m_title->setText(QString::fromStdString(event.title));
        for (const auto& [calendarId, present] : event.calendarIds)
            if (present)
                m_calendar->setCurrentIndex(
                    m_calendar->findData(QString::fromStdString(calendarId)));
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
            const auto isSimple = rule.interval == 1 && !rule.count && !rule.until;
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
        if (recurrenceKey == QStringLiteral("custom") && m_recurrence->findData(recurrenceKey) < 0)
            m_recurrence->addItem(QStringLiteral("Custom recurrence (unchanged)"), recurrenceKey);
        m_recurrence->setCurrentIndex(m_recurrence->findData(recurrenceKey));

        QStringList attendeeLines;
        for (const auto& address :
             javelin::jmap::calendar::editableAttendeeAddresses(event.attendees))
            attendeeLines.push_back(QString::fromStdString(address));
        m_attendees->setPlainText(attendeeLines.join(QLatin1Char('\n')));

        m_calendarEdited = false;
        m_timeZoneEdited = false;
        m_recurrenceEdited = false;
        m_attendeesEdited = false;
    }

    javelin::jmap::calendar::CalendarEvent EventDialog::eventDocument() const
    {
        auto result = m_event;
        result.title = m_title->text().trimmed().toStdString();
        if (m_calendarEdited || result.calendarIds.empty())
        {
            result.calendarIds.clear();
            result.calendarIds.emplace(m_calendar->currentData().toString().toStdString(), true);
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
            else if (recurrenceKey != QStringLiteral("custom"))
                result.recurrenceRule = javelin::jmap::calendar::RecurrenceRule{
                    .frequency = recurrenceKey == QStringLiteral("daily")
                                     ? javelin::jmap::calendar::RecurrenceFrequency::Daily
                                 : recurrenceKey == QStringLiteral("weekly")
                                     ? javelin::jmap::calendar::RecurrenceFrequency::Weekly
                                 : recurrenceKey == QStringLiteral("monthly")
                                     ? javelin::jmap::calendar::RecurrenceFrequency::Monthly
                                     : javelin::jmap::calendar::RecurrenceFrequency::Yearly,
                    .interval = 1,
                    .count = std::nullopt,
                    .until = std::nullopt};
        }
        if (m_attendeesEdited)
        {
            std::vector<std::string> addresses;
            for (const auto& line :
                 m_attendees->toPlainText().split(QLatin1Char('\n'), Qt::SkipEmptyParts))
                addresses.push_back(line.trimmed().toStdString());
            result.attendees =
                javelin::jmap::calendar::reconcileEditableAttendees(result.attendees, addresses);
        }
        return result;
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
