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
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    class Reader final : public javelin::jmap::calendar::CalendarReader
    {
      public:
        bool writable = true;
        mutable int accountReadCount = 0;
        mutable int calendarReadCount = 0;

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
            ++accountReadCount;
            return std::vector<javelin::jmap::cache::CalendarAccount>{{
                .ownerAccountId = "owner-1",
                .accountId = "calendar-1",
                .name = "Personal",
            }};
        }

        javelin::jmap::calendar::CalendarListResult
        calendars(const std::string_view accountId) const override
        {
            ++calendarReadCount;
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
                             .mayWriteAll = writable,
                             .mayWriteOwn = writable,
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
        int colorBatchRequestCount = 0;
        int defaultAlertsBatchRequestCount = 0;
        int subscriptionRequestCount = 0;
        int deleteCalendarRequestCount = 0;
        std::string lastOwnerAccountId;
        std::string lastAccountId;
        std::string lastCalendarId;
        bool lastSubscribed = false;
        std::vector<javelin::app::CalendarColorChange> lastColorBatch;
        std::vector<javelin::app::CalendarDefaultAlertsChange> lastDefaultAlertsBatch;
        std::vector<std::string> managerBatchOrder;

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
                .calendarMetadataAuthoritative = true,
                .reconciledMutations = {},
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
        setCalendarSubscribed(std::string ownerAccountId, std::string accountId,
                              std::string calendarId, const bool subscribed) override
        {
            ++subscriptionRequestCount;
            lastOwnerAccountId = std::move(ownerAccountId);
            lastAccountId = accountId;
            lastCalendarId = std::move(calendarId);
            lastSubscribed = subscribed;
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

        QCoro::Task<javelin::app::CalendarColorBatchResult>
        setCalendarColors(std::vector<javelin::app::CalendarColorChange> changes) override
        {
            ++colorBatchRequestCount;
            managerBatchOrder.push_back("color");
            lastColorBatch = std::move(changes);
            co_return javelin::app::CalendarColorBatchResult{
                .requestedCount = lastColorBatch.size(),
                .appliedCount = lastColorBatch.size(),
                .failures = {},
                .error = std::nullopt,
            };
        }

        QCoro::Task<javelin::app::CalendarDefaultAlertsBatchResult> setCalendarDefaultAlerts(
            std::vector<javelin::app::CalendarDefaultAlertsChange> changes) override
        {
            ++defaultAlertsBatchRequestCount;
            managerBatchOrder.push_back("alerts");
            lastDefaultAlertsBatch = std::move(changes);
            co_return javelin::app::CalendarDefaultAlertsBatchResult{
                .requestedCount = lastDefaultAlertsBatch.size(),
                .appliedCount = lastDefaultAlertsBatch.size(),
                .failures = {},
                .error = std::nullopt,
            };
        }

        QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        createCalendar(std::string, javelin::jmap::calendar::CreateCalendarCommand command) override
        {
            co_return committed(command.accountId);
        }

        QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
        deleteCalendar(std::string ownerAccountId,
                       javelin::jmap::calendar::DeleteCalendarCommand command) override
        {
            ++deleteCalendarRequestCount;
            lastOwnerAccountId = std::move(ownerAccountId);
            lastAccountId = command.accountId;
            lastCalendarId = command.calendarId;
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

TEST_CASE("calendar workspace state distinguishes availability from event creation rights",
          "[gui][calendar][actions][rights]")
{
    javelin::gui::settings::GuiSettings settings{javelin::protocol::SettingsSnapshot{}};
    Reader reader;
    reader.writable = false;
    CommandPort commands;
    QStackedWidget contentStack;
    std::vector<javelin::gui::shell::TabState> tabs;
    javelin::gui::shell::CalendarTabController controller{settings, reader, commands, contentStack,
                                                          tabs};

    const auto state = controller.workspaceState();
    CHECK(state.available);
    CHECK_FALSE(state.canCreateEvent);
    CHECK(state.canManageCalendars);
    CHECK(state.canRefresh);
    CHECK(reader.accountReadCount == 1);
    CHECK(reader.calendarReadCount == 1);

    static_cast<void>(controller.workspaceState());
    controller.invokeWorkspace(javelin::gui::shell::CalendarTabCommand::CreateEvent);
    CHECK(tabs.empty());
    CHECK(reader.accountReadCount == 1);
    CHECK(reader.calendarReadCount == 1);

    int stateChanges = 0;
    QObject::connect(&controller,
                     &javelin::gui::shell::CalendarTabController::workspaceStateChanged,
                     &contentStack, [&stateChanges] { ++stateChanges; });
    reader.writable = true;
    controller.accountsChanged();
    CHECK(stateChanges == 1);
    CHECK(controller.workspaceState().canCreateEvent);
    CHECK(reader.accountReadCount == 2);
    CHECK(reader.calendarReadCount == 2);
}

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

TEST_CASE("calendar manager saves color before default notification changes",
          "[gui][calendar][notifications][actions]")
{
    javelin::gui::settings::GuiSettings settings{javelin::protocol::SettingsSnapshot{}};
    Reader reader;
    CommandPort commands;
    QStackedWidget contentStack;
    std::vector<javelin::gui::shell::TabState> tabs;
    javelin::gui::shell::CalendarTabController controller{settings, reader, commands, contentStack,
                                                          tabs};

    controller.invokeWorkspace(javelin::gui::shell::CalendarTabCommand::Today);
    REQUIRE(tabs.size() == 1);
    auto* widget =
        qobject_cast<javelin::gui::calendar::MonthCalendarWidget*>(contentStack.widget(0));
    REQUIRE(widget != nullptr);
    const std::unordered_map<std::string, javelin::jmap::calendar::Alert> timed{
        {"reminder",
         {.id = "reminder",
          .action = "display",
          .triggerKind = javelin::jmap::calendar::AlertTriggerKind::Offset,
          .relativeTo = "start",
          .offset = javelin::jmap::calendar::Duration{.value = "-PT15M"},
          .when = std::nullopt,
          .acknowledged = std::nullopt}},
    };

    Q_EMIT widget->calendarManagerChangesSaved({{.calendar = {.ownerAccountId = "owner-1",
                                                              .accountId = "calendar-1",
                                                              .calendarId = "cal-1"},
                                                 .color = std::optional<std::string>{"#336699"}}},
                                               {{.calendar = {.ownerAccountId = "owner-1",
                                                              .accountId = "calendar-1",
                                                              .calendarId = "cal-1"},
                                                 .withTime = timed,
                                                 .withoutTime = {}}});
    QCoreApplication::processEvents();

    REQUIRE(commands.managerBatchOrder.size() == 2);
    CHECK(commands.managerBatchOrder == std::vector<std::string>{"color", "alerts"});
    REQUIRE(commands.lastDefaultAlertsBatch.size() == 1);
    REQUIRE(commands.lastDefaultAlertsBatch.front().withTime.contains("reminder"));
    CHECK(commands.lastDefaultAlertsBatch.front().withTime.at("reminder").offset->value ==
          "-PT15M");
}

TEST_CASE("calendar color edits cross the controller as one typed batch",
          "[gui][calendar][color][actions]")
{
    javelin::gui::settings::GuiSettings settings{javelin::protocol::SettingsSnapshot{}};
    Reader reader;
    CommandPort commands;
    QStackedWidget contentStack;
    std::vector<javelin::gui::shell::TabState> tabs;
    javelin::gui::shell::CalendarTabController controller{settings, reader, commands, contentStack,
                                                          tabs};

    controller.invokeWorkspace(javelin::gui::shell::CalendarTabCommand::Today);
    REQUIRE(tabs.size() == 1);
    auto* widget =
        qobject_cast<javelin::gui::calendar::MonthCalendarWidget*>(contentStack.widget(0));
    REQUIRE(widget != nullptr);

    Q_EMIT widget->calendarColorsChanged({
        {.calendar = {.ownerAccountId = "owner-1",
                      .accountId = "calendar-1",
                      .calendarId = "cal-1"},
         .color = std::optional<std::string>{"#336699"}},
        {.calendar = {.ownerAccountId = "owner-2",
                      .accountId = "calendar-2",
                      .calendarId = "cal-2"},
         .color = std::nullopt},
    });
    QCoreApplication::processEvents();

    CHECK(commands.colorBatchRequestCount == 1);
    REQUIRE(commands.lastColorBatch.size() == 2);
    CHECK(commands.lastColorBatch[0].ownerAccountId == "owner-1");
    CHECK(commands.lastColorBatch[0].accountId == "calendar-1");
    CHECK(commands.lastColorBatch[0].calendarId == "cal-1");
    CHECK(commands.lastColorBatch[0].color == std::optional<std::string>{"#336699"});
    CHECK(commands.lastColorBatch[1].ownerAccountId == "owner-2");
    CHECK(commands.lastColorBatch[1].accountId == "calendar-2");
    CHECK(commands.lastColorBatch[1].calendarId == "cal-2");
    CHECK_FALSE(commands.lastColorBatch[1].color.has_value());

    const javelin::gui::calendar::CalendarIdentity identity{
        .ownerAccountId = "owner-1",
        .accountId = "calendar-1",
        .calendarId = "cal-1",
    };
    Q_EMIT widget->calendarSubscriptionChanged(identity, false);
    QCoreApplication::processEvents();
    CHECK(commands.subscriptionRequestCount == 1);
    CHECK(commands.lastOwnerAccountId == "owner-1");
    CHECK(commands.lastAccountId == "calendar-1");
    CHECK(commands.lastCalendarId == "cal-1");
    CHECK_FALSE(commands.lastSubscribed);

    Q_EMIT widget->calendarDeletionRequested(identity);
    QCoreApplication::processEvents();
    CHECK(commands.deleteCalendarRequestCount == 1);
    CHECK(commands.lastOwnerAccountId == "owner-1");
    CHECK(commands.lastAccountId == "calendar-1");
    CHECK(commands.lastCalendarId == "cal-1");
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
