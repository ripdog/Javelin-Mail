#include "app/DeferredSendRepository.h"
#include "app/undo/HistoryRepository.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QUuid>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <variant>

namespace
{
    using namespace javelin::app;
    using namespace javelin::app::undo;

    class ApplicationGuard
    {
      public:
        ApplicationGuard()
        {
            if (QCoreApplication::instance() != nullptr)
                return;
            static int argc = 1;
            static char appName[] = "javelin-deferred-send-tests";
            static char* argv[] = {appName, nullptr};
            m_application = std::make_unique<QCoreApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };

    [[nodiscard]] javelin::jmap::cache::DatabaseConnection openDatabase(const QString& path)
    {
        auto result = javelin::jmap::cache::DatabaseConnection::open({
            .connectionName = QStringLiteral("deferred-send-test-%1")
                                  .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)),
            .databasePath = path,
        });
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
            FAIL(error->message.toStdString());
        return std::get<javelin::jmap::cache::DatabaseConnection>(std::move(result));
    }

    [[nodiscard]] HistoryEntry addHistory(HistoryRepository& repository, const QString& sendId)
    {
        auto result = repository.pushUndoClearingRedo({
            .entryId = QStringLiteral("history-") + sendId,
            .label = QStringLiteral("Send Message"),
            .domain = HistoryDomain::DeferredSend,
            .commandKind = QStringLiteral("deferred_send"),
            .payload =
                DeferredSendHistory{
                    .sendId = sendId.toStdString(),
                    .connectionId = "connection",
                    .accountId = "account",
                    .composeSessionId = "compose",
                    .draftEmailId = "draft",
                    .subject = "Subject",
                    .delaySeconds = 10,
                },
            .status = HistoryEntryStatus::Ready,
            .operationGroupId = std::nullopt,
            .expiresAt = std::nullopt,
            .explanation = std::nullopt,
            .failureJson = std::nullopt,
            .createdAt = {},
            .updatedAt = {},
        });
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
            FAIL(error->message.toStdString());
        return std::get<HistoryEntry>(std::move(result));
    }

    [[nodiscard]] PendingSend pending(const QString& sendId, const QString& historyEntryId,
                                      const QDateTime& dueAt)
    {
        return {
            .sendId = sendId,
            .historyEntryId = historyEntryId,
            .connectionId = "connection",
            .accountId = "account",
            .composeSessionId = "compose",
            .draftEmailId = "draft",
            .subject = "Subject",
            .status = DeferredSendStatus::Scheduled,
            .dueAt = dueAt,
            .dispatchStartedAt = std::nullopt,
            .submissionId = std::nullopt,
            .lastError = std::nullopt,
            .createdAt = {},
            .updatedAt = {},
        };
    }

    [[nodiscard]] bool requireBool(std::variant<bool, javelin::jmap::cache::DatabaseError> result)
    {
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
            FAIL(error->message.toStdString());
        return std::get<bool>(result);
    }

    [[nodiscard]] PendingSend requireSend(
        std::variant<std::optional<PendingSend>, javelin::jmap::cache::DatabaseError> result)
    {
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
            FAIL(error->message.toStdString());
        auto send = std::get<std::optional<PendingSend>>(std::move(result));
        REQUIRE(send.has_value());
        return std::move(*send);
    }
} // namespace

