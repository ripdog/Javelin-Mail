#include "app/undo/MailTransferHistoryExecutor.h"

#include <QCoroTask>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace
{
    using namespace javelin::app::undo;

    [[nodiscard]] javelin::jmap::domain::Email
    email(std::string id, std::vector<std::string> mailboxIds,
          std::vector<std::string> keywords = {"$seen", "project"})
    {
        javelin::jmap::domain::Email value;
        value.id = std::move(id);
        value.mailboxIds = std::move(mailboxIds);
        value.keywords = std::move(keywords);
        value.subject = "Transfer test";
        return value;
    }

    class FakeMailTransferHistoryPort final : public MailTransferHistoryPort
    {
      public:
        std::unordered_map<std::string, std::vector<javelin::jmap::domain::Email>> emailsByAccount;
        std::vector<std::pair<std::string, javelin::jmap::EmailMailboxMutation>> mutations;
        std::vector<std::string> calls;
        std::optional<javelin::jmap::OperationError> readError;
        std::optional<javelin::jmap::OperationError> mutationError;
        std::optional<javelin::jmap::OperationError> recreateError;
        bool rejectMutation = false;
        RecreatedMailTransferSource recreated{
            .emailId = "source-recreated",
            .blobId = std::optional<std::string>{"source-blob-new"},
            .threadId = std::optional<std::string>{"source-thread-new"},
            .size = std::optional<std::uint64_t>{1234},
        };
        std::optional<QString> recreateHistoryEntryId;
        std::optional<std::string> recreateRawContentHash;

        QCoro::Task<javelin::jmap::AuthoritativeEmailsResult>
        getAuthoritativeEmails(std::string accountId, std::vector<std::string> emailIds) override
        {
            calls.push_back("get:" + accountId);
            if (readError.has_value())
                co_return *readError;

            javelin::jmap::AuthoritativeEmails result{
                .accountId = accountId,
                .state = "state-" + accountId,
                .emails = {},
                .notFound = {},
            };
            const auto found = emailsByAccount.find(accountId);
            for (const auto& emailId : emailIds)
            {
                if (found == emailsByAccount.end())
                {
                    result.notFound.push_back(emailId);
                    continue;
                }
                const auto current = std::ranges::find(found->second, emailId,
                                                       &javelin::jmap::domain::Email::id);
                if (current == found->second.end())
                    result.notFound.push_back(emailId);
                else
                    result.emails.push_back(*current);
            }
            co_return result;
        }

        QCoro::Task<javelin::jmap::SubmittedEmailMutationsResult>
        applyExactEmailMutation(std::string accountId,
                                javelin::jmap::EmailMailboxMutation mutation) override
        {
            calls.push_back("mutate:" + accountId);
            mutations.emplace_back(accountId, mutation);
            if (mutationError.has_value())
                co_return *mutationError;

            const bool accepted = !rejectMutation;
            co_return javelin::jmap::SubmittedEmailMutations{
                .accountId = std::move(accountId),
                .attemptedEmailCount = 1,
                .updatedEmailCount = accepted ? 1U : 0U,
                .failedEmailCount = accepted ? 0U : 1U,
                .statePreconditionUsed = true,
                .items = {{.emailId = mutation.emailId,
                           .mutationIds = {"history-mutation"},
                           .accepted = accepted,
                           .error = accepted ? std::nullopt
                                             : std::optional<std::string>{"rejected"}}},
                .receipt = {},
            };
        }

        QCoro::Task<RecreatedMailTransferSourceResult> recreateSourceFromHistory(
            QString historyEntryId, std::string accountId, std::string rawContentHash,
            std::vector<std::string>, std::vector<std::string>,
            std::optional<std::string>) override
        {
            calls.push_back("recreate:" + accountId);
            recreateHistoryEntryId = std::move(historyEntryId);
            recreateRawContentHash = std::move(rawContentHash);
            if (recreateError.has_value())
                co_return *recreateError;
            co_return recreated;
        }
    };

    [[nodiscard]] MailTransferItemHistory item(bool reused = false)
    {
        return {
            .currentSourceEmailId = std::optional<std::string>{"source-email"},
            .originalSourceMailboxIds = {"inbox"},
            .sourceKeywords = {"$seen", "project"},
            .sourceReceivedAt = std::optional<std::string>{"2026-08-15T00:00:00Z"},
            .sourceSize = 1234,
            .sourceRemovedMailboxIds = {"inbox"},
            .sourceDestroyed = false,
            .rawContentHash = std::nullopt,
            .currentDestinationEmailId = std::optional<std::string>{"destination-email"},
            .destinationReusedExisting = reused,
            .destinationPriorMailboxIds = reused ? std::vector<std::string>{"old-mailbox"}
                                                 : std::vector<std::string>{},
            .destinationMailboxIds = {"archive"},
            .destinationKeywords = {"$seen", "project"},
        };
    }

    [[nodiscard]] HistoryEntry entry(MailTransferHistoryOperation operation,
                                     MailTransferItemHistory historyItem)
    {
        HistoryEntry value;
        value.entryId = QStringLiteral("history-transfer-1");
        value.label = QStringLiteral("Move 1 message");
        value.domain = HistoryDomain::Mail;
        value.commandKind = QStringLiteral("mail_transfer");
        value.payload = MailTransferHistory{
            .sourceAccountId = "source-account",
            .destinationAccountId = "destination-account",
            .destinationMailboxId = "archive",
            .operation = operation,
            .items = {std::move(historyItem)},
        };
        value.status = HistoryEntryStatus::Ready;
        return value;
    }

    [[nodiscard]] const MailTransferHistory& updatedHistory(const HistoryExecutionResult& result)
    {
        REQUIRE(result.updatedPayload.has_value());
        REQUIRE(std::holds_alternative<MailTransferHistory>(*result.updatedPayload));
        return std::get<MailTransferHistory>(*result.updatedPayload);
    }

} // namespace

