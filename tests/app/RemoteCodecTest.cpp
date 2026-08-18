#include "app/RemoteCodec.h"
#include "app/DeveloperDiagnostics.h"
#include "app/DeveloperMaintenance.h"
#include "app/MailTransferCommandService.h"
#include "app/RemoteActionTypes.h"
#include "jmap/calendar/CalendarCommandTypes.h"
#include "jmap/calendar/CalendarTypes.h"
#include "jmap/identity/IdentityCommandTypes.h"
#include "jmap/submission/ComposeTypes.h"
#include "jmap/sync/MailboxMutationEngine.h"
#include "protocol/actions/ActionCatalog.h"
#include "protocol/actions/CalendarActions.h"
#include "protocol/actions/MailActions.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace
{
    struct CodecFixture
    {
        QString title;
        QByteArray bytes;
        std::vector<std::string> identifiers;
        std::map<std::string, std::optional<std::size_t>> totals;
        std::variant<int, QString> status;
        std::chrono::milliseconds delay{};
    };

    struct OrderedFixture
    {
        int sequence = 0;
        QString label;
    };

    struct ReorderedFixture
    {
        QString label;
        int sequence = 0;
    };
} // namespace

TEST_CASE("remote codec round-trips nested typed values", "[app][remote-codec]")
{
    const CodecFixture fixture{
        .title = QStringLiteral("Mailbox window"),
        .bytes = QByteArray{"binary\0payload", 14},
        .identifiers = {"account-1", "mailbox-1"},
        .totals = {{"known", 42}, {"unknown", std::nullopt}},
        .status = QStringLiteral("ready"),
        .delay = std::chrono::milliseconds{275},
    };

    const auto encoded = javelin::app::remote::encode(fixture);
    const auto* payload = std::get_if<QByteArray>(&encoded);
    REQUIRE(payload != nullptr);

    const auto decoded = javelin::app::remote::decodeValue<CodecFixture>(*payload);
    const auto* value = std::get_if<CodecFixture>(&decoded);
    REQUIRE(value != nullptr);
    CHECK(value->title == fixture.title);
    CHECK(value->bytes == fixture.bytes);
    CHECK(value->identifiers == fixture.identifiers);
    CHECK(value->totals == fixture.totals);
    CHECK(value->status == fixture.status);
    CHECK(value->delay == fixture.delay);
}

TEST_CASE("remote codec aggregate fields are keyed by name rather than source order",
          "[app][remote-codec][schema]")
{
    const auto encoded = javelin::app::remote::encode(
        OrderedFixture{.sequence = 7, .label = QStringLiteral("named")});
    const auto* payload = std::get_if<QByteArray>(&encoded);
    REQUIRE(payload != nullptr);

    const auto decoded = javelin::app::remote::decodeValue<ReorderedFixture>(*payload);
    const auto* value = std::get_if<ReorderedFixture>(&decoded);
    REQUIRE(value != nullptr);
    CHECK(value->sequence == 7);
    CHECK(value->label == QStringLiteral("named"));
}

