#include "gui/calendar/CalendarEventContextMenuEditorDialog.h"
#include "gui/settings/GuiSettings.h"

#include <KActionCollection>
#include <KActionSelector>

#include <QDialogButtonBox>
#include <QListWidget>
#include <QObject>
#include <QPushButton>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("calendar event context menu editor supports separators and cancellation",
          "[gui][calendar-context-menu]")
{
    javelin::protocol::SettingsSnapshot snapshot;
    snapshot.revision = {.value = 4};
    javelin::gui::settings::GuiSettings settings{std::move(snapshot)};
    QObject actionOwner;
    KActionCollection actions{&actionOwner};

    javelin::gui::calendar::CalendarEventContextMenuEditorDialog dialog{settings, actions};
    auto* selector = dialog.findChild<KActionSelector*>(
        QStringLiteral("calendarEventContextMenuActionSelector"));
    REQUIRE(selector != nullptr);
    CHECK(selector->selectedListWidget()->count() == 4);

    auto* addSeparator =
        dialog.findChild<QPushButton*>(QStringLiteral("addCalendarEventContextMenuSeparator"));
    REQUIRE(addSeparator != nullptr);
    addSeparator->click();
    CHECK(selector->selectedListWidget()->count() == 5);

    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    REQUIRE(buttons != nullptr);
    buttons->button(QDialogButtonBox::RestoreDefaults)->click();
    CHECK(selector->selectedListWidget()->count() == 4);
    buttons->button(QDialogButtonBox::Cancel)->click();
    CHECK(dialog.result() == QDialog::Rejected);
    CHECK(settings.workspaceSettings().calendarEventContextMenuLayout.empty());
    CHECK(settings.snapshot().revision.value == 4);
}
