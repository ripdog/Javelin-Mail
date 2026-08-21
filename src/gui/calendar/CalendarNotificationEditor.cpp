#include "gui/calendar/CalendarNotificationEditor.h"

#include <KLocalizedString>

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace javelin::gui::calendar
{
    namespace
    {
        struct EditableDuration
        {
            int amount = 10;
            int unitSeconds = 60;
            bool negative = true;
        };

        std::optional<qint64> signedDurationSeconds(const QString& value)
        {
            static const QRegularExpression expression{QStringLiteral(
                "^([+-])?P(?:(\\d+)W)?(?:(\\d+)D)?(?:T(?:(\\d+)H)?(?:(\\d+)M)?(?:(\\d+)S)?)?$")};
            const auto match = expression.match(value);
            if (!match.hasMatch())
                return std::nullopt;
            const auto number = [&match](const int capture)
            {
                return match.captured(capture).isEmpty() ? 0LL
                                                         : match.captured(capture).toLongLong();
            };
            const auto seconds = number(2) * 7 * 24 * 60 * 60 + number(3) * 24 * 60 * 60 +
                                 number(4) * 60 * 60 + number(5) * 60 + number(6);
            return match.captured(1) == QStringLiteral("-") ? -seconds : seconds;
        }

        EditableDuration
        editableDuration(const std::optional<javelin::jmap::calendar::Duration>& value)
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
            const auto value =
                unitSeconds == 7 * 86400 ? QStringLiteral("%1P%2W").arg(prefix).arg(amount)
                : unitSeconds == 86400   ? QStringLiteral("%1P%2D").arg(prefix).arg(amount)
                : unitSeconds == 3600    ? QStringLiteral("%1PT%2H").arg(prefix).arg(amount)
                : unitSeconds == 60      ? QStringLiteral("%1PT%2M").arg(prefix).arg(amount)
                                         : QStringLiteral("%1PT%2S").arg(prefix).arg(amount);
            return value.toStdString();
        }
    } // namespace

    CalendarNotificationEditor::CalendarNotificationEditor(const bool allowCalendarDefaults,
                                                           QWidget* parent)
        : QWidget(parent)
    {
        m_rowsLayout = new QVBoxLayout(this);
        m_rowsLayout->setContentsMargins(0, 0, 0, 0);
        if (allowCalendarDefaults)
        {
            m_useCalendarDefaults = new QCheckBox(i18n("Use calendar default notifications"), this);
            m_rowsLayout->addWidget(m_useCalendarDefaults);
            connect(m_useCalendarDefaults, &QCheckBox::toggled, this,
                    [this]
                    {
                        if (!m_loading)
                            m_edited = true;
                        updateEnabledState();
                    });
        }
        m_addAlert = new QPushButton(i18n("+ Add notification"), this);
        m_addAlert->setObjectName(QStringLiteral("addNotification"));
        m_rowsLayout->addWidget(m_addAlert, 0, Qt::AlignLeft);
        connect(m_addAlert, &QPushButton::clicked, this,
                [this]
                {
                    addAlertRow();
                    m_edited = true;
                });
    }

    void CalendarNotificationEditor::setAlerts(
        const std::unordered_map<std::string, javelin::jmap::calendar::Alert>& alerts)
    {
        m_loading = true;
        clearAlertRows();
        for (const auto& [id, alert] : alerts)
        {
            Q_UNUSED(id);
            if (alert.action == "display" &&
                alert.triggerKind == javelin::jmap::calendar::AlertTriggerKind::Offset)
                addAlertRow(alert);
        }
        updateEnabledState();
        m_loading = false;
        m_edited = false;
    }

    std::unordered_map<std::string, javelin::jmap::calendar::Alert>
    CalendarNotificationEditor::displayAlerts() const
    {
        std::unordered_map<std::string, javelin::jmap::calendar::Alert> result;
        result.reserve(m_rows.size());
        for (const auto& row : m_rows)
        {
            if (!row.edited)
            {
                result.emplace(row.id, row.original);
                continue;
            }

            auto alert = row.original;
            alert.id = row.id;
            alert.action = "display";
            alert.triggerKind = javelin::jmap::calendar::AlertTriggerKind::Offset;
            const auto relation = row.relation->currentData().toString();
            alert.relativeTo = relation.endsWith(QStringLiteral("end")) ? "end" : "start";
            alert.offset = javelin::jmap::calendar::Duration{
                .value = signedDuration(row.amount->value(), row.unit->currentData().toInt(),
                                        relation.startsWith(QStringLiteral("before")))};
            alert.when = std::nullopt;
            if (row.edited)
                alert.acknowledged = std::nullopt;
            result.emplace(alert.id, std::move(alert));
        }
        return result;
    }

    void CalendarNotificationEditor::setUseCalendarDefaults(const bool enabled)
    {
        if (m_useCalendarDefaults == nullptr)
            return;
        m_loading = true;
        m_useCalendarDefaults->setChecked(enabled);
        updateEnabledState();
        m_loading = false;
    }

    bool CalendarNotificationEditor::useCalendarDefaults() const
    {
        return m_useCalendarDefaults != nullptr && m_useCalendarDefaults->isChecked();
    }

    bool CalendarNotificationEditor::edited() const
    {
        return m_edited;
    }

    void CalendarNotificationEditor::resetEdited()
    {
        m_edited = false;
        for (auto& row : m_rows)
            row.edited = false;
    }

    void CalendarNotificationEditor::addAlertRow(
        const std::optional<javelin::jmap::calendar::Alert>& existingAlert)
    {
        AlertRow row;
        row.container = new QWidget(this);
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
        row.amount = new QSpinBox(row.container);
        row.amount->setRange(0, 10000);
        row.unit = new QComboBox(row.container);
        row.unit->addItem(i18nc("@item time unit", "minutes"), 60);
        row.unit->addItem(i18nc("@item time unit", "hours"), 3600);
        row.unit->addItem(i18nc("@item time unit", "days"), 86400);
        row.unit->addItem(i18nc("@item time unit", "weeks"), 7 * 86400);
        row.unit->addItem(i18nc("@item time unit", "seconds"), 1);
        row.relation = new QComboBox(row.container);
        row.relation->addItem(i18n("before event starts"), QStringLiteral("before-start"));
        row.relation->addItem(i18n("after event starts"), QStringLiteral("after-start"));
        row.relation->addItem(i18n("before event ends"), QStringLiteral("before-end"));
        row.relation->addItem(i18n("after event ends"), QStringLiteral("after-end"));
        row.remove = new QPushButton(QStringLiteral("−"), row.container);
        row.remove->setAccessibleName(i18n("Remove notification"));
        row.remove->setToolTip(i18n("Remove notification"));
        row.remove->setFixedWidth(row.remove->sizeHint().height() + 8);

        const auto duration = editableDuration(row.original.offset);
        row.amount->setValue(duration.amount);
        row.unit->setCurrentIndex(row.unit->findData(duration.unitSeconds));
        const auto relation = QStringLiteral("%1-%2").arg(
            duration.negative ? QStringLiteral("before") : QStringLiteral("after"),
            row.original.relativeTo == "end" ? QStringLiteral("end") : QStringLiteral("start"));
        row.relation->setCurrentIndex(row.relation->findData(relation));

        layout->addWidget(row.amount);
        layout->addWidget(row.unit);
        layout->addWidget(row.relation, 1);
        layout->addWidget(row.remove);
        auto* container = row.container;
        m_rows.push_back(row);
        auto& stored = m_rows.back();
        const auto changed = [this, container]() { markAlertEdited(container); };
        connect(stored.amount, &QSpinBox::valueChanged, this, changed);
        connect(stored.unit, &QComboBox::currentIndexChanged, this, changed);
        connect(stored.relation, &QComboBox::currentIndexChanged, this, changed);
        connect(stored.remove, &QPushButton::clicked, this,
                [this, container]
                {
                    removeAlertRow(container);
                    if (!m_loading)
                        m_edited = true;
                });
        const auto insertionIndex = m_rowsLayout->count() - 1;
        m_rowsLayout->insertWidget(
            std::max(m_useCalendarDefaults != nullptr ? 1 : 0, insertionIndex), stored.container);
        updateEnabledState();
    }

    void CalendarNotificationEditor::removeAlertRow(QWidget* row)
    {
        const auto found = std::ranges::find(m_rows, row, &AlertRow::container);
        if (found == m_rows.end())
            return;
        found->container->deleteLater();
        m_rows.erase(found);
    }

    void CalendarNotificationEditor::clearAlertRows()
    {
        for (const auto& row : m_rows)
            delete row.container;
        m_rows.clear();
    }

    void CalendarNotificationEditor::markAlertEdited(QWidget* row)
    {
        const auto found = std::ranges::find(m_rows, row, &AlertRow::container);
        if (found != m_rows.end())
            found->edited = true;
        if (!m_loading)
            m_edited = true;
    }

    void CalendarNotificationEditor::updateEnabledState()
    {
        const bool enabled =
            m_useCalendarDefaults == nullptr || !m_useCalendarDefaults->isChecked();
        m_addAlert->setEnabled(enabled);
        for (auto& row : m_rows)
            row.container->setEnabled(enabled);
    }
} // namespace javelin::gui::calendar
