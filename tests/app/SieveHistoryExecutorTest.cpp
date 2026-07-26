#include "app/undo/SieveHistoryExecutor.h"

#include <QCoroTask>

#include <catch2/catch_test_macros.hpp>

#include <ranges>
#include <unordered_map>

namespace
{
    using namespace javelin::app::undo;

    class FakeSieveHistoryPort final : public SieveHistoryPort
    {
      public:
        std::vector<javelin::jmap::sieve::SieveScript> scripts;
        std::unordered_map<std::string, QByteArray> content;
        int nextId = 1;

        QCoro::Task<javelin::jmap::sieve::SieveListResult> requestSieveScripts(std::string) override
        {
            auto result = scripts;
            co_return result;
        }

        QCoro::Task<javelin::jmap::sieve::SieveContentResult>
        requestSieveScript(std::string, javelin::jmap::sieve::SieveScript script) override
        {
            auto result = content.at(script.id);
            co_return result;
        }

        QCoro::Task<javelin::jmap::sieve::SieveSaveResult>
        saveSieveScript(std::string, javelin::jmap::sieve::SieveScript script,
                        QByteArray newContent, CommandOrigin) override
        {
            if (script.id.empty())
            {
                script.id = "new-" + std::to_string(nextId++);
                scripts.push_back(script);
            }
            content.insert_or_assign(script.id, std::move(newContent));
            co_return script;
        }

        QCoro::Task<javelin::jmap::sieve::SieveDeleteResult>
        deleteSieveScript(std::string, javelin::jmap::sieve::SieveScript script,
                          CommandOrigin) override
        {
            std::erase_if(scripts, [&](const auto& value) { return value.id == script.id; });
            content.erase(script.id);
            co_return std::monostate{};
        }

        QCoro::Task<javelin::jmap::sieve::SieveActivationResult>
        setSieveScriptActive(std::string, javelin::jmap::sieve::SieveScript script,
                             const bool active, CommandOrigin) override
        {
            for (auto& value : scripts)
                value.isActive = active && value.id == script.id;
            co_return std::monostate{};
        }
    };

    [[nodiscard]] HistoryEntry entry(SieveHistory history)
    {
        HistoryEntry value;
        value.entryId = QStringLiteral("history-1");
        value.label = QStringLiteral("Edit Sieve Script");
        value.domain = HistoryDomain::Mail;
        value.commandKind = QStringLiteral("sieve");
        value.payload = std::move(history);
        value.status = HistoryEntryStatus::Ready;
        return value;
    }
} // namespace

TEST_CASE("undoing Sieve creation deletes only the created script", "[app][undo][sieve-executor]")
{
    FakeSieveHistoryPort port;
    port.scripts = {
        {.id = "script-1", .name = "vacation", .blobId = "blob-1", .isActive = false},
        {.id = "unrelated", .name = "filing", .blobId = "blob-2", .isActive = true},
    };
    port.content = {{"script-1", QByteArrayLiteral("discard;")},
                    {"unrelated", QByteArrayLiteral("keep;")}};
    SieveHistoryExecutor executor{port};
    const auto result = QCoro::waitFor(executor.execute(entry({
                                                            .connectionId = "connection-1",
                                                            .accountId = "account-1",
                                                            .currentScriptId = "script-1",
                                                            .previousScriptId = std::nullopt,
                                                            .beforeName = std::nullopt,
                                                            .beforeContent = std::nullopt,
                                                            .afterName = "vacation",
                                                            .afterContent = "discard;",
                                                            .activeScriptIdBefore = std::nullopt,
                                                            .activeScriptIdAfter = std::nullopt,
                                                        }),
                                                        HistoryExecutionDirection::Undo));

    CHECK(result.outcome == HistoryExecutionOutcome::Success);
    REQUIRE(port.scripts.size() == 1);
    CHECK(port.scripts.front().id == "unrelated");
    REQUIRE(result.updatedPayload.has_value());
    CHECK_FALSE(std::get<SieveHistory>(*result.updatedPayload).currentScriptId.has_value());
}

TEST_CASE("undoing Sieve deletion recreates the script and remaps its id",
          "[app][undo][sieve-executor]")
{
    FakeSieveHistoryPort port;
    SieveHistoryExecutor executor{port};
    const auto result = QCoro::waitFor(executor.execute(entry({
                                                            .connectionId = "connection-1",
                                                            .accountId = "account-1",
                                                            .currentScriptId = std::nullopt,
                                                            .previousScriptId = "old-id",
                                                            .beforeName = "vacation",
                                                            .beforeContent = "discard;",
                                                            .afterName = std::nullopt,
                                                            .afterContent = std::nullopt,
                                                            .activeScriptIdBefore = std::nullopt,
                                                            .activeScriptIdAfter = std::nullopt,
                                                        }),
                                                        HistoryExecutionDirection::Undo));

    CHECK(result.outcome == HistoryExecutionOutcome::Success);
    REQUIRE(port.scripts.size() == 1);
    CHECK(port.content.at(port.scripts.front().id) == QByteArrayLiteral("discard;"));
    CHECK(std::get<SieveHistory>(*result.updatedPayload).currentScriptId ==
          std::optional<std::string>{"new-1"});
}

TEST_CASE("Sieve undo rejects an external content change", "[app][undo][sieve-executor]")
{
    FakeSieveHistoryPort port;
    port.scripts = {
        {.id = "script-1", .name = "vacation", .blobId = "blob-2", .isActive = false},
    };
    port.content = {{"script-1", QByteArrayLiteral("external-change;")}};
    SieveHistoryExecutor executor{port};
    const auto result = QCoro::waitFor(executor.execute(entry({
                                                            .connectionId = "connection-1",
                                                            .accountId = "account-1",
                                                            .currentScriptId = "script-1",
                                                            .previousScriptId = std::nullopt,
                                                            .beforeName = "vacation",
                                                            .beforeContent = "keep;",
                                                            .afterName = "vacation",
                                                            .afterContent = "discard;",
                                                            .activeScriptIdBefore = std::nullopt,
                                                            .activeScriptIdAfter = std::nullopt,
                                                        }),
                                                        HistoryExecutionDirection::Undo));

    CHECK(result.outcome == HistoryExecutionOutcome::Conflict);
    CHECK(port.content.at("script-1") == QByteArrayLiteral("external-change;"));
}

TEST_CASE("Sieve activation history restores the previously active script",
          "[app][undo][sieve-executor]")
{
    FakeSieveHistoryPort port;
    port.scripts = {
        {.id = "old-active", .name = "old", .blobId = "blob-1", .isActive = false},
        {.id = "new-active", .name = "new", .blobId = "blob-2", .isActive = true},
    };
    SieveHistoryExecutor executor{port};
    const auto result = QCoro::waitFor(executor.execute(entry({
                                                            .connectionId = "connection-1",
                                                            .accountId = "account-1",
                                                            .currentScriptId = "new-active",
                                                            .previousScriptId = std::nullopt,
                                                            .beforeName = std::nullopt,
                                                            .beforeContent = std::nullopt,
                                                            .afterName = std::nullopt,
                                                            .afterContent = std::nullopt,
                                                            .activeScriptIdBefore = "old-active",
                                                            .activeScriptIdAfter = "new-active",
                                                        }),
                                                        HistoryExecutionDirection::Undo));

    CHECK(result.outcome == HistoryExecutionOutcome::Success);
    const auto active =
        std::ranges::find(port.scripts, true, &javelin::jmap::sieve::SieveScript::isActive);
    REQUIRE(active != port.scripts.end());
    CHECK(active->id == "old-active");
}
