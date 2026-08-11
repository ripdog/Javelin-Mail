#include "app/undo/MailHistoryExecutor.h"

#include <QCoroTask>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using namespace javelin::app::undo;

    class FakeMailHistoryPort final : public MailHistoryPort
    {
      public:
        javelin::jmap::AuthoritativeEmailsResult
        getEffectiveEmails(std::string_view, std::span<const std::string>) override
        {
            return authoritative;
        }

        javelin::jmap::AuthoritativeEmailsResult authoritative = javelin::jmap::AuthoritativeEmails{
            .accountId = "account-1",
            .state = "state-2",
            .emails = {},
            .notFound = {},
        };
        javelin::jmap::SubmittedEmailMutationsResult submitted =
            javelin::jmap::SubmittedEmailMutations{
                .accountId = "account-1",
                .attemptedEmailCount = 1,
                .updatedEmailCount = 1,
                .failedEmailCount = 0,
                .statePreconditionUsed = false,
                .items = {},
                .receipt = {},
            };
        std::vector<javelin::jmap::EmailMailboxMutation> queued;

        QCoro::Task<javelin::jmap::AuthoritativeEmailsResult>
        getAuthoritativeEmails(std::string, std::vector<std::string>) override
        {
            co_return authoritative;
        }

        javelin::jmap::QueuedEmailMutationResult
        queueExactEmailMutation(std::string accountId,
                                javelin::jmap::EmailMailboxMutation mutation) override
        {
            auto result = queueExactEmailMutations(std::move(accountId), {std::move(mutation)});
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                return *error;
            auto values =
                std::get<std::vector<javelin::jmap::QueuedEmailMutation>>(std::move(result));
            return std::move(values.front());
        }

        javelin::jmap::QueuedEmailMutationsResult queueExactEmailMutations(
            std::string accountId,
            std::vector<javelin::jmap::EmailMailboxMutation> mutations) override
        {
            std::vector<javelin::jmap::QueuedEmailMutation> values;
            values.reserve(mutations.size());
            for (std::size_t index = 0; index < mutations.size(); ++index)
            {
                queued.push_back(mutations[index]);
                values.push_back({
                    .mutationId = "inverse-" + std::to_string(index + 1),
                    .accountId = accountId,
                    .emailId = mutations[index].emailId,
                    .patch = std::move(mutations[index]),
                });
            }
            return values;
        }

        QCoro::Task<javelin::jmap::SubmittedEmailMutationsResult>
        submitPendingEmailMutations(std::string, std::optional<std::string>) override
        {
            co_return submitted;
        }
    };

    [[nodiscard]] javelin::jmap::domain::Email email(std::string id,
                                                     std::vector<std::string> mailboxIds)
    {
        javelin::jmap::domain::Email value;
        value.id = std::move(id);
        value.mailboxIds = std::move(mailboxIds);
        value.keywords = {"$seen", "project-label"};
        value.subject = "Quarterly report";
        return value;
    }

    [[nodiscard]] HistoryEntry moveEntry(std::vector<std::string> emailIds = {"email-1"})
    {
        MailPatchHistory history;
        for (auto& emailId : emailIds)
        {
            history.items.push_back({
                .accountId = "account-1",
                .emailId = std::move(emailId),
                .subject = "Quarterly report",
                .forward =
                    {
                        .addMailboxIds = {"archive"},
                        .removeMailboxIds = {"inbox"},
                        .addKeywords = {},
                        .removeKeywords = {},
                    },
                .inverse =
                    {
                        .addMailboxIds = {"inbox"},
                        .removeMailboxIds = {"archive"},
                        .addKeywords = {},
                        .removeKeywords = {},
                    },
                .expectedBefore =
                    {
                        .addMailboxIds = {"archive"},
                        .removeMailboxIds = {"inbox"},
                        .addKeywords = {},
                        .removeKeywords = {},
                    },
                .expectedAfter =
                    {
                        .addMailboxIds = {"inbox"},
                        .removeMailboxIds = {"archive"},
                        .addKeywords = {},
                        .removeKeywords = {},
                    },
                .mutationId = "forward-1",
            });
        }
        HistoryEntry entry;
        entry.entryId = QStringLiteral("history-1");
        entry.label = QStringLiteral("Move Message to Archive");
        entry.domain = HistoryDomain::Mail;
        entry.commandKind = QStringLiteral("mail_patch");
        entry.payload = std::move(history);
        entry.status = HistoryEntryStatus::Ready;
        return entry;
    }

} // namespace