TEST_CASE("remote codec round-trips cross-account mail transfer action payloads",
          "[app][remote-codec][mail-transfer]")
{
    using Action = javelin::protocol::actions::MailTransferAcrossAccounts;
    const javelin::app::CrossAccountMailTransferIntent intent{
        .sourceAccountId = "local-source",
        .sourceMailboxId = std::optional<std::string>{"inbox"},
        .destinationAccountId = "local-destination",
        .destinationMailboxId = "archive",
        .operation = javelin::app::MailTransferOperation::Move,
        .selection = {javelin::app::SelectedEmail{.emailId = "email-1"},
                      javelin::app::SelectedCollapsedThread{.threadId = "thread-2"}},
    };
    const auto encoded =
        javelin::app::remote::encodeVersioned<Action::requestSchemaVersion>(intent);
    const auto* payload = std::get_if<QByteArray>(&encoded);
    REQUIRE(payload != nullptr);
    const auto decoded = javelin::app::remote::decodeVersionedValue<
        Action::requestSchemaVersion, javelin::app::CrossAccountMailTransferIntent>(*payload);
    const auto* value = std::get_if<javelin::app::CrossAccountMailTransferIntent>(&decoded);
    REQUIRE(value != nullptr);
    CHECK(value->sourceAccountId == intent.sourceAccountId);
    CHECK(value->sourceMailboxId == intent.sourceMailboxId);
    CHECK(value->destinationAccountId == intent.destinationAccountId);
    CHECK(value->destinationMailboxId == intent.destinationMailboxId);
    CHECK(value->operation == intent.operation);
    REQUIRE(value->selection.size() == 2);
    REQUIRE(std::holds_alternative<javelin::app::SelectedEmail>(value->selection.at(0)));
    CHECK(std::get<javelin::app::SelectedEmail>(value->selection.at(0)).emailId == "email-1");
    REQUIRE(std::holds_alternative<javelin::app::SelectedCollapsedThread>(value->selection.at(1)));
    CHECK(std::get<javelin::app::SelectedCollapsedThread>(value->selection.at(1)).threadId ==
          "thread-2");

    javelin::app::MailTransferExecutionResult result = javelin::app::MailTransferExecutionSummary{
        .operationId = "transfer-operation",
        .status = javelin::app::MailTransferStatus::Partial,
        .completeItemCount = 3,
        .destinationConfirmedItemCount = 1,
        .failedItemCount = 1,
        .partialItemCount = 1,
        .unknownItemCount = 0,
        .historyEntryId = std::optional<QString>{QStringLiteral("history-1")},
    };
    const auto resultEncoded =
        javelin::app::remote::encodeVersioned<Action::resultSchemaVersion>(result);
    const auto* resultPayload = std::get_if<QByteArray>(&resultEncoded);
    REQUIRE(resultPayload != nullptr);
    const auto resultDecoded = javelin::app::remote::decodeVersionedValue<
        Action::resultSchemaVersion, javelin::app::MailTransferExecutionResult>(*resultPayload);
    const auto* decodedResult =
        std::get_if<javelin::app::MailTransferExecutionResult>(&resultDecoded);
    REQUIRE(decodedResult != nullptr);
    REQUIRE(std::holds_alternative<javelin::app::MailTransferExecutionSummary>(*decodedResult));
    const auto& summary = std::get<javelin::app::MailTransferExecutionSummary>(*decodedResult);
    CHECK(summary.operationId == "transfer-operation");
    CHECK(summary.status == javelin::app::MailTransferStatus::Partial);
    CHECK(summary.completeItemCount == 3);
    CHECK(summary.destinationConfirmedItemCount == 1);
    CHECK(summary.failedItemCount == 1);
    CHECK(summary.partialItemCount == 1);
    CHECK(summary.unknownItemCount == 0);
    CHECK(summary.historyEntryId == std::optional<QString>{QStringLiteral("history-1")});

    const auto metadata = javelin::protocol::actions::findActionMetadata(Action::id);
    REQUIRE(metadata.has_value());
    CHECK(metadata->id.value == 94);
    CHECK(metadata->name == "MailTransferAcrossAccounts");
    CHECK(metadata->admission == javelin::protocol::actions::AdmissionSemantics::Asynchronous);
    CHECK(metadata->replay == javelin::protocol::actions::ReplayPolicy::Never);
}

TEST_CASE("remote codec preserves calendar delete range materialization",
          "[app][remote-codec][calendar]")
{
    using Action = javelin::protocol::actions::CalendarDeleteEvent;
    const javelin::jmap::calendar::DeleteEventCommand command{
        .accountId = "calendar-account",
        .eventId = "event-1",
        .calendarIds = {"work"},
        .operationGroupId = std::optional<std::string>{"operation-1"},
        .ifInState = std::optional<std::string>{"event-state"},
        .materialization =
            javelin::jmap::calendar::CalendarRangeMaterialization{
                .interval = {.start = {.value = "2026-08-01T00:00:00"},
                             .end = {.value = "2026-09-01T00:00:00"}},
                .displayTimeZone = {.value = "Pacific/Auckland"},
            },
    };

    const auto encoded = javelin::app::remote::encodeVersioned<Action::requestSchemaVersion>(
        std::string{"owner-account"}, command, javelin::app::undo::CommandOrigin::User);
    const auto* payload = std::get_if<QByteArray>(&encoded);
    REQUIRE(payload != nullptr);
    const auto decoded =
        javelin::app::remote::decodeVersionedValue<Action::requestSchemaVersion, Action::Request>(
            *payload);
    const auto* value = std::get_if<Action::Request>(&decoded);
    REQUIRE(value != nullptr);
    CHECK(std::get<0>(*value) == "owner-account");
    const auto& decodedCommand = std::get<1>(*value);
    CHECK(decodedCommand.accountId == command.accountId);
    CHECK(decodedCommand.eventId == command.eventId);
    CHECK(decodedCommand.calendarIds == command.calendarIds);
    CHECK(decodedCommand.operationGroupId == command.operationGroupId);
    CHECK(decodedCommand.ifInState == command.ifInState);
    REQUIRE(decodedCommand.materialization.has_value());
    CHECK(decodedCommand.materialization->interval.start.value == "2026-08-01T00:00:00");
    CHECK(decodedCommand.materialization->interval.end.value == "2026-09-01T00:00:00");
    CHECK(decodedCommand.materialization->displayTimeZone.value == "Pacific/Auckland");
    CHECK(std::get<2>(*value) == javelin::app::undo::CommandOrigin::User);
}

