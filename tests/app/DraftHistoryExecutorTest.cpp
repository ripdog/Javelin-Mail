#include "app/undo/DraftHistoryExecutor.h"

#include "jmap/submission/DraftSnapshotSerialization.h"

#include <QCoroTask>

#include <catch2/catch_test_macros.hpp>

namespace
{
    using namespace javelin::app::undo;

    class FakeDraftHistoryPort final : public DraftHistoryPort
    {
      public:
        std::optional<javelin::jmap::submission::DraftSnapshot> current;
        int nextId = 1;

        QCoro::Task<
            std::variant<javelin::jmap::submission::DraftSnapshot, javelin::jmap::OperationError>>
        loadAuthoritativeDraft(std::string, std::string draftEmailId, std::string) override
        {
            if (!current.has_value() || current->draftEmailId != draftEmailId)
                co_return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::NotFound,
                    .message = QStringLiteral("Draft not found."),
                };
            auto result = *current;
            co_return result;
        }

        QCoro::Task<std::variant<javelin::jmap::submission::DraftSaveSummary,
                                 javelin::jmap::OperationError>>
        saveDraftFromHistory(javelin::jmap::submission::DraftSnapshot snapshot,
                             CommandOrigin) override
        {
            snapshot.draftEmailId = "draft-" + std::to_string(nextId++);
            current = snapshot;
            co_return javelin::jmap::submission::DraftSaveSummary{
                .composeSessionId = snapshot.composeSessionId,
                .accountId = snapshot.accountId,
                .draftEmailId = *snapshot.draftEmailId,
                .affectedMailboxIds = {},
                .operationGroupId = "group",
                .createMutationId = "create",
                .destroyMutationId = std::nullopt,
                .acceptedRevision = snapshot.revision,
                .acceptedManifest = snapshot.attachments,
                .savedSnapshot = snapshot,
            };
        }

        QCoro::Task<std::variant<javelin::jmap::submission::DraftDeleteSummary,
                                 javelin::jmap::OperationError>>
        deleteDraftFromHistory(std::string accountId, std::string draftEmailId,
                               CommandOrigin) override
        {
            current = std::nullopt;
            co_return javelin::jmap::submission::DraftDeleteSummary{
                .accountId = std::move(accountId),
                .draftEmailId = std::move(draftEmailId),
                .operationGroupId = "group",
                .mutationId = "destroy",
            };
        }
    };

    [[nodiscard]] javelin::jmap::submission::DraftSnapshot snapshot(std::optional<std::string> id,
                                                                    std::string body)
    {
        return {
            .composeSessionId = "compose-1",
            .accountId = "account-1",
            .draftEmailId = std::move(id),
            .mode = javelin::jmap::submission::ComposeMode::EditDraft,
            .editorMode = javelin::jmap::submission::BodyEditorMode::RichText,
            .identityId = "identity-1",
            .to = {},
            .cc = {},
            .bcc = {},
            .subject = "Notes",
            .plainTextBody = std::move(body),
            .htmlBody = {},
            .threading = {},
            .attachments = {},
        };
    }

    [[nodiscard]] HistoryEntry entry(std::optional<javelin::jmap::submission::DraftSnapshot> before,
                                     const javelin::jmap::submission::DraftSnapshot& after)
    {
        DraftHistory history{
            .connectionId = "connection-1",
            .accountId = after.accountId,
            .composeSessionId = after.composeSessionId,
            .currentDraftEmailId = after.draftEmailId,
            .beforeSnapshotJson =
                before.has_value()
                    ? std::optional{javelin::jmap::submission::serializeDraftSnapshot(*before)
                                        .toStdString()}
                    : std::nullopt,
            .afterSnapshotJson =
                javelin::jmap::submission::serializeDraftSnapshot(after).toStdString(),
        };
        HistoryEntry value;
        value.entryId = QStringLiteral("history-1");
        value.label = QStringLiteral("Save Draft");
        value.domain = HistoryDomain::Mail;
        value.commandKind = QStringLiteral("draft");
        value.payload = std::move(history);
        value.status = HistoryEntryStatus::Ready;
        return value;
    }
} // namespace

TEST_CASE("undo and redo of the first explicit draft save delete and recreate it",
          "[app][undo][draft-executor]")
{
    FakeDraftHistoryPort port;
    port.current = snapshot("draft-original", "saved");
    DraftHistoryExecutor executor{port};

    auto undone = QCoro::waitFor(
        executor.execute(entry(std::nullopt, *port.current), HistoryExecutionDirection::Undo));
    CHECK(undone.outcome == HistoryExecutionOutcome::Success);
    CHECK_FALSE(port.current.has_value());

    HistoryEntry redoEntry = entry(std::nullopt, snapshot("draft-original", "saved"));
    redoEntry.payload = *undone.updatedPayload;
    auto redone =
        QCoro::waitFor(executor.execute(std::move(redoEntry), HistoryExecutionDirection::Redo));
    CHECK(redone.outcome == HistoryExecutionOutcome::Success);
    REQUIRE(port.current.has_value());
    CHECK(port.current->plainTextBody == "saved");
    CHECK(std::get<DraftHistory>(*redone.updatedPayload).currentDraftEmailId ==
          std::optional<std::string>{"draft-1"});
}

TEST_CASE("undoing a draft edit restores the complete prior snapshot with a remapped id",
          "[app][undo][draft-executor]")
{
    FakeDraftHistoryPort port;
    const auto before = snapshot("draft-old", "before");
    const auto after = snapshot("draft-current", "after");
    port.current = after;
    DraftHistoryExecutor executor{port};

    const auto result =
        QCoro::waitFor(executor.execute(entry(before, after), HistoryExecutionDirection::Undo));
    CHECK(result.outcome == HistoryExecutionOutcome::Success);
    REQUIRE(port.current.has_value());
    CHECK(port.current->plainTextBody == "before");
    CHECK(port.current->draftEmailId == std::optional<std::string>{"draft-1"});
}

TEST_CASE("draft undo refuses to overwrite an external edit", "[app][undo][draft-executor]")
{
    FakeDraftHistoryPort port;
    const auto before = snapshot("draft-old", "before");
    const auto after = snapshot("draft-current", "after");
    port.current = snapshot("draft-current", "external");
    DraftHistoryExecutor executor{port};

    const auto result =
        QCoro::waitFor(executor.execute(entry(before, after), HistoryExecutionDirection::Undo));
    CHECK(result.outcome == HistoryExecutionOutcome::Conflict);
    CHECK(port.current->plainTextBody == "external");
}
