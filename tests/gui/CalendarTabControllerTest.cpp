#include "gui/shell/CalendarTabController.h"

#include "app/CalendarApplicationPorts.h"
#include "gui/calendar/MonthCalendarWidget.h"
#include "gui/settings/GuiSettings.h"
#include "jmap/calendar/CalendarReader.h"

#include <QApplication>
#include <QDate>
#include <QDialog>
#include <QStackedWidget>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    class Reader final : public javelin::jmap::calendar::CalendarReader
    {
      public:
        javelin::jmap::calendar::CalendarLoadResult
        loadCached(const std::string_view accountId,
                   const javelin::jmap::calendar::VisibleInterval& interval,
                   const javelin::jmap::calendar::TimeZoneId& displayTimeZone) const override
        {
            return std::optional<javelin::jmap::cache::CalendarWindow>{
                javelin::jmap::cache::CalendarWindow{
                    .accountId = std::string{accountId},
                    .start = interval.start,
                    .end = interval.end,
                    .displayTimeZone = displayTimeZone,
                    .queryState = "q1",
                    .eventState = "e1",
                    .events = {},
                    .occurrences = {},
                }};
        }

        javelin::jmap::calendar::CalendarAccountsResult accounts() const override
        {
            return std::vector<javelin::jmap::cache::CalendarAccount>{{
                .ownerAccountId = "owner-1",
                .accountId = "calendar-1",
                .name = "Personal",
            }};
        }

        javelin::jmap::calendar::CalendarListResult
        calendars(const std::string_view accountId) const override
        {
            return std::vector<javelin::jmap::calendar::Calendar>{{
                .accountId = std::string{accountId},
                .id = "cal-1",
                .name = "Personal",
                .description = std::nullopt,
                .color = std::nullopt,
                .sortOrder = 0,
                .isSubscribed = true,
                .isVisible = true,
                .isDefault = true,
                .timeZone = std::nullopt,
                .defaultAlertsWithTime = {},
                .defaultAlertsWithoutTime = {},
                .myRights = {.mayReadFreeBusy = true,
                             .mayReadItems = true,
                             .mayWriteAll = true,
                             .mayWriteOwn = true,
                             .mayUpdatePrivate = true,
                             .mayRSVP = true,
                             .mayShare = false,
                             .mayDelete = false},
            }};
        }

        javelin::jmap::calendar::ParticipantIdentityListResult
        participantIdentities(std::string_view) const override
        {
            return std::vector<javelin::jmap::calendar::ParticipantIdentity>{};
        }

        javelin::jmap::calendar::PendingCalendarInvitationsResult
        pendingInvitations() const override
        {
            return std::vector<javelin::jmap::calendar::PendingCalendarInvitation>{};
        }

        javelin::jmap::calendar::CalendarEventReadResult event(std::string_view,
                                                               std::string_view) const override
        {
            return std::optional<javelin::jmap::calendar::CalendarEvent>{};
        }
    };

    class CommandPort final : public javelin::app::CalendarCommandPort
    {
      public:
        int rangeRequestCount = 0;

        QCoro::Task<javelin::jmap::calendar::CalendarRefreshResult>
        requestCalendarRange(std::string, javelin::jmap::calendar::VisibleInterval interval,
                             javelin::jmap::calendar::TimeZoneId displayTimeZone) override
        {
            ++rangeRequestCount;
            co_return javelin::jmap::calendar::RefreshedRange{
                .interval = std::move(interval),
                .displayTimeZone = std::move(displayTimeZone),
                .accountCount = 1,
                .eventCount = 0,
            };
        }

        QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        createCalendarEvent(std::string, javelin::jmap::calendar::CreateEventCommand command,
                            javelin::app::undo::CommandOrigin) override
        {
            co_return committed(command.accountId);
        }

        QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        updateCalendarEvent(std::string, javelin::jmap::calendar::UpdateEventCommand command,
                            javelin::app::undo::CommandOrigin) override
        {
            co_return committed(command.accountId);
        }

        QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        deleteCalendarEvent(std::string, javelin::jmap::calendar::DeleteEventCommand command,
                            javelin::app::undo::CommandOrigin) override
        {
            co_return committed(command.accountId);
        }

        QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        respondToCalendarEvent(std::string,
                               javelin::jmap::calendar::RespondToEventCommand command) override
        {
            co_return committed(command.accountId);
        }

        QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        setCalendarSubscribed(std::string, std::string accountId, std::string, bool) override
        {
            co_return committed(std::move(accountId));
        }

        QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        setDefaultCalendar(std::string, std::string accountId, std::string,
                           javelin::app::undo::CommandOrigin) override
        {
            co_return committed(std::move(accountId));
        }

        QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        setCalendarColor(std::string, std::string accountId, std::string,
                         std::optional<std::string>) override
        {
            co_return committed(std::move(accountId));
        }

        QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        createCalendar(std::string, javelin::jmap::calendar::CreateCalendarCommand command) override
        {
            co_return committed(command.accountId);
        }

        QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        deleteCalendar(std::string, javelin::jmap::calendar::DeleteCalendarCommand command) override
        {
            co_return committed(command.accountId);
        }

        javelin::jmap::calendar::CalendarPreferenceResult
        setCalendarVisible(std::string, std::string, bool,
                           javelin::app::undo::CommandOrigin) override
        {
            return std::monostate{};
        }

      private:
        static javelin::jmap::calendar::CommittedMutation committed(std::string accountId)
        {
            return {
                .accountId = std::move(accountId),
                .newState = "state-2",
                .createdId = std::nullopt,
                .receipt = {},
            };
        }
    };
} // namespace

