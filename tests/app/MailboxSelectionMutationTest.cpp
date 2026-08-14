#include "app/MailboxSelectionMutation.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

namespace
{
    [[nodiscard]] javelin::jmap::cache::MailboxTreeItem
    mailbox(std::string id, std::string name, std::optional<std::string> role = std::nullopt,
            const bool mayAdd = true, const bool mayRemove = true, const bool maySetKeywords = true)
    {
        javelin::jmap::cache::MailboxTreeItem result;
        result.id = std::move(id);
        result.name = std::move(name);
        result.role = std::move(role);
        result.myRights.mayAddItems = mayAdd;
        result.myRights.mayRemoveItems = mayRemove;
        result.myRights.maySetKeywords = maySetKeywords;
        return result;
    }

    [[nodiscard]] javelin::jmap::domain::Email email(std::string id,
                                                     std::vector<std::string> mailboxIds,
                                                     std::vector<std::string> keywords = {})
    {
        javelin::jmap::domain::Email result;
        result.id = std::move(id);
        result.mailboxIds = std::move(mailboxIds);
        result.keywords = std::move(keywords);
        return result;
    }

    const std::vector<javelin::jmap::cache::MailboxTreeItem> mailboxes{
        mailbox("inbox", "Inbox", "inbox"),    mailbox("archive", "Archive", "archive"),
        mailbox("junk", "Junk", "junk"),       mailbox("projects", "Projects"),
        mailbox("destination", "Destination"),
    };
} // namespace

TEST_CASE("search move replaces all mailbox memberships", "[app][mailbox-mutation]")
{
    const javelin::app::MailboxSelectionMutationIntent intent{
        .accountId = "account-1",
        .selection = {javelin::app::SelectedEmail{.emailId = "email-1"}},
        .operation = javelin::app::MailboxSelectionOperation::Move,
        .sourceMailboxId = std::nullopt,
        .destinationMailboxId = "destination",
    };

    const auto result = javelin::app::planMailboxSelectionMutation(
        intent, {"email-1"}, {email("email-1", {"inbox", "projects"})}, mailboxes);
    REQUIRE(std::holds_alternative<javelin::app::PlannedMailboxSelectionMutation>(result));
    const auto& plan = std::get<javelin::app::PlannedMailboxSelectionMutation>(result);
    REQUIRE(plan.mutations.size() == 1);
    CHECK(plan.mutations.front().addMailboxIds == std::vector<std::string>{"destination"});
    CHECK(plan.mutations.front().removeMailboxIds == std::vector<std::string>{"inbox", "projects"});
}

TEST_CASE("search copy preserves existing memberships and skips existing destination",
          "[app][mailbox-mutation]")
{
    const javelin::app::MailboxSelectionMutationIntent intent{
        .accountId = "account-1",
        .selection = {javelin::app::SelectedEmail{.emailId = "email-1"},
                      javelin::app::SelectedEmail{.emailId = "email-2"}},
        .operation = javelin::app::MailboxSelectionOperation::Copy,
        .sourceMailboxId = std::nullopt,
        .destinationMailboxId = "destination",
    };

    const auto result = javelin::app::planMailboxSelectionMutation(
        intent, {"email-1", "email-2"},
        {email("email-1", {"inbox"}), email("email-2", {"inbox", "destination"})}, mailboxes);
    REQUIRE(std::holds_alternative<javelin::app::PlannedMailboxSelectionMutation>(result));
    const auto& plan = std::get<javelin::app::PlannedMailboxSelectionMutation>(result);
    REQUIRE(plan.mutations.size() == 1);
    CHECK(plan.mutations.front().emailId == "email-1");
    CHECK(plan.mutations.front().addMailboxIds == std::vector<std::string>{"destination"});
    CHECK(plan.mutations.front().removeMailboxIds.empty());
    CHECK(plan.skippedEmailCount == 1);
}

