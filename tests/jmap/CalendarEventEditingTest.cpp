#include "jmap/calendar/CalendarEventEditing.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using javelin::jmap::calendar::Attendee;

namespace
{
    Attendee attendee(std::string id, std::string address, const bool owner = false)
    {
        return {.id = std::move(id),
                .name = address,
                .email = address,
                .calendarAddress = "mailto:" + address,
                .participationStatus = owner ? "accepted" : "tentative",
                .isOwner = owner,
                .isAttendee = true,
                .scheduleSequence = 7,
                .scheduleUpdated = std::nullopt};
    }
} // namespace

TEST_CASE("editable attendee addresses exclude the owner")
{
    const std::vector attendees{attendee("owner", "owner@example.test", true),
                                attendee("guest", "guest@example.test")};

    CHECK(javelin::jmap::calendar::editableAttendeeAddresses(attendees) ==
          std::vector<std::string>{"guest@example.test"});
}

TEST_CASE("attendee edits preserve matching scheduling records and owners")
{
    const std::vector existing{attendee("owner", "owner@example.test", true),
                               attendee("guest-id", "Guest@Example.test")};
    const std::vector<std::string> requested{"guest@example.test", "new@example.test",
                                             "new@example.test"};

    const auto result = javelin::jmap::calendar::reconcileEditableAttendees(existing, requested);

    REQUIRE(result.size() == 3);
    CHECK(result[0].id == "owner");
    CHECK(result[0].isOwner);
    CHECK(result[1].id == "guest-id");
    CHECK(result[1].participationStatus == "tentative");
    CHECK(result[1].scheduleSequence == 7);
    CHECK(result[2].id == "attendee-1");
    CHECK(result[2].email == std::optional<std::string>{"new@example.test"});
    CHECK(result[2].participationStatus == "needs-action");
}

TEST_CASE("removed editable attendees do not remove hidden participant records")
{
    auto hidden = attendee("resource", "room@example.test");
    hidden.isAttendee = false;
    const std::vector existing{hidden, attendee("guest", "guest@example.test")};
    const std::vector<std::string> requested;

    const auto result = javelin::jmap::calendar::reconcileEditableAttendees(existing, requested);

    REQUIRE(result.size() == 1);
    CHECK(result.front().id == "resource");
}
