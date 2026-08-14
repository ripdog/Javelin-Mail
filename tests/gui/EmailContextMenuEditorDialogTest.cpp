#include "gui/shell/EmailContextMenuEditorDialog.h"
#include "gui/settings/GuiSettings.h"
#include "gui/shell/EmailContextMenuLayout.h"

#include <KActionCollection>
#include <KActionSelector>

#include <QDialogButtonBox>
#include <QListWidget>
#include <QObject>
#include <QPushButton>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("email context menu editor supports separator insertion and cancellation",
          "[gui][email-context-menu]")
{
    javelin::protocol::SettingsSnapshot snapshot;
    snapshot.revision = {.value = 3};
    javelin::gui::settings::GuiSettings settings{std::move(snapshot)};

    QObject actionOwner;
    KActionCollection actions{&actionOwner};

    javelin::gui::shell::EmailContextMenuEditorDialog dialog{settings, actions};
    auto* selector =
        dialog.findChild<KActionSelector*>(QStringLiteral("emailContextMenuActionSelector"));
    REQUIRE(selector != nullptr);
    CHECK(selector->selectedListWidget()->count() == 4);

    auto* addSeparator =
        dialog.findChild<QPushButton*>(QStringLiteral("addEmailContextMenuSeparator"));
    REQUIRE(addSeparator != nullptr);
    addSeparator->click();
    CHECK(selector->selectedListWidget()->count() == 5);

    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    REQUIRE(buttons != nullptr);
    buttons->button(QDialogButtonBox::RestoreDefaults)->click();
    CHECK(selector->selectedListWidget()->count() == 4);
    buttons->button(QDialogButtonBox::Cancel)->click();

    CHECK(dialog.result() == QDialog::Rejected);
    CHECK(settings.workspaceSettings().emailContextMenuLayout.empty());
    CHECK(settings.snapshot().revision.value == 3);
}
