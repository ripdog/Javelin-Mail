#include "app/undo/HistoryRepository.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QUuid>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <variant>

namespace
{
    using javelin::app::undo::HistoryDomain;
    using javelin::app::undo::HistoryEntry;
    using javelin::app::undo::HistoryEntryStatus;
    using javelin::app::undo::HistoryRepository;
    using javelin::app::undo::HistoryStack;
    using javelin::app::undo::ImpossibleHistory;

    class ApplicationGuard
    {
      public:
        ApplicationGuard()
        {
            if (QCoreApplication::instance() != nullptr)
                return;
            static int argc = 1;
            static char appName[] = "javelin-history-tests";
            static char* argv[] = {appName, nullptr};
            m_application = std::make_unique<QCoreApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };

    [[nodiscard]] QString connectionName()
    {
        return QStringLiteral("history-test-%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    }

    [[nodiscard]] javelin::jmap::cache::DatabaseConnection openDatabase(const QString& path)
    {
        auto result = javelin::jmap::cache::DatabaseConnection::open({
            .connectionName = connectionName(),
            .databasePath = path,
        });
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
            FAIL(error->message.toStdString());
        return std::get<javelin::jmap::cache::DatabaseConnection>(std::move(result));
    }

    [[nodiscard]] HistoryEntry entry(const QString& id, const QString& label)
    {
        return {
            .entryId = id,
            .label = label,
            .domain = HistoryDomain::Mail,
            .commandKind = QStringLiteral("impossible"),
            .payload = ImpossibleHistory{.explanation = label.toStdString()},
            .status = HistoryEntryStatus::Ready,
            .operationGroupId = std::nullopt,
            .expiresAt = std::nullopt,
            .explanation = std::nullopt,
            .failureJson = std::nullopt,
            .createdAt = {},
            .updatedAt = {},
        };
    }

    [[nodiscard]] HistoryEntry
    requireEntry(std::variant<HistoryEntry, javelin::jmap::cache::DatabaseError> result)
    {
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
            FAIL(error->message.toStdString());
        return std::get<HistoryEntry>(std::move(result));
    }

    [[nodiscard]] std::vector<HistoryEntry> requireLoaded(
        std::variant<std::vector<HistoryEntry>, javelin::jmap::cache::DatabaseError> result)
    {
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
            FAIL(error->message.toStdString());
        return std::get<std::vector<HistoryEntry>>(std::move(result));
    }

} // namespace

TEST_CASE("operation history persists typed payloads in stable LIFO order",
          "[app][undo][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("history.sqlite3"));

    {
        auto database = openDatabase(path);
        HistoryRepository repository{database};
        const auto first = requireEntry(repository.pushUndoClearingRedo(
            entry(QStringLiteral("first"), QStringLiteral("Archive message"))));
        const auto second = requireEntry(repository.pushUndoClearingRedo(
            entry(QStringLiteral("second"), QStringLiteral("Flag message"))));
        CHECK(second.stackOrder > first.stackOrder);
    }

    auto database = openDatabase(path);
    HistoryRepository repository{database};
    const auto loaded = requireLoaded(repository.load());
    REQUIRE(loaded.size() == 2);
    CHECK(loaded.at(0).entryId == QStringLiteral("second"));
    CHECK(loaded.at(1).entryId == QStringLiteral("first"));
    CHECK(std::get<ImpossibleHistory>(loaded.at(0).payload).explanation == "Flag message");
}

TEST_CASE("history stack movement is durable and a new mutation clears all redo",
          "[app][undo][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto database = openDatabase(directory.filePath(QStringLiteral("history.sqlite3")));
    HistoryRepository repository{database};

    auto first = requireEntry(
        repository.pushUndoClearingRedo(entry(QStringLiteral("first"), QStringLiteral("First"))));
    first = requireEntry(repository.move(first, HistoryStack::Redo, HistoryEntryStatus::Ready));
    CHECK(first.stack == HistoryStack::Redo);

    static_cast<void>(requireEntry(repository.pushUndoClearingRedo(
        entry(QStringLiteral("second"), QStringLiteral("Second")))));
    const auto loaded = requireLoaded(repository.load());
    REQUIRE(loaded.size() == 1);
    CHECK(loaded.front().entryId == QStringLiteral("second"));
    CHECK(loaded.front().stack == HistoryStack::Undo);
}

TEST_CASE("preparing history does not clear redo until it becomes ready", "[app][undo][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto database = openDatabase(directory.filePath(QStringLiteral("history.sqlite3")));
    HistoryRepository repository{database};

    auto existing = requireEntry(repository.pushUndoClearingRedo(
        entry(QStringLiteral("existing"), QStringLiteral("Existing"))));
    existing =
        requireEntry(repository.move(existing, HistoryStack::Redo, HistoryEntryStatus::Ready));

    auto preparing = requireEntry(repository.insertPreparing(
        entry(QStringLiteral("preparing"), QStringLiteral("Preparing"))));
    auto loaded = requireLoaded(repository.load());
    REQUIRE(loaded.size() == 2);

    preparing.status = HistoryEntryStatus::ExecutingForward;
    REQUIRE_FALSE(repository.update(preparing).has_value());
    preparing = requireEntry(repository.markPreparedReady(std::move(preparing)));
    CHECK(preparing.status == HistoryEntryStatus::Ready);

    loaded = requireLoaded(repository.load());
    REQUIRE(loaded.size() == 1);
    CHECK(loaded.front().entryId == QStringLiteral("preparing"));
}