TEST_CASE("deferred send cancellation and dispatch are mutually exclusive",
          "[app][deferred-send][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto database = openDatabase(directory.filePath(QStringLiteral("deferred.sqlite3")));
    HistoryRepository historyRepository{database};
    DeferredSendRepository repository{database};

    const auto dueAt = QDateTime::currentDateTimeUtc().addSecs(10);
    const auto firstHistory = addHistory(historyRepository, QStringLiteral("cancel-wins"));
    REQUIRE_FALSE(
        repository.insert(pending(QStringLiteral("cancel-wins"), firstHistory.entryId, dueAt)));
    CHECK(requireBool(repository.cancelBeforeDispatch(QStringLiteral("cancel-wins"))));
    CHECK_FALSE(requireBool(repository.claimForDispatch(QStringLiteral("cancel-wins"),
                                                        QDateTime::currentDateTimeUtc())));

    const auto secondHistory = addHistory(historyRepository, QStringLiteral("dispatch-wins"));
    REQUIRE_FALSE(
        repository.insert(pending(QStringLiteral("dispatch-wins"), secondHistory.entryId, dueAt)));
    CHECK(requireBool(repository.claimForDispatch(QStringLiteral("dispatch-wins"),
                                                  QDateTime::currentDateTimeUtc())));
    CHECK_FALSE(requireBool(repository.cancelBeforeDispatch(QStringLiteral("dispatch-wins"))));
}

TEST_CASE("scheduling activates history and clears redo in one transaction",
          "[app][deferred-send][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto database = openDatabase(directory.filePath(QStringLiteral("deferred.sqlite3")));
    HistoryRepository historyRepository{database};
    DeferredSendRepository repository{database};

    auto oldEntry = addHistory(historyRepository, QStringLiteral("old"));
    auto moved = historyRepository.move(oldEntry, HistoryStack::Redo, HistoryEntryStatus::Ready);
    REQUIRE(std::holds_alternative<HistoryEntry>(moved));

    auto reservation = historyRepository.insertPreparing({
        .entryId = QStringLiteral("history-scheduled"),
        .label = QStringLiteral("Send Message"),
        .domain = HistoryDomain::DeferredSend,
        .commandKind = QStringLiteral("deferred_send"),
        .payload =
            DeferredSendHistory{
                .sendId = "scheduled",
                .connectionId = "connection",
                .accountId = "account",
                .composeSessionId = "compose",
                .draftEmailId = "draft",
                .subject = "Subject",
                .delaySeconds = 10,
            },
        .status = HistoryEntryStatus::Preparing,
        .operationGroupId = std::nullopt,
        .expiresAt = std::nullopt,
        .explanation = std::nullopt,
        .failureJson = std::nullopt,
        .createdAt = {},
        .updatedAt = {},
    });
    REQUIRE(std::holds_alternative<HistoryEntry>(reservation));
    const auto reserved = std::get<HistoryEntry>(std::move(reservation));
    const auto send = pending(QStringLiteral("scheduled"), reserved.entryId,
                              QDateTime::currentDateTimeUtc().addSecs(10));
    REQUIRE_FALSE(repository.insertAndActivateHistory(send));

    auto loaded = historyRepository.load();
    REQUIRE(std::holds_alternative<std::vector<HistoryEntry>>(loaded));
    const auto& entries = std::get<std::vector<HistoryEntry>>(loaded);
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().entryId == reserved.entryId);
    CHECK(entries.front().stack == HistoryStack::Undo);
    CHECK(entries.front().status == HistoryEntryStatus::Ready);
    CHECK(requireSend(repository.find(QStringLiteral("scheduled"))).status ==
          DeferredSendStatus::Scheduled);
}

TEST_CASE("deferred sends survive restart and interrupted dispatch becomes unknown",
          "[app][deferred-send][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("deferred.sqlite3"));
    const auto dueAt = QDateTime::currentDateTimeUtc().addSecs(10);

    {
        auto database = openDatabase(path);
        HistoryRepository historyRepository{database};
        DeferredSendRepository repository{database};
        const auto history = addHistory(historyRepository, QStringLiteral("restart"));
        REQUIRE_FALSE(
            repository.insert(pending(QStringLiteral("restart"), history.entryId, dueAt)));
        CHECK(requireBool(repository.claimForDispatch(QStringLiteral("restart"),
                                                      QDateTime::currentDateTimeUtc())));
    }

    auto database = openDatabase(path);
    DeferredSendRepository repository{database};
    auto recovery = repository.recoverDispatching();
    REQUIRE(std::holds_alternative<std::size_t>(recovery));
    CHECK(std::get<std::size_t>(recovery) == 1);
    const auto recovered = requireSend(repository.find(QStringLiteral("restart")));
    CHECK(recovered.status == DeferredSendStatus::Unknown);
    CHECK(recovered.dueAt.isValid());
    CHECK(recovered.createdAt.isValid());
    CHECK(recovered.updatedAt.isValid());
}