TEST_CASE("remote codec rejects unsupported action schema versions", "[app][remote-codec][schema]")
{
    const auto encoded = javelin::app::remote::encodeVersioned<2>(42);
    const auto* payload = std::get_if<QByteArray>(&encoded);
    REQUIRE(payload != nullptr);

    const auto decoded = javelin::app::remote::decodeVersionedValue<1, int>(*payload);
    REQUIRE(std::holds_alternative<javelin::app::remote::CodecError>(decoded));
}

TEST_CASE("remote codec rejects truncated named aggregates", "[app][remote-codec][schema]")
{
    const auto encoded = javelin::app::remote::encode(
        OrderedFixture{.sequence = 7, .label = QStringLiteral("named")});
    const auto* payload = std::get_if<QByteArray>(&encoded);
    REQUIRE(payload != nullptr);
    auto truncated = *payload;
    truncated.chop(1);

    const auto decoded = javelin::app::remote::decodeValue<OrderedFixture>(truncated);
    REQUIRE(std::holds_alternative<javelin::app::remote::CodecError>(decoded));
}

TEST_CASE("remote codec named aggregates reject every truncated prefix",
          "[app][remote-codec][schema][property]")
{
    for (int iteration = 0; iteration < 32; ++iteration)
    {
        const OrderedFixture fixture{
            .sequence = iteration * 7919 - 17,
            .label = QStringLiteral("fixture-%1-%2").arg(iteration).arg(iteration * iteration),
        };
        const auto encoded = javelin::app::remote::encode(fixture);
        const auto* payload = std::get_if<QByteArray>(&encoded);
        REQUIRE(payload != nullptr);

        const auto roundTrip = javelin::app::remote::decodeValue<OrderedFixture>(*payload);
        const auto* decoded = std::get_if<OrderedFixture>(&roundTrip);
        REQUIRE(decoded != nullptr);
        CHECK(decoded->sequence == fixture.sequence);
        CHECK(decoded->label == fixture.label);

        for (qsizetype length = 0; length < payload->size(); ++length)
        {
            const auto truncated =
                javelin::app::remote::decodeValue<OrderedFixture>(payload->left(length));
            CHECK(std::holds_alternative<javelin::app::remote::CodecError>(truncated));
        }
    }
}

TEST_CASE("remote codec preserves mailbox visibility changes", "[app][remote-codec][mailbox]")
{
    const javelin::jmap::MailboxSubscriptionChange change{
        .accountId = "account-1",
        .mailboxId = "mailbox-1",
        .subscribed = false,
    };

    const auto encoded = javelin::app::remote::encode(change);
    const auto* payload = std::get_if<QByteArray>(&encoded);
    REQUIRE(payload != nullptr);
    const auto decoded =
        javelin::app::remote::decodeValue<javelin::jmap::MailboxSubscriptionChange>(*payload);
    const auto* value = std::get_if<javelin::jmap::MailboxSubscriptionChange>(&decoded);
    REQUIRE(value != nullptr);
    CHECK(value->accountId == "account-1");
    CHECK(value->mailboxId == "mailbox-1");
    CHECK_FALSE(value->subscribed);
}

