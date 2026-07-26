#include "app/undo/CalendarPreferenceExecutor.h"

#include <QCoroTask>

#include <catch2/catch_test_macros.hpp>

namespace
{
    using namespace javelin::app::undo;

    class FakePreferencePort final : public CalendarPreferencePort
    {
      public:
        std::optional<std::string> current = "false";
        int writes = 0;

        std::variant<std::optional<std::string>, javelin::jmap::OperationError>
        currentCalendarPreference(const CalendarPreferenceHistory&) const override
        {
            return current;
        }

        QCoro::Task<std::optional<javelin::jmap::OperationError>>
        applyCalendarPreference(CalendarPreferenceHistory, std::optional<std::string> value,
                                CommandOrigin) override
        {
            ++writes;
            current = std::move(value);
            co_return std::nullopt;
        }
    };

    [[nodiscard]] HistoryEntry entry()
    {
        HistoryEntry result;
        result.entryId = QStringLiteral("preference-history");
        result.label = QStringLiteral("Show Calendar");
        result.domain = HistoryDomain::LocalPreference;
        result.commandKind = QStringLiteral("calendar_preference");
        result.payload = CalendarPreferenceHistory{
            .connectionId = {},
            .accountId = "account-1",
            .preferenceKind = "visibility",
            .objectId = "calendar-1",
            .beforeValue = "false",
            .afterValue = "true",
        };
        result.status = HistoryEntryStatus::Ready;
        return result;
    }
} // namespace

TEST_CASE("calendar preference undo and redo validate the current value",
          "[app][undo][calendar-preference]")
{
    FakePreferencePort port;
    port.current = "true";
    CalendarPreferenceExecutor executor{port};

    const auto undone = QCoro::waitFor(executor.execute(entry(), HistoryExecutionDirection::Undo));
    CHECK(undone.outcome == HistoryExecutionOutcome::Success);
    CHECK(port.current == std::optional<std::string>{"false"});

    const auto redone = QCoro::waitFor(executor.execute(entry(), HistoryExecutionDirection::Redo));
    CHECK(redone.outcome == HistoryExecutionOutcome::Success);
    CHECK(port.current == std::optional<std::string>{"true"});
    CHECK(port.writes == 2);
}

TEST_CASE("calendar preference history refuses a branched local value",
          "[app][undo][calendar-preference]")
{
    FakePreferencePort port;
    port.current = "external";
    CalendarPreferenceExecutor executor{port};

    const auto result = QCoro::waitFor(executor.execute(entry(), HistoryExecutionDirection::Undo));
    CHECK(result.outcome == HistoryExecutionOutcome::Conflict);
    CHECK(port.writes == 0);
}