TEST_CASE("mail history executor preflights and queues the exact inverse",
          "[app][undo][mail-executor]")
{
    FakeMailHistoryPort port;
    auto authoritative = std::get<javelin::jmap::AuthoritativeEmails>(port.authoritative);
    authoritative.emails = {email("email-1", {"archive", "unrelated"})};
    port.authoritative = std::move(authoritative);
    MailHistoryExecutor executor{port};

    const auto result =
        QCoro::waitFor(executor.execute(moveEntry(), HistoryExecutionDirection::Undo));
    CHECK(result.outcome == HistoryExecutionOutcome::Success);
    REQUIRE(port.queued.size() == 1);
    CHECK(port.queued.front().addMailboxIds == std::vector<std::string>{"inbox"});
    CHECK(port.queued.front().removeMailboxIds == std::vector<std::string>{"archive"});
    CHECK(port.queued.front().ifInState == std::optional<std::string>{"state-2"});
    CHECK(port.queued.front().authoritativeMailboxIds ==
          std::optional<std::vector<std::string>>{{"archive", "unrelated"}});
    CHECK(port.queued.front().authoritativeKeywords ==
          std::optional<std::vector<std::string>>{{"$seen", "project-label"}});
    CHECK(port.queued.front().operationGroupId.has_value());
}

TEST_CASE("mail history executor rejects the whole batch when one precondition changed",
          "[app][undo][mail-executor]")
{
    FakeMailHistoryPort port;
    auto authoritative = std::get<javelin::jmap::AuthoritativeEmails>(port.authoritative);
    authoritative.emails = {
        email("email-1", {"archive"}),
        email("email-2", {"inbox"}),
    };
    port.authoritative = std::move(authoritative);
    MailHistoryExecutor executor{port};

    const auto result = QCoro::waitFor(
        executor.execute(moveEntry({"email-1", "email-2"}), HistoryExecutionDirection::Undo));
    CHECK(result.outcome == HistoryExecutionOutcome::Conflict);
    REQUIRE(result.objectFailures.size() == 1);
    CHECK(result.objectFailures.front().objectId == QStringLiteral("email-2"));
    CHECK(port.queued.empty());
}

TEST_CASE("mail history redo validates the undone state and reapplies the forward patch",
          "[app][undo][mail-executor]")
{
    FakeMailHistoryPort port;
    auto authoritative = std::get<javelin::jmap::AuthoritativeEmails>(port.authoritative);
    authoritative.emails = {email("email-1", {"inbox", "unrelated"})};
    port.authoritative = std::move(authoritative);
    MailHistoryExecutor executor{port};

    const auto result =
        QCoro::waitFor(executor.execute(moveEntry(), HistoryExecutionDirection::Redo));
    CHECK(result.outcome == HistoryExecutionOutcome::Success);
    REQUIRE(port.queued.size() == 1);
    CHECK(port.queued.front().addMailboxIds == std::vector<std::string>{"archive"});
    CHECK(port.queued.front().removeMailboxIds == std::vector<std::string>{"inbox"});
}

TEST_CASE("mail history executor blocks mixed server outcomes as partial",
          "[app][undo][mail-executor]")
{
    FakeMailHistoryPort port;
    auto authoritative = std::get<javelin::jmap::AuthoritativeEmails>(port.authoritative);
    authoritative.emails = {
        email("email-1", {"archive"}),
        email("email-2", {"archive"}),
    };
    port.authoritative = std::move(authoritative);
    port.submitted = javelin::jmap::SubmittedEmailMutations{
        .accountId = "account-1",
        .attemptedEmailCount = 2,
        .updatedEmailCount = 1,
        .failedEmailCount = 1,
        .statePreconditionUsed = false,
        .items = {},
        .receipt = {},
    };
    MailHistoryExecutor executor{port};

    const auto result = QCoro::waitFor(
        executor.execute(moveEntry({"email-1", "email-2"}), HistoryExecutionDirection::Undo));
    CHECK(result.outcome == HistoryExecutionOutcome::PartialFailure);
    CHECK(result.updatedPayload.has_value());
}