TEST_CASE("search archive removes Inbox and preserves other memberships", "[app][mailbox-mutation]")
{
    const javelin::app::MailboxSelectionMutationIntent intent{
        .accountId = "account-1",
        .selection = {javelin::app::SelectedEmail{.emailId = "email-1"},
                      javelin::app::SelectedEmail{.emailId = "email-2"}},
        .operation = javelin::app::MailboxSelectionOperation::Archive,
        .sourceMailboxId = std::nullopt,
        .destinationMailboxId = std::nullopt,
    };

    const auto result = javelin::app::planMailboxSelectionMutation(
        intent, {"email-1", "email-2"},
        {email("email-1", {"inbox", "projects"}), email("email-2", {"projects"})}, mailboxes);
    REQUIRE(std::holds_alternative<javelin::app::PlannedMailboxSelectionMutation>(result));
    const auto& plan = std::get<javelin::app::PlannedMailboxSelectionMutation>(result);
    REQUIRE(plan.mutations.size() == 1);
    CHECK(plan.mutations.front().addMailboxIds == std::vector<std::string>{"archive"});
    CHECK(plan.mutations.front().removeMailboxIds == std::vector<std::string>{"inbox"});
    CHECK(plan.skippedEmailCount == 1);
}

TEST_CASE("mailbox move removes only its explicit source", "[app][mailbox-mutation]")
{
    const javelin::app::MailboxSelectionMutationIntent intent{
        .accountId = "account-1",
        .selection = {javelin::app::SelectedEmail{.emailId = "email-1"}},
        .operation = javelin::app::MailboxSelectionOperation::Move,
        .sourceMailboxId = "inbox",
        .destinationMailboxId = "destination",
    };

    const auto result = javelin::app::planMailboxSelectionMutation(
        intent, {"email-1"}, {email("email-1", {"inbox", "projects"})}, mailboxes);
    REQUIRE(std::holds_alternative<javelin::app::PlannedMailboxSelectionMutation>(result));
    const auto& plan = std::get<javelin::app::PlannedMailboxSelectionMutation>(result);
    REQUIRE(plan.mutations.size() == 1);
    CHECK(plan.mutations.front().removeMailboxIds == std::vector<std::string>{"inbox"});
}

TEST_CASE("mailbox move uses an explicit Email's resident mailboxes when view context differs",
          "[app][mailbox-mutation][thread]")
{
    const javelin::app::MailboxSelectionMutationIntent intent{
        .accountId = "account-1",
        .selection = {javelin::app::SelectedEmail{.emailId = "archived-child"}},
        .operation = javelin::app::MailboxSelectionOperation::Move,
        .sourceMailboxId = "inbox",
        .destinationMailboxId = "projects",
    };

    const auto result = javelin::app::planMailboxSelectionMutation(
        intent, {"archived-child"}, {email("archived-child", {"archive", "destination"})},
        mailboxes);
    REQUIRE(std::holds_alternative<javelin::app::PlannedMailboxSelectionMutation>(result));
    const auto& plan = std::get<javelin::app::PlannedMailboxSelectionMutation>(result);
    REQUIRE(plan.mutations.size() == 1);
    CHECK(plan.mutations.front().addMailboxIds == std::vector<std::string>{"projects"});
    CHECK(plan.mutations.front().removeMailboxIds ==
          std::vector<std::string>{"archive", "destination"});
}

TEST_CASE("mixed-residency copies can target either selected mailbox",
          "[app][mailbox-mutation][thread]")
{
    const auto selectedEmails =
        std::vector{email("inbox-email", {"inbox"}), email("archive-email", {"archive"})};

    for (const auto& destination : std::vector<std::string>{"inbox", "archive"})
    {
        const javelin::app::MailboxSelectionMutationIntent intent{
            .accountId = "account-1",
            .selection = {javelin::app::SelectedEmail{.emailId = "inbox-email"},
                          javelin::app::SelectedEmail{.emailId = "archive-email"}},
            .operation = javelin::app::MailboxSelectionOperation::Copy,
            .sourceMailboxId = "inbox",
            .destinationMailboxId = destination,
        };

        const auto result = javelin::app::planMailboxSelectionMutation(
            intent, {"inbox-email", "archive-email"}, selectedEmails, mailboxes);
        REQUIRE(std::holds_alternative<javelin::app::PlannedMailboxSelectionMutation>(result));
        const auto& plan = std::get<javelin::app::PlannedMailboxSelectionMutation>(result);
        REQUIRE(plan.mutations.size() == 1);
        CHECK(plan.mutations.front().emailId ==
              (destination == "inbox" ? "archive-email" : "inbox-email"));
        CHECK(plan.mutations.front().addMailboxIds == std::vector<std::string>{destination});
        CHECK(plan.mutations.front().removeMailboxIds.empty());
        CHECK(plan.skippedEmailCount == 1);
    }
}