TEST_CASE("remote codec preserves scheduled send instants", "[app][remote-codec][compose]")
{
    const auto sendAt = std::chrono::system_clock::time_point{std::chrono::seconds{1786325400}};
    const javelin::jmap::submission::ScheduledSendRequest request{
        .snapshot =
            {
                .composeSessionId = "compose-1",
                .accountId = "account-1",
                .revision = 1,
                .draftEmailId = std::nullopt,
                .mode = javelin::jmap::submission::ComposeMode::NewMessage,
                .editorMode = javelin::jmap::submission::BodyEditorMode::PlainText,
                .identityId = "identity-1",
                .to = {{.name = std::nullopt, .email = "recipient@example.test"}},
                .cc = {},
                .bcc = {},
                .subject = "Scheduled",
                .plainTextBody = "Body",
                .htmlBody = {},
                .threading = {},
                .attachments = {},
            },
        .sendAt = sendAt,
    };

    const auto encoded = javelin::app::remote::encode(request);
    const auto* payload = std::get_if<QByteArray>(&encoded);
    REQUIRE(payload != nullptr);
    const auto decoded =
        javelin::app::remote::decodeValue<javelin::jmap::submission::ScheduledSendRequest>(
            *payload);
    const auto* value = std::get_if<javelin::jmap::submission::ScheduledSendRequest>(&decoded);
    REQUIRE(value != nullptr);
    CHECK(value->sendAt == sendAt);
    CHECK(value->snapshot.composeSessionId == "compose-1");
    CHECK(value->snapshot.accountId == "account-1");
}

TEST_CASE("remote codec preserves calendar participant roles", "[app][remote-codec][calendar]")
{
    const javelin::jmap::calendar::Attendee attendee{
        .id = "participant-1",
        .name = "Alice",
        .email = "alice@example.test",
        .calendarAddress = "mailto:alice@example.test",
        .participationStatus = "accepted",
        .isOwner = false,
        .isAttendee = true,
        .roles = {{"chair", true}, {"x-example-role", true}},
        .expectReply = false,
        .scheduleSequence = 4,
        .scheduleUpdated = std::nullopt,
    };

    const auto encoded = javelin::app::remote::encode(attendee);
    const auto* payload = std::get_if<QByteArray>(&encoded);
    REQUIRE(payload != nullptr);

    const auto decoded =
        javelin::app::remote::decodeValue<javelin::jmap::calendar::Attendee>(*payload);
    const auto* value = std::get_if<javelin::jmap::calendar::Attendee>(&decoded);
    REQUIRE(value != nullptr);
    CHECK(*value == attendee);
}

TEST_CASE("remote codec preserves structured undo failures", "[app][remote-codec][undo]")
{
    const javelin::app::RemoteUndoExecutionResult result{
        .succeeded = false,
        .completedEntryId = std::nullopt,
        .refreshScope = std::nullopt,
        .failure =
            javelin::app::undo::HistoryFailure{
                .entryId = QStringLiteral("history-1"),
                .actionLabel = QStringLiteral("Move message"),
                .summary = QStringLiteral("The message is no longer in Archive."),
                .objectFailures = {{.objectId = QStringLiteral("email-1"),
                                    .summary = QStringLiteral("Object not found")}},
                .mayRemoveFromHistory = true,
                .acknowledgeAndRemove = true,
            },
    };

    const auto encoded = javelin::app::remote::encode(result);
    const auto* payload = std::get_if<QByteArray>(&encoded);
    REQUIRE(payload != nullptr);

    const auto decoded =
        javelin::app::remote::decodeValue<javelin::app::RemoteUndoExecutionResult>(*payload);
    const auto* value = std::get_if<javelin::app::RemoteUndoExecutionResult>(&decoded);
    REQUIRE(value != nullptr);
    CHECK_FALSE(value->succeeded);
    REQUIRE(value->failure.has_value());
    CHECK(value->failure->entryId == QStringLiteral("history-1"));
    CHECK(value->failure->summary == QStringLiteral("The message is no longer in Archive."));
    REQUIRE(value->failure->objectFailures.size() == 1);
    CHECK(value->failure->objectFailures.front().objectId == QStringLiteral("email-1"));
    CHECK(value->failure->acknowledgeAndRemove);
}

