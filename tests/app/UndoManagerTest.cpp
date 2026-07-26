#include "app/undo/UndoManager.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QUuid>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <utility>
#include <variant>

namespace
{
    using namespace javelin::app::undo;

    class ApplicationGuard
    {
      public:
        ApplicationGuard()
        {
            if (QCoreApplication::instance() != nullptr)
                return;
            static int argc = 1;
            static char appName[] = "javelin-undo-manager-tests";
            static char* argv[] = {appName, nullptr};
            m_application = std::make_unique<QCoreApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };

    struct Fixture
    {
        QTemporaryDir directory;
        javelin::jmap::cache::DatabaseConnection database;
        HistoryRepository repository;
        UndoManager manager;

        Fixture() : database(open()), repository(database), manager(repository)
        {
            REQUIRE_FALSE(manager.load().has_value());
        }

      private:
        [[nodiscard]] javelin::jmap::cache::DatabaseConnection open()
        {
            REQUIRE(directory.isValid());
            auto result = javelin::jmap::cache::DatabaseConnection::open({
                .connectionName = QStringLiteral("undo-manager-test-%1")
                                      .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)),
                .databasePath = directory.filePath(QStringLiteral("history.sqlite3")),
            });
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
                FAIL(error->message.toStdString());
            return std::get<javelin::jmap::cache::DatabaseConnection>(std::move(result));
        }
    };

    class FakeExecutor final : public HistoryCommandExecutor
    {
      public:
        HistoryExecutionResult result{
            .outcome = HistoryExecutionOutcome::Success,
            .updatedPayload = std::nullopt,
            .refreshScope = {},
            .summary = {},
            .objectFailures = {},
            .mayRemoveFromHistory = false,
        };
        int calls = 0;
        std::optional<HistoryExecutionDirection> lastDirection;

        QCoro::Task<HistoryExecutionResult>
        execute(HistoryEntry, const HistoryExecutionDirection direction) override
        {
            ++calls;
            lastDirection = direction;
            co_return result;
        }
    };

    [[nodiscard]] HistoryEntry prepareAndCommit(UndoManager& manager, const QString& label)
    {
        auto prepareResult = manager.prepareNormal(label, HistoryDomain::Mail,
                                                   ImpossibleHistory{.explanation = "test"},
                                                   QStringLiteral("group-") + label);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&prepareResult))
            FAIL(error->message.toStdString());
        auto prepared = std::get<std::optional<HistoryEntry>>(std::move(prepareResult));
        REQUIRE(prepared.has_value());

        auto commitResult = manager.commitNormal(std::move(*prepared));
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&commitResult))
            FAIL(error->message.toStdString());
        return std::get<HistoryEntry>(std::move(commitResult));
    }

} // namespace

TEST_CASE("undo manager transfers successful commands between stacks", "[app][undo][manager]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    Fixture fixture;
    FakeExecutor executor;
    fixture.manager.setExecutor(HistoryDomain::Mail, &executor);
    const auto entry = prepareAndCommit(fixture.manager, QStringLiteral("Archive message"));

    CHECK(fixture.manager.state().canUndo);
    CHECK(fixture.manager.state().undoLabel == QStringLiteral("Undo Archive message"));
    CHECK(QCoro::waitFor(fixture.manager.undo()));
    CHECK(executor.calls == 1);
    CHECK(executor.lastDirection == HistoryExecutionDirection::Undo);
    CHECK_FALSE(fixture.manager.state().canUndo);
    CHECK(fixture.manager.state().canRedo);
    CHECK(fixture.manager.entries().front().entryId == entry.entryId);
    CHECK(fixture.manager.entries().front().stack == HistoryStack::Redo);

    CHECK(QCoro::waitFor(fixture.manager.redo()));
    CHECK(executor.calls == 2);
    CHECK(executor.lastDirection == HistoryExecutionDirection::Redo);
    CHECK(fixture.manager.state().canUndo);
    CHECK_FALSE(fixture.manager.state().canRedo);
}

TEST_CASE("failed undo retains its stack position and a new mutation clears redo",
          "[app][undo][manager]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    Fixture fixture;
    FakeExecutor executor;
    fixture.manager.setExecutor(HistoryDomain::Mail, &executor);
    const auto first = prepareAndCommit(fixture.manager, QStringLiteral("First"));

    executor.result.outcome = HistoryExecutionOutcome::Conflict;
    executor.result.summary = QStringLiteral("The message changed on another client.");
    CHECK_FALSE(QCoro::waitFor(fixture.manager.undo()));
    REQUIRE(fixture.manager.entries().size() == 1);
    CHECK(fixture.manager.entries().front().entryId == first.entryId);
    CHECK(fixture.manager.entries().front().stack == HistoryStack::Undo);
    CHECK(fixture.manager.entries().front().status == HistoryEntryStatus::Ready);

    executor.result.outcome = HistoryExecutionOutcome::Success;
    CHECK(QCoro::waitFor(fixture.manager.undo()));
    REQUIRE(fixture.manager.state().canRedo);
    static_cast<void>(prepareAndCommit(fixture.manager, QStringLiteral("Second")));
    CHECK_FALSE(fixture.manager.state().canRedo);
    REQUIRE(fixture.manager.entries().size() == 1);
    CHECK(fixture.manager.entries().front().label == QStringLiteral("Second"));
}

TEST_CASE("impossible history is removed only after acknowledgement", "[app][undo][manager]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    Fixture fixture;
    std::optional<HistoryFailure> failure;
    QObject::connect(&fixture.manager, &UndoManager::executionFailed,
                     [&failure](HistoryFailure value) { failure = std::move(value); });

    auto result = fixture.manager.recordImpossible(
        QStringLiteral("Permanently Delete Message"), HistoryDomain::Mail,
        QStringLiteral("Unable to undo permanently deleting this message."));
    if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
        FAIL(error->message.toStdString());

    CHECK_FALSE(QCoro::waitFor(fixture.manager.undo()));
    REQUIRE(failure.has_value());
    CHECK(failure->acknowledgeAndRemove);
    REQUIRE(fixture.manager.entries().size() == 1);
    REQUIRE_FALSE(fixture.manager.acknowledgeAndRemove(failure->entryId).has_value());
    CHECK(fixture.manager.entries().empty());
}

TEST_CASE("startup recovery blocks entries interrupted while executing", "[app][undo][manager]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    Fixture fixture;
    auto entry = prepareAndCommit(fixture.manager, QStringLiteral("Move message"));
    entry.status = HistoryEntryStatus::ExecutingUndo;
    REQUIRE_FALSE(fixture.repository.update(entry).has_value());

    UndoManager recovered{fixture.repository};
    REQUIRE_FALSE(recovered.load().has_value());
    REQUIRE(recovered.entries().size() == 1);
    CHECK(recovered.entries().front().status == HistoryEntryStatus::BlockedUnknown);
    CHECK(recovered.state().blocked);
    CHECK_FALSE(recovered.state().canUndo);
    CHECK_FALSE(recovered.state().canRedo);
}

TEST_CASE("system child mutations do not create operation history", "[app][undo][manager]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    Fixture fixture;
    auto result =
        fixture.manager.prepareNormal(QStringLiteral("Save Draft"), HistoryDomain::Mail,
                                      ImpossibleHistory{.explanation = "nested"}, std::nullopt,
                                      std::nullopt, CommandOrigin::SystemChild);
    REQUIRE(std::holds_alternative<std::optional<HistoryEntry>>(result));
    CHECK_FALSE(std::get<std::optional<HistoryEntry>>(result).has_value());
    CHECK(fixture.manager.entries().empty());
}