TEST_CASE("workspace Calendar refresh does not duplicate materialization refresh",
          "[gui][calendar][actions]")
{
    javelin::gui::settings::GuiSettings settings{javelin::protocol::SettingsSnapshot{}};
    Reader reader;
    CommandPort commands;
    QStackedWidget contentStack;
    std::vector<javelin::gui::shell::TabState> tabs;
    javelin::gui::shell::CalendarTabController controller{settings, reader, commands, contentStack,
                                                          tabs};

    controller.invokeWorkspace(javelin::gui::shell::CalendarTabCommand::Refresh);

    REQUIRE(tabs.size() == 1);
    CHECK(commands.rangeRequestCount == 1);

    controller.invokeWorkspace(javelin::gui::shell::CalendarTabCommand::Refresh);
    CHECK(commands.rangeRequestCount == 2);
}

TEST_CASE("workspace calendar manager command materializes Calendar before opening",
          "[gui][calendar][actions]")
{
    javelin::gui::settings::GuiSettings settings{javelin::protocol::SettingsSnapshot{}};
    Reader reader;
    CommandPort commands;
    QStackedWidget contentStack;
    std::vector<javelin::gui::shell::TabState> tabs;
    javelin::gui::shell::CalendarTabController controller{settings, reader, commands, contentStack,
                                                          tabs};
    bool managerOpened = false;
    QTimer::singleShot(0, &contentStack,
                       [&managerOpened]
                       {
                           auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                           REQUIRE(dialog != nullptr);
                           managerOpened = true;
                           dialog->reject();
                       });

    controller.invokeWorkspace(javelin::gui::shell::CalendarTabCommand::ManageCalendars);

    CHECK(managerOpened);
    REQUIRE(tabs.size() == 1);
    CHECK(qobject_cast<javelin::gui::calendar::MonthCalendarWidget*>(contentStack.widget(0)) !=
          nullptr);
}

TEST_CASE("workspace calendar commands materialize Calendar before executing",
          "[gui][calendar][actions]")
{
    javelin::gui::settings::GuiSettings settings{javelin::protocol::SettingsSnapshot{}};
    Reader reader;
    CommandPort commands;
    QStackedWidget contentStack;
    std::vector<javelin::gui::shell::TabState> tabs;
    javelin::gui::shell::CalendarTabController controller{settings, reader, commands, contentStack,
                                                          tabs};
    int activatedIndex = -1;
    QObject::connect(&controller, &javelin::gui::shell::CalendarTabController::tabReady,
                     &contentStack, [&activatedIndex](const int index) { activatedIndex = index; });

    controller.invokeWorkspace(javelin::gui::shell::CalendarTabCommand::Today);

    CHECK(activatedIndex == 0);
    REQUIRE(tabs.size() == 1);
    auto* widget =
        qobject_cast<javelin::gui::calendar::MonthCalendarWidget*>(contentStack.widget(0));
    REQUIRE(widget != nullptr);
    CHECK(widget->selectedDate() == QDate::currentDate());
    CHECK(widget->displayedMonth() ==
          QDate{QDate::currentDate().year(), QDate::currentDate().month(), 1});
}