TEST_CASE("search move applies per-Email residency across a mixed selection",
          "[app][mailbox-mutation][search]")
{
    const javelin::app::MailboxSelectionMutationIntent intent{
        .accountId = "account-1",
        .selection = {javelin::app::SelectedEmail{.emailId = "inbox-email"},
                      javelin::app::SelectedEmail{.emailId = "archive-email"}},
        .operation = javelin::app::MailboxSelectionOperation::Move,
        .sourceMailboxId = std::nullopt,
        .destinationMailboxId = "inbox",
    };

    const auto result = javelin::app::planMailboxSelectionMutation(
        intent, {"inbox-email", "archive-email"},
        {email("inbox-email", {"inbox"}), email("archive-email", {"archive"})}, mailboxes);
    REQUIRE(std::holds_alternative<javelin::app::PlannedMailboxSelectionMutation>(result));
    const auto& plan = std::get<javelin::app::PlannedMailboxSelectionMutation>(result);
    REQUIRE(plan.mutations.size() == 1);
    CHECK(plan.mutations.front().emailId == "archive-email");
    CHECK(plan.mutations.front().addMailboxIds == std::vector<std::string>{"inbox"});
    CHECK(plan.mutations.front().removeMailboxIds == std::vector<std::string>{"archive"});
    CHECK(plan.skippedEmailCount == 1);
}

TEST_CASE("junk move uses an explicit Email's resident mailbox when view context differs",
          "[app][mailbox-mutation][junk][thread]")
{
    const javelin::app::MailboxSelectionMutationIntent intent{
        .accountId = "account-1",
        .selection = {javelin::app::SelectedEmail{.emailId = "archived-child"}},
        .operation = javelin::app::MailboxSelectionOperation::Junk,
        .sourceMailboxId = "inbox",
        .destinationMailboxId = std::nullopt,
    };

    const auto result = javelin::app::planMailboxSelectionMutation(
        intent, {"archived-child"}, {email("archived-child", {"archive"})}, mailboxes);
    REQUIRE(std::holds_alternative<javelin::app::PlannedMailboxSelectionMutation>(result));
    const auto& plan = std::get<javelin::app::PlannedMailboxSelectionMutation>(result);
    REQUIRE(plan.mutations.size() == 1);
    CHECK(plan.mutations.front().addMailboxIds == std::vector<std::string>{"junk"});
    CHECK(plan.mutations.front().removeMailboxIds == std::vector<std::string>{"archive"});
}

TEST_CASE("mailbox mutation planning rejects insufficient rights before queuing",
          "[app][mailbox-mutation]")
{
    auto restrictedMailboxes = mailboxes;
    restrictedMailboxes.back().myRights.mayAddItems = false;
    const javelin::app::MailboxSelectionMutationIntent intent{
        .accountId = "account-1",
        .selection = {javelin::app::SelectedEmail{.emailId = "email-1"}},
        .operation = javelin::app::MailboxSelectionOperation::Copy,
        .sourceMailboxId = std::nullopt,
        .destinationMailboxId = "destination",
    };

    const auto result = javelin::app::planMailboxSelectionMutation(
        intent, {"email-1"}, {email("email-1", {"inbox"})}, restrictedMailboxes);
    REQUIRE(std::holds_alternative<QString>(result));
    CHECK(std::get<QString>(result).contains(QStringLiteral("permission")));
}

TEST_CASE("junk classification moves to Junk and replaces the not-junk keyword",
          "[app][mailbox-mutation][junk]")
{
    const javelin::app::MailboxSelectionMutationIntent intent{
        .accountId = "account-1",
        .selection = {javelin::app::SelectedEmail{.emailId = "email-1"}},
        .operation = javelin::app::MailboxSelectionOperation::Junk,
        .sourceMailboxId = "inbox",
        .destinationMailboxId = std::nullopt,
    };

    const auto result = javelin::app::planMailboxSelectionMutation(
        intent, {"email-1"}, {email("email-1", {"inbox"}, {"$notjunk"})}, mailboxes);
    REQUIRE(std::holds_alternative<javelin::app::PlannedMailboxSelectionMutation>(result));
    const auto& plan = std::get<javelin::app::PlannedMailboxSelectionMutation>(result);
    REQUIRE(plan.mutations.size() == 1);
    CHECK(plan.mutations.front().addMailboxIds == std::vector<std::string>{"junk"});
    CHECK(plan.mutations.front().removeMailboxIds == std::vector<std::string>{"inbox"});
    CHECK(plan.mutations.front().addKeywords == std::vector<std::string>{"$junk"});
    CHECK(plan.mutations.front().removeKeywords == std::vector<std::string>{"$notjunk"});
}

