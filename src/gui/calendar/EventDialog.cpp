#include "gui/calendar/EventDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimeZone>

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
        m_start = new QDateTimeEdit(QDateTime::currentDateTime(), this);
        m_end = new QDateTimeEdit(QDateTime::currentDateTime().addSecs(3600), this);
        m_start->setCalendarPopup(true);
        m_end->setCalendarPopup(true);
        m_timeZone = new QComboBox(this);
        const auto zones = QTimeZone::availableTimeZoneIds();
        for (const auto& zone : zones)
            m_timeZone->addItem(QString::fromUtf8(zone));
        m_timeZone->setCurrentText(QString::fromUtf8(QTimeZone::systemTimeZoneId()));
        m_description = new QPlainTextEdit(this);
        m_location = new QLineEdit(this);
        m_recurrence = new QComboBox(this);
        m_recurrence->addItems({QStringLiteral("Does not repeat"), QStringLiteral("Daily"),
                                QStringLiteral("Weekly"), QStringLiteral("Monthly"),
                                QStringLiteral("Yearly")});
        m_attendees = new QPlainTextEdit(this);
        m_attendees->setPlaceholderText(QStringLiteral("One email address per line"));
        m_error = new QLabel(this);
        m_error->setStyleSheet(QStringLiteral("color: palette(link-visited);"));
        m_error->setWordWrap(true);
        layout->addRow(QStringLiteral("Title"), m_title);
        layout->addRow(QStringLiteral("Calendar"), m_calendar);
        layout->addRow(QString{}, m_allDay);
        layout->addRow(QStringLiteral("Start"), m_start);
        layout->addRow(QStringLiteral("End"), m_end);
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
            m_start->setDateTime(start);
        m_timeZone->setCurrentText(QString::fromStdString(event.timeZone.value));
        m_description->setPlainText(event.description ? QString::fromStdString(*event.description)
                                                      : QString{});
        m_location->setText(event.location ? QString::fromStdString(*event.location) : QString{});
    }

    javelin::jmap::calendar::CalendarEvent EventDialog::eventDocument() const
    {
        auto result = m_event;
        result.title = m_title->text().trimmed().toStdString();
        result.calendarIds.clear();
        result.calendarIds.emplace(m_calendar->currentData().toString().toStdString(), true);
        result.showWithoutTime = m_allDay->isChecked();
        result.start.value = m_start->dateTime().toString(Qt::ISODate).toStdString();
        result.duration.value = QStringLiteral("PT%1S")
                                    .arg(m_start->dateTime().secsTo(m_end->dateTime()))
                                    .toStdString();
        result.timeZone.value = m_timeZone->currentText().toStdString();
        const auto description = m_description->toPlainText().trimmed();
        result.description =
            description.isEmpty() ? std::nullopt : std::optional{description.toStdString()};
        const auto location = m_location->text().trimmed();
        result.location = location.isEmpty() ? std::nullopt : std::optional{location.toStdString()};
        if (m_recurrence->currentIndex() == 0)
            result.recurrenceRule = std::nullopt;
        else
            result.recurrenceRule = javelin::jmap::calendar::RecurrenceRule{
                .frequency = static_cast<javelin::jmap::calendar::RecurrenceFrequency>(
                    m_recurrence->currentIndex() == 1   ? 3
                    : m_recurrence->currentIndex() == 2 ? 2
                    : m_recurrence->currentIndex() == 3 ? 1
                                                        : 0),
                .interval = 1,
                .count = std::nullopt,
                .until = std::nullopt};
        result.attendees.clear();
        const auto addresses =
            m_attendees->toPlainText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        int index = 0;
        for (const auto& line : addresses)
        {
            const auto address = line.trimmed();
            result.attendees.push_back(
                {.id = QStringLiteral("attendee-%1").arg(++index).toStdString(),
                 .name = address.toStdString(),
                 .email = address.toStdString(),
                 .calendarAddress = QStringLiteral("mailto:%1").arg(address).toStdString(),
                 .participationStatus = "needs-action",
                 .isOwner = false,
                 .isAttendee = true,
                 .scheduleSequence = 0,
                 .scheduleUpdated = std::nullopt});
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
        if (m_end->dateTime() <= m_start->dateTime())
        {
            showMutationError(QStringLiteral("The end must be after the start."));
            return;
        }
        m_error->clear();
        accept();
    }

    void EventDialog::updateAllDayMode(const bool allDay)
    {
        m_start->setDisplayFormat(allDay ? QStringLiteral("yyyy-MM-dd")
                                         : QStringLiteral("yyyy-MM-dd HH:mm"));
        m_end->setDisplayFormat(allDay ? QStringLiteral("yyyy-MM-dd")
                                       : QStringLiteral("yyyy-MM-dd HH:mm"));
        m_timeZone->setEnabled(!allDay);
    }
} // namespace javelin::gui::calendar