TEST_CASE("remote codec round-trips mailbox create results", "[app][remote-codec][mailbox]")
{
    const javelin::jmap::MailboxCreateResult result{javelin::jmap::MailboxCreateChange{
        .accountId = "account-1", .mailboxId = "mailbox-1", .name = "Projects"}};

    const auto encoded = javelin::app::remote::encode(result);
    const auto* payload = std::get_if<QByteArray>(&encoded);
    REQUIRE(payload != nullptr);

    const auto decoded =
        javelin::app::remote::decodeValue<javelin::jmap::MailboxCreateResult>(*payload);
    const auto* value = std::get_if<javelin::jmap::MailboxCreateResult>(&decoded);
    REQUIRE(value != nullptr);
    const auto* change = std::get_if<javelin::jmap::MailboxCreateChange>(value);
    REQUIRE(change != nullptr);
    CHECK(change->accountId == "account-1");
    CHECK(change->mailboxId == "mailbox-1");
    CHECK(change->name == "Projects");
}

TEST_CASE("remote codec round-trips mailbox destroy results", "[app][remote-codec][mailbox]")
{
    const javelin::jmap::MailboxDestroyResult result{
        javelin::jmap::MailboxDestroyChange{.accountId = "account-1", .mailboxId = "mailbox-1"}};

    const auto encoded = javelin::app::remote::encode(result);
    const auto* payload = std::get_if<QByteArray>(&encoded);
    REQUIRE(payload != nullptr);

    const auto decoded =
        javelin::app::remote::decodeValue<javelin::jmap::MailboxDestroyResult>(*payload);
    const auto* value = std::get_if<javelin::jmap::MailboxDestroyResult>(&decoded);
    REQUIRE(value != nullptr);
    const auto* change = std::get_if<javelin::jmap::MailboxDestroyChange>(value);
    REQUIRE(change != nullptr);
    CHECK(change->accountId == "account-1");
    CHECK(change->mailboxId == "mailbox-1");
}

TEST_CASE("remote codec round-trips developer mailbox diagnostics",
          "[app][remote-codec][developer-diagnostics]")
{
    javelin::app::DeveloperMailboxRecord mailbox;
    mailbox.accountId = QStringLiteral("account-1");
    mailbox.accountName = QStringLiteral("Personal");
    mailbox.accountEmailAddress = QStringLiteral("person@example.test");
    mailbox.mailboxId = QStringLiteral("mailbox-1");
    mailbox.mailboxName = QStringLiteral("Inbox");
    mailbox.parentMailboxId = QStringLiteral("parent-1");
    mailbox.role = QStringLiteral("inbox");
    mailbox.totalEmails = 42;
    mailbox.unreadEmails = 3;
    mailbox.isSubscribed = true;
    mailbox.mayReadItems = true;
    mailbox.accountEmailState = QStringLiteral("email-state-7");
    mailbox.offlineDesired = true;
    mailbox.offlineExpectedTotal = 42;
    mailbox.usage = {.sqliteEstimatedBytes = 4096,
                     .logicalBodyBytes = 8192,
                     .sharedBodyBytes = 2048,
                     .reclaimableBodyBytes = 6144,
                     .allocatedBodyBytes = 12288,
                     .missingBodyObjects = 1,
                     .activeBodyLeases = 2};
    mailbox.measuredAt = QStringLiteral("2026-08-07T06:30:00.000Z");

    javelin::app::DeveloperDiagnosticsSnapshot snapshot;
    snapshot.databasePath = QStringLiteral("/cache/cache.sqlite3");
    snapshot.vaultPath = QStringLiteral("/cache/mail-vault/v1");
    snapshot.databaseDataVersion = 12;
    snapshot.mailboxes.push_back(mailbox);
    const javelin::app::DeveloperDiagnosticsResult result{std::move(snapshot)};

    const auto encoded = javelin::app::remote::encode(result);
    const auto* payload = std::get_if<QByteArray>(&encoded);
    REQUIRE(payload != nullptr);

    const auto decoded =
        javelin::app::remote::decodeValue<javelin::app::DeveloperDiagnosticsResult>(*payload);
    const auto* decodedResult = std::get_if<javelin::app::DeveloperDiagnosticsResult>(&decoded);
    REQUIRE(decodedResult != nullptr);
    const auto* decodedSnapshot =
        std::get_if<javelin::app::DeveloperDiagnosticsSnapshot>(decodedResult);
    REQUIRE(decodedSnapshot != nullptr);
    REQUIRE(decodedSnapshot->mailboxes.size() == 1);
    const auto& decodedMailbox = decodedSnapshot->mailboxes.front();
    CHECK(decodedSnapshot->databaseDataVersion == 12);
    CHECK(decodedMailbox.accountId == QStringLiteral("account-1"));
    CHECK(decodedMailbox.parentMailboxId == QStringLiteral("parent-1"));
    CHECK(decodedMailbox.offlineExpectedTotal == 42);
    CHECK(decodedMailbox.usage.reclaimableBodyBytes == 6144);
    CHECK(decodedMailbox.usage.activeBodyLeases == 2);
}