TEST_CASE("not-junk classification returns to Inbox and preserves unrelated memberships",
          "[app][mailbox-mutation][junk]")
{
    const javelin::app::MailboxSelectionMutationIntent intent{
        .accountId = "account-1",
        .selection = {javelin::app::SelectedEmail{.emailId = "email-1"}},
        .operation = javelin::app::MailboxSelectionOperation::NotJunk,
        .sourceMailboxId = "junk",
        .destinationMailboxId = std::nullopt,
    };

    const auto result = javelin::app::planMailboxSelectionMutation(
        intent, {"email-1"}, {email("email-1", {"junk", "projects"}, {"$junk"})}, mailboxes);
    REQUIRE(std::holds_alternative<javelin::app::PlannedMailboxSelectionMutation>(result));
    const auto& plan = std::get<javelin::app::PlannedMailboxSelectionMutation>(result);
    REQUIRE(plan.mutations.size() == 1);
    CHECK(plan.mutations.front().addMailboxIds == std::vector<std::string>{"inbox"});
    CHECK(plan.mutations.front().removeMailboxIds == std::vector<std::string>{"junk"});
    CHECK(plan.mutations.front().addKeywords == std::vector<std::string>{"$notjunk"});
    CHECK(plan.mutations.front().removeKeywords == std::vector<std::string>{"$junk"});
}

TEST_CASE("not-junk classification does not require the junk keyword",
          "[app][mailbox-mutation][junk]")
{
    const javelin::app::MailboxSelectionMutationIntent intent{
        .accountId = "account-1",
        .selection = {javelin::app::SelectedEmail{.emailId = "email-1"}},
        .operation = javelin::app::MailboxSelectionOperation::NotJunk,
        .sourceMailboxId = "junk",
        .destinationMailboxId = std::nullopt,
    };

    const auto result = javelin::app::planMailboxSelectionMutation(
        intent, {"email-1"}, {email("email-1", {"junk"})}, mailboxes);
    REQUIRE(std::holds_alternative<javelin::app::PlannedMailboxSelectionMutation>(result));
    const auto& plan = std::get<javelin::app::PlannedMailboxSelectionMutation>(result);
    REQUIRE(plan.mutations.size() == 1);
    CHECK(plan.mutations.front().addMailboxIds == std::vector<std::string>{"inbox"});
    CHECK(plan.mutations.front().removeMailboxIds == std::vector<std::string>{"junk"});
    CHECK(plan.mutations.front().addKeywords == std::vector<std::string>{"$notjunk"});
    CHECK(plan.mutations.front().removeKeywords.empty());
}

TEST_CASE("junk classification preflights keyword rights for every current mailbox",
          "[app][mailbox-mutation][junk]")
{
    auto restrictedMailboxes = mailboxes;
    restrictedMailboxes.front().myRights.maySetKeywords = false;
    const javelin::app::MailboxSelectionMutationIntent intent{
        .accountId = "account-1",
        .selection = {javelin::app::SelectedEmail{.emailId = "email-1"}},
        .operation = javelin::app::MailboxSelectionOperation::Junk,
        .sourceMailboxId = "inbox",
        .destinationMailboxId = std::nullopt,
    };

    const auto result = javelin::app::planMailboxSelectionMutation(
        intent, {"email-1"}, {email("email-1", {"inbox"})}, restrictedMailboxes);
    REQUIRE(std::holds_alternative<QString>(result));
    CHECK(std::get<QString>(result).contains(QStringLiteral("keywords")));
}

TEST_CASE("search move preflights removal rights for every membership", "[app][mailbox-mutation]")
{
    auto restrictedMailboxes = mailboxes;
    restrictedMailboxes.front().myRights.mayRemoveItems = false;
    const javelin::app::MailboxSelectionMutationIntent intent{
        .accountId = "account-1",
        .selection = {javelin::app::SelectedEmail{.emailId = "email-1"},
                      javelin::app::SelectedEmail{.emailId = "email-2"}},
        .operation = javelin::app::MailboxSelectionOperation::Move,
        .sourceMailboxId = std::nullopt,
        .destinationMailboxId = "destination",
    };

    const auto result = javelin::app::planMailboxSelectionMutation(
        intent, {"email-1", "email-2"},
        {email("email-1", {"projects"}), email("email-2", {"inbox"})}, restrictedMailboxes);
    REQUIRE(std::holds_alternative<QString>(result));
    CHECK(std::get<QString>(result).contains(QStringLiteral("Inbox")));
}