TEST_CASE("transfer copy undo destroys an untouched transfer-created destination",
          "[app][undo][mail-transfer]")
{
    FakeMailTransferHistoryPort port;
    port.emailsByAccount["destination-account"] = {
        email("destination-email", {"archive"}),
    };
    MailTransferHistoryExecutor executor{port};

    const auto result = QCoro::waitFor(
        executor.execute(entry(MailTransferHistoryOperation::Copy, item()),
                         HistoryExecutionDirection::Undo));
    CHECK(result.outcome == HistoryExecutionOutcome::Success);
    REQUIRE(port.mutations.size() == 1);
    CHECK(port.mutations.front().first == "destination-account");
    const auto& mutation = port.mutations.front().second;
    CHECK(mutation.emailId == "destination-email");
    CHECK(mutation.destroy);
    CHECK(mutation.authoritativeMailboxIds ==
          std::optional<std::vector<std::string>>{{"archive"}});
    CHECK(mutation.authoritativeKeywords ==
          std::optional<std::vector<std::string>>{{"$seen", "project"}});
    CHECK_FALSE(updatedHistory(result).items.front().currentDestinationEmailId.has_value());
}

TEST_CASE("transfer copy undo preserves a destination adopted into another mailbox",
          "[app][undo][mail-transfer][concurrency]")
{
    FakeMailTransferHistoryPort port;
    port.emailsByAccount["destination-account"] = {
        email("destination-email", {"archive", "important"}, {"$seen", "later-edit"}),
    };
    MailTransferHistoryExecutor executor{port};

    const auto result = QCoro::waitFor(
        executor.execute(entry(MailTransferHistoryOperation::Copy, item()),
                         HistoryExecutionDirection::Undo));
    CHECK(result.outcome == HistoryExecutionOutcome::Success);
    REQUIRE(port.mutations.size() == 1);
    const auto& mutation = port.mutations.front().second;
    CHECK_FALSE(mutation.destroy);
    CHECK(mutation.removeMailboxIds == std::vector<std::string>{"archive"});
    CHECK(mutation.addKeywords.empty());
    CHECK(mutation.removeKeywords.empty());
    CHECK(updatedHistory(result).items.front().currentDestinationEmailId ==
          std::optional<std::string>{"destination-email"});
}

TEST_CASE("transfer copy undo refuses to destroy destination after keyword changes",
          "[app][undo][mail-transfer][conflict]")
{
    FakeMailTransferHistoryPort port;
    port.emailsByAccount["destination-account"] = {
        email("destination-email", {"archive"}, {"$seen", "later-edit"}),
    };
    MailTransferHistoryExecutor executor{port};

    const auto result = QCoro::waitFor(
        executor.execute(entry(MailTransferHistoryOperation::Copy, item()),
                         HistoryExecutionDirection::Undo));
    CHECK(result.outcome == HistoryExecutionOutcome::Conflict);
    CHECK(port.mutations.empty());
}