TEST_CASE("redo reschedules a cancelled send with a fresh deadline",
          "[app][deferred-send][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto database = openDatabase(directory.filePath(QStringLiteral("deferred.sqlite3")));
    HistoryRepository historyRepository{database};
    DeferredSendRepository repository{database};
    const auto history = addHistory(historyRepository, QStringLiteral("redo"));
    const auto originalDueAt = QDateTime::currentDateTimeUtc().addSecs(10);
    REQUIRE_FALSE(
        repository.insert(pending(QStringLiteral("redo"), history.entryId, originalDueAt)));
    REQUIRE(requireBool(repository.cancelBeforeDispatch(QStringLiteral("redo"))));

    const auto freshDueAt = originalDueAt.addSecs(30);
    CHECK(requireBool(repository.reschedule(QStringLiteral("redo"), freshDueAt)));
    const auto rescheduled = requireSend(repository.find(QStringLiteral("redo")));
    CHECK(rescheduled.status == DeferredSendStatus::Scheduled);
    CHECK(rescheduled.dueAt == freshDueAt);
}

TEST_CASE("release for dispatch advances only scheduled deadlines and survives reopening",
          "[app][deferred-send][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("release.sqlite3"));
    const auto releasedAt = QDateTime::currentDateTimeUtc();

    {
        auto database = openDatabase(path);
        HistoryRepository historyRepository{database};
        DeferredSendRepository repository{database};

        const auto futureHistory = addHistory(historyRepository, QStringLiteral("future"));
        const auto futureDueAt = releasedAt.addSecs(30);
        REQUIRE_FALSE(repository.insert(
            pending(QStringLiteral("future"), futureHistory.entryId, futureDueAt)));
        CHECK(requireBool(repository.releaseForDispatch(QStringLiteral("future"), releasedAt)));
        CHECK(requireSend(repository.find(QStringLiteral("future"))).dueAt == releasedAt);

        const auto pastHistory = addHistory(historyRepository, QStringLiteral("past"));
        const auto pastDueAt = releasedAt.addSecs(-30);
        REQUIRE_FALSE(
            repository.insert(pending(QStringLiteral("past"), pastHistory.entryId, pastDueAt)));
        CHECK(requireBool(repository.releaseForDispatch(QStringLiteral("past"), releasedAt)));
        CHECK(requireSend(repository.find(QStringLiteral("past"))).dueAt == pastDueAt);

        const auto cancelledHistory = addHistory(historyRepository, QStringLiteral("cancelled"));
        REQUIRE_FALSE(repository.insert(
            pending(QStringLiteral("cancelled"), cancelledHistory.entryId, futureDueAt)));
        REQUIRE(requireBool(repository.cancelBeforeDispatch(QStringLiteral("cancelled"))));
        CHECK_FALSE(
            requireBool(repository.releaseForDispatch(QStringLiteral("cancelled"), releasedAt)));

        const auto dispatchingHistory =
            addHistory(historyRepository, QStringLiteral("dispatching"));
        REQUIRE_FALSE(repository.insert(
            pending(QStringLiteral("dispatching"), dispatchingHistory.entryId, futureDueAt)));
        REQUIRE(
            requireBool(repository.claimForDispatch(QStringLiteral("dispatching"), releasedAt)));
        CHECK_FALSE(
            requireBool(repository.releaseForDispatch(QStringLiteral("dispatching"), releasedAt)));
    }

    auto database = openDatabase(path);
    DeferredSendRepository repository{database};
    CHECK(requireSend(repository.find(QStringLiteral("future"))).dueAt == releasedAt);
    CHECK(requireSend(repository.find(QStringLiteral("past"))).dueAt == releasedAt.addSecs(-30));
}