TEST_CASE("remote codec round-trips developer mailbox clear results",
          "[app][remote-codec][developer-maintenance]")
{
    const javelin::app::DeveloperMailboxClearResult result{
        javelin::app::DeveloperMailboxClearQueued{
            .jobId = QStringLiteral("mailbox-cache-cleanup:job-1"),
        }};

    const auto encoded = javelin::app::remote::encode(result);
    const auto* payload = std::get_if<QByteArray>(&encoded);
    REQUIRE(payload != nullptr);

    const auto decoded =
        javelin::app::remote::decodeValue<javelin::app::DeveloperMailboxClearResult>(*payload);
    const auto* decodedResult = std::get_if<javelin::app::DeveloperMailboxClearResult>(&decoded);
    REQUIRE(decodedResult != nullptr);
    const auto* queued = std::get_if<javelin::app::DeveloperMailboxClearQueued>(decodedResult);
    REQUIRE(queued != nullptr);
    CHECK(queued->jobId == QStringLiteral("mailbox-cache-cleanup:job-1"));
}

TEST_CASE("remote codec round-trips sender Identity signatures and pending creates",
          "[app][remote-codec][identity]")
{
    const javelin::jmap::identity::IdentityListResult result{
        javelin::jmap::identity::IdentitySnapshot{
            .identities = {{.id = "identity-1",
                            .name = "Alice",
                            .email = "alice@example.test",
                            .replyTo = {{.name = "Replies", .email = "reply@example.test"}},
                            .bcc = {},
                            .textSignature = "Regards,\nAlice",
                            .htmlSignature = "<p>Regards,<br>Alice</p>",
                            .mayDelete = false}},
            .pendingCreates = {{.creationId = "creation-1",
                                .mutationId = "mutation-1",
                                .identity = {.id = {},
                                             .name = "Alice Work",
                                             .email = "alice@example.test",
                                             .replyTo = {},
                                             .bcc = {},
                                             .textSignature = "Work",
                                             .htmlSignature = "<p>Work</p>",
                                             .mayDelete = true},
                                .status = "unknown",
                                .errorJson = std::nullopt}},
        }};

    const auto encoded = javelin::app::remote::encode(result);
    const auto* payload = std::get_if<QByteArray>(&encoded);
    REQUIRE(payload != nullptr);
    const auto decoded =
        javelin::app::remote::decodeValue<javelin::jmap::identity::IdentityListResult>(*payload);
    const auto* decodedResult = std::get_if<javelin::jmap::identity::IdentityListResult>(&decoded);
    REQUIRE(decodedResult != nullptr);
    const auto* snapshot = std::get_if<javelin::jmap::identity::IdentitySnapshot>(decodedResult);
    REQUIRE(snapshot != nullptr);
    REQUIRE(snapshot->identities.size() == 1);
    CHECK(snapshot->identities.front().htmlSignature ==
          std::optional<std::string>{"<p>Regards,<br>Alice</p>"});
    REQUIRE(snapshot->pendingCreates.size() == 1);
    CHECK(snapshot->pendingCreates.front().identity.id.empty());
    CHECK(snapshot->pendingCreates.front().status == "unknown");
}

TEST_CASE("remote codec rejects trailing data", "[app][remote-codec]")
{
    auto encoded = javelin::app::remote::encode(QStringLiteral("value"));
    auto* payload = std::get_if<QByteArray>(&encoded);
    REQUIRE(payload != nullptr);
    payload->append('x');

    const auto decoded = javelin::app::remote::decodeValue<QString>(*payload);
    CHECK(std::holds_alternative<javelin::app::remote::CodecError>(decoded));
}