TEST_CASE("transfer undo removes only membership added to a reused destination",
          "[app][undo][mail-transfer][duplicate]")
{
    FakeMailTransferHistoryPort port;
    port.emailsByAccount["destination-account"] = {
        email("destination-email", {"archive", "old-mailbox"}, {"destination-tag"}),
    };
    MailTransferHistoryExecutor executor{port};

    const auto result = QCoro::waitFor(
        executor.execute(entry(MailTransferHistoryOperation::Copy, item(true)),
                         HistoryExecutionDirection::Undo));
    CHECK(result.outcome == HistoryExecutionOutcome::Success);
    REQUIRE(port.mutations.size() == 1);
    const auto& mutation = port.mutations.front().second;
    CHECK_FALSE(mutation.destroy);
    CHECK(mutation.removeMailboxIds == std::vector<std::string>{"archive"});
    CHECK(mutation.authoritativeKeywords ==
          std::optional<std::vector<std::string>>{{"destination-tag"}});
}

TEST_CASE("transfer move undo restores source membership before reversing destination",
          "[app][undo][mail-transfer][move]")
{
    FakeMailTransferHistoryPort port;
    auto historyItem = item();
    historyItem.originalSourceMailboxIds = {"inbox", "important"};
    historyItem.sourceRemovedMailboxIds = {"inbox"};
    port.emailsByAccount["source-account"] = {
        email("source-email", {"important"}),
    };
    port.emailsByAccount["destination-account"] = {
        email("destination-email", {"archive"}),
    };
    MailTransferHistoryExecutor executor{port};

    const auto result = QCoro::waitFor(
        executor.execute(entry(MailTransferHistoryOperation::Move, std::move(historyItem)),
                         HistoryExecutionDirection::Undo));
    CHECK(result.outcome == HistoryExecutionOutcome::Success);
    REQUIRE(port.mutations.size() == 2);
    CHECK(port.mutations.at(0).first == "source-account");
    CHECK(port.mutations.at(0).second.addMailboxIds == std::vector<std::string>{"inbox"});
    CHECK(port.mutations.at(1).first == "destination-account");
    CHECK(port.mutations.at(1).second.destroy);
    REQUIRE(port.calls.size() >= 4);
    CHECK(port.calls.at(0) == "get:source-account");
    CHECK(port.calls.at(1) == "mutate:source-account");
    CHECK(port.calls.at(2) == "get:destination-account");
    CHECK(port.calls.at(3) == "mutate:destination-account");
}

TEST_CASE("destructive move undo recreates source before deleting destination",
          "[app][undo][mail-transfer][move][recreate]")
{
    FakeMailTransferHistoryPort port;
    auto historyItem = item();
    historyItem.currentSourceEmailId = std::nullopt;
    historyItem.sourceDestroyed = true;
    historyItem.rawContentHash = std::optional<std::string>{"raw-hash"};
    port.emailsByAccount["destination-account"] = {
        email("destination-email", {"archive"}),
    };
    MailTransferHistoryExecutor executor{port};

    const auto result = QCoro::waitFor(
        executor.execute(entry(MailTransferHistoryOperation::Move, std::move(historyItem)),
                         HistoryExecutionDirection::Undo));
    CHECK(result.outcome == HistoryExecutionOutcome::Success);
    REQUIRE(port.calls.size() >= 3);
    CHECK(port.calls.at(0) == "recreate:source-account");
    CHECK(port.calls.at(1) == "get:destination-account");
    CHECK(port.calls.at(2) == "mutate:destination-account");
    CHECK(port.recreateHistoryEntryId ==
          std::optional<QString>{QStringLiteral("history-transfer-1")});
    CHECK(port.recreateRawContentHash == std::optional<std::string>{"raw-hash"});
    const auto& history = updatedHistory(result);
    CHECK(history.items.front().currentSourceEmailId ==
          std::optional<std::string>{"source-recreated"});
    CHECK_FALSE(history.items.front().currentDestinationEmailId.has_value());
}

TEST_CASE("source recreation is retained in payload when destination reversal conflicts",
          "[app][undo][mail-transfer][move][partial]")
{
    FakeMailTransferHistoryPort port;
    auto historyItem = item();
    historyItem.currentSourceEmailId = std::nullopt;
    historyItem.sourceDestroyed = true;
    historyItem.rawContentHash = std::optional<std::string>{"raw-hash"};
    port.emailsByAccount["destination-account"] = {
        email("destination-email", {"archive"}, {"later-edit"}),
    };
    MailTransferHistoryExecutor executor{port};

    const auto result = QCoro::waitFor(
        executor.execute(entry(MailTransferHistoryOperation::Move, std::move(historyItem)),
                         HistoryExecutionDirection::Undo));
    CHECK(result.outcome == HistoryExecutionOutcome::PartialFailure);
    const auto& history = updatedHistory(result);
    CHECK(history.items.front().currentSourceEmailId ==
          std::optional<std::string>{"source-recreated"});
    CHECK(history.items.front().currentDestinationEmailId ==
          std::optional<std::string>{"destination-email"});
    CHECK(port.mutations.empty());
}
