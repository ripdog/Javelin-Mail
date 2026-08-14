#include "gui/calendar/CalendarEventContextMenuEditorDialog.h"

#include "gui/calendar/CalendarEventContextMenuLayout.h"
#include "gui/settings/GuiSettings.h"

#include <KActionCollection>
#include <KActionSelector>
#include <KLocalizedString>

#include <QAction>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace javelin::gui::calendar
{
    namespace
    {
        constexpr auto actionIdRole = Qt::UserRole;

        [[nodiscard]] QString displayText(const QAction& action)
        {
            auto text = action.text();
            text.remove(QLatin1Char('&'));
            return text;
        }
    } // namespace

    CalendarEventContextMenuEditorDialog::CalendarEventContextMenuEditorDialog(
        javelin::gui::settings::GuiSettings& settings, KActionCollection& actions, QWidget* parent)
        : QDialog(parent), m_settings(settings), m_actions(actions)
    {
        setWindowTitle(i18n("Configure Calendar Event Context Menu"));
        resize(760, 480);
        auto* layout = new QVBoxLayout(this);
        m_selector = new KActionSelector(this);
        m_selector->setObjectName(QStringLiteral("calendarEventContextMenuActionSelector"));
        m_selector->setAvailableLabel(i18n("Available actions:"));
        m_selector->setSelectedLabel(i18n("Current actions:"));
        m_selector->setAvailableInsertionPolicy(KActionSelector::Sorted);
        layout->addWidget(m_selector, 1);

        auto* separatorRow = new QHBoxLayout;
        auto* addSeparator = new QPushButton(i18n("Add Separator"), this);
        addSeparator->setObjectName(QStringLiteral("addCalendarEventContextMenuSeparator"));
        separatorRow->addStretch(1);
        separatorRow->addWidget(addSeparator);
        layout->addLayout(separatorRow);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel |
                                                 QDialogButtonBox::RestoreDefaults,
                                             this);
        layout->addWidget(buttons);
        connect(addSeparator, &QPushButton::clicked, this,
                &CalendarEventContextMenuEditorDialog::addSeparator);
        connect(m_selector, &KActionSelector::removed, this,
                [this](QListWidgetItem* item)
                {
                    if (item->data(actionIdRole).toString() !=
                        calendarEventContextMenuSeparatorId())
                        return;
                    delete m_selector->availableListWidget()->takeItem(
                        m_selector->availableListWidget()->row(item));
                });
        connect(buttons, &QDialogButtonBox::accepted, this,
                &CalendarEventContextMenuEditorDialog::save);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this,
                [this] { populate(defaultCalendarEventContextMenuLayout()); });

        populate(effectiveCalendarEventContextMenuLayout(
            m_settings.workspaceSettings().calendarEventContextMenuLayout));
    }

    QListWidgetItem* CalendarEventContextMenuEditorDialog::itemForId(const QString& id) const
    {
        if (id == calendarEventContextMenuSeparatorId())
        {
            auto* item = new QListWidgetItem(i18nc("context menu separator", "— Separator —"));
            item->setData(actionIdRole, id);
            return item;
        }
        auto* action = m_actions.action(id);
        if (action == nullptr)
            return nullptr;
        auto* item = new QListWidgetItem(action->icon(), displayText(*action));
        item->setData(actionIdRole, id);
        return item;
    }

    void CalendarEventContextMenuEditorDialog::populate(const std::vector<QString>& layout)
    {
        auto* available = m_selector->availableListWidget();
        auto* selected = m_selector->selectedListWidget();
        available->clear();
        selected->clear();

        const auto normalized = normalizeCalendarEventContextMenuLayout(layout);
        for (const auto& id : normalized)
            if (auto* item = itemForId(id); item != nullptr)
                selected->addItem(item);
        for (const auto& id : supportedCalendarEventContextMenuActionIds())
        {
            if (std::ranges::contains(normalized, id))
                continue;
            if (auto* item = itemForId(id); item != nullptr)
                available->addItem(item);
        }
        available->sortItems();
        if (selected->count() > 0)
            selected->setCurrentRow(0);
    }

    void CalendarEventContextMenuEditorDialog::addSeparator()
    {
        auto* selected = m_selector->selectedListWidget();
        auto* item = itemForId(calendarEventContextMenuSeparatorId());
        const int currentRow = selected->currentRow();
        selected->insertItem(currentRow < 0 ? selected->count() : currentRow + 1, item);
        selected->setCurrentItem(item);
    }

    std::vector<QString> CalendarEventContextMenuEditorDialog::selectedLayout() const
    {
        const auto* selected = m_selector->selectedListWidget();
        std::vector<QString> layout;
        layout.reserve(static_cast<std::size_t>(selected->count()));
        for (int row = 0; row < selected->count(); ++row)
            layout.push_back(selected->item(row)->data(actionIdRole).toString());
        return normalizeCalendarEventContextMenuLayout(layout);
    }

    void CalendarEventContextMenuEditorDialog::save()
    {
        const auto selected = selectedLayout();
        if (selected.empty())
        {
            QMessageBox::warning(this, windowTitle(),
                                 i18n("The context menu must contain at least one action."));
            return;
        }
        auto workspace = m_settings.workspaceSettings();
        workspace.calendarEventContextMenuLayout =
            calendarEventContextMenuOverrideForLayout(selected);
        if (const auto error = m_settings.updateWorkspace(std::move(workspace)))
        {
            QMessageBox::warning(
                this, windowTitle(),
                i18n("The calendar event context menu could not be saved.\n\n%1", error->detail));
            return;
        }
        accept();
    }
} // namespace javelin::gui::calendar
