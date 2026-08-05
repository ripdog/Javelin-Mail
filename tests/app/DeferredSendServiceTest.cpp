#include "app/DeferredSendService.h"

#include "app/undo/HistoryRepository.h"
#include "app/undo/UndoManager.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QEventLoop>
#include <QMetaObject>
#include <QTemporaryDir>
#include <QUuid>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
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
            static char appName[] = "javelin-deferred-send-service-tests";
            static char* argv[] = {appName, nullptr};
            m_application = std::make_unique<QCoreApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };

    [[nodiscard]] javelin::jmap::cache::DatabaseConnection openDatabase(const QString& path)
    {
        auto result = javelin::jmap::cache::DatabaseConnection::open({
            .connectionName = QStringLiteral("deferred-send-service-test-%1")
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
                    .delaySeconds = 30,
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
                                      const DeferredSendStatus status, const QDateTime& dueAt)
    {
        return {
            .sendId = sendId,
            .historyEntryId = historyEntryId,
            .connectionId = "connection",
            .accountId = "account",
            .composeSessionId = "compose",
            .draftEmailId = "draft",
            .subject = "Subject",
            .status = status,
            .dueAt = dueAt,
            .dispatchStartedAt = std::nullopt,
            .submissionId = std::nullopt,
            .lastError = std::nullopt,
            .createdAt = {},
            .updatedAt = {},
        };
    }

    class FakeConnectionProvider final : public AccountConnectionProvider
    {
      public:
        [[nodiscard]] std::optional<AccountConnectionSettings>
        connectionSettingsFor(std::string_view) const override
        {
            return settings;
        }

        std::optional<AccountConnectionSettings> settings = AccountConnectionSettings{
            .connectionId = "connection",
            .revision = 1,
            .sessionUrl = "https://example.test/jmap",
            .loginEmail = "alice@example.test",
            .apiKey = "api-key",
            .refreshToken = {},
            .tokenEndpoint = {},
            .oauthClientId = {},
        };
    };

    class ScriptedSubmitter final : public DeferredSendSubmitter
    {
      public:
        [[nodiscard]] QCoro::Task<DeferredSendSubmitResult>
        submit(javelin::jmap::LiveConnectionSettings, javelin::jmap::submission::PreparedSend,
               std::function<void()> dispatched) override
        {
            ++attempts;
            if (invokeDispatched && dispatched)
                dispatched();
            co_return result;
        }

        int attempts = 0;
        bool invokeDispatched = true;
        DeferredSendSubmitResult result = javelin::jmap::submission::SendSummary{
            .composeSessionId = "compose",
            .accountId = "account",
            .draftEmailId = "draft",
            .submissionId = std::string{"submission"},
            .acceptedRevision = 1,
            .scheduled = false,
        };
    };

    struct Fixture
    {
        ApplicationGuard application;
        QTemporaryDir directory;
        javelin::jmap::cache::DatabaseConnection database;
        HistoryRepository historyRepository;
        DeferredSendRepository repository;
        ScriptedSubmitter submitter;
        FakeConnectionProvider connectionProvider;
        UndoManager undoManager;
        QDateTime currentTime =
            QDateTime::fromString(QStringLiteral("2026-08-05T00:00:00.000Z"), Qt::ISODateWithMs);
        DeferredSendService service;

        Fixture()
            : database(openDatabase(directory.filePath(QStringLiteral("deferred.sqlite3")))),
              historyRepository(database), repository(database), undoManager(historyRepository),
              service(repository, submitter, connectionProvider, undoManager,
                      [this] { return currentTime; })
        {
            REQUIRE(directory.isValid());
            REQUIRE_FALSE(undoManager.load().has_value());
        }

        PendingSend seed(const QString& sendId, const DeferredSendStatus status,
                         const QDateTime& dueAt)
        {
            const auto history = addHistory(historyRepository, sendId);
            auto send = pending(sendId, history.entryId, status, dueAt);
            REQUIRE_FALSE(repository.insert(send));
            REQUIRE_FALSE(undoManager.load().has_value());
            return send;
        }
    };

    void settleCoroutines()
    {
        for (int iteration = 0; iteration < 3; ++iteration)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    }

    void dispatchNow(DeferredSendService& service)
    {
        REQUIRE(QMetaObject::invokeMethod(&service, "dispatchDue", Qt::DirectConnection));
        settleCoroutines();
    }

    [[nodiscard]] PendingSend requireSend(DeferredSendRepository& repository, const QString& sendId)
    {
        const auto found = repository.find(sendId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&found))
            FAIL(error->message.toStdString());
        const auto send = std::get<std::optional<PendingSend>>(found);
        REQUIRE(send.has_value());
        return *send;
    }
} // namespace

TEST_CASE("a gated due send waits until its notification window ends",
          "[app][deferred-send][service][notification]")
{
    Fixture fixture;
    fixture.seed(QStringLiteral("gated"), DeferredSendStatus::Scheduled,
                 fixture.currentTime.addSecs(-1));

    fixture.service.notificationWindowPresented(QStringLiteral("gated"));
    dispatchNow(fixture.service);
    CHECK(fixture.submitter.attempts == 0);

    fixture.service.notificationWindowEnded(QStringLiteral("gated"));
    settleCoroutines();
    CHECK(fixture.submitter.attempts == 1);
    CHECK(requireSend(fixture.repository, QStringLiteral("gated")).status ==
          DeferredSendStatus::Submitted);
    CHECK(requireSend(fixture.repository, QStringLiteral("gated")).dueAt ==
          fixture.currentTime.addSecs(-1));
}

TEST_CASE("early notification dismissal persists an immediate deadline",
          "[app][deferred-send][service][notification]")
{
    Fixture fixture;
    const auto originalDueAt = fixture.currentTime.addSecs(30);
    fixture.seed(QStringLiteral("early"), DeferredSendStatus::Scheduled, originalDueAt);
    fixture.service.notificationWindowPresented(QStringLiteral("early"));

    fixture.service.notificationWindowEnded(QStringLiteral("early"));
    settleCoroutines();

    const auto send = requireSend(fixture.repository, QStringLiteral("early"));
    CHECK(send.dueAt == fixture.currentTime);
    CHECK(send.dueAt < originalDueAt);
    CHECK(fixture.submitter.attempts == 1);
}

TEST_CASE("an ungated send remains timer-driven when notification presentation fails",
          "[app][deferred-send][service][notification]")
{
    Fixture fixture;
    fixture.seed(QStringLiteral("ungated"), DeferredSendStatus::Scheduled,
                 fixture.currentTime.addSecs(-1));

    dispatchNow(fixture.service);
    CHECK(fixture.submitter.attempts == 1);
    CHECK(requireSend(fixture.repository, QStringLiteral("ungated")).status ==
          DeferredSendStatus::Submitted);
}

TEST_CASE("a gated send does not block a later due ungated send",
          "[app][deferred-send][service][notification]")
{
    Fixture fixture;
    fixture.seed(QStringLiteral("gated"), DeferredSendStatus::Scheduled,
                 fixture.currentTime.addSecs(-2));
    fixture.seed(QStringLiteral("ungated"), DeferredSendStatus::Scheduled,
                 fixture.currentTime.addSecs(-1));
    fixture.service.notificationWindowPresented(QStringLiteral("gated"));

    dispatchNow(fixture.service);
    CHECK(fixture.submitter.attempts == 1);
    CHECK(requireSend(fixture.repository, QStringLiteral("ungated")).status ==
          DeferredSendStatus::Submitted);
    CHECK(requireSend(fixture.repository, QStringLiteral("gated")).status ==
          DeferredSendStatus::Scheduled);
}

TEST_CASE("Undo before notification closure cancels without submitting",
          "[app][deferred-send][service][notification]")
{
    Fixture fixture;
    fixture.seed(QStringLiteral("cancelled"), DeferredSendStatus::Scheduled,
                 fixture.currentTime.addSecs(-1));
    fixture.service.notificationWindowPresented(QStringLiteral("cancelled"));

    const auto cancelled = fixture.service.cancelTargeted(QStringLiteral("cancelled"));
    REQUIRE(std::holds_alternative<bool>(cancelled));
    CHECK(std::get<bool>(cancelled));
    fixture.service.notificationWindowEnded(QStringLiteral("cancelled"));
    dispatchNow(fixture.service);

    CHECK(fixture.submitter.attempts == 0);
}

TEST_CASE("waiting states keep their retry timer independent of Undo notifications",
          "[app][deferred-send][service][notification]")
{
    SECTION("network")
    {
        Fixture fixture;
        fixture.seed(QStringLiteral("network"), DeferredSendStatus::WaitingForNetwork,
                     fixture.currentTime.addSecs(-1));
        fixture.submitter.invokeDispatched = false;
        fixture.submitter.result = javelin::jmap::OperationError{
            .code = javelin::jmap::OperationErrorCode::NetworkUnavailable,
            .message = QStringLiteral("offline"),
        };
        fixture.service.start();
        dispatchNow(fixture.service);
        CHECK(fixture.submitter.attempts == 1);
        CHECK(requireSend(fixture.repository, QStringLiteral("network")).status ==
              DeferredSendStatus::WaitingForNetwork);
    }

    SECTION("authentication")
    {
        Fixture fixture;
        fixture.seed(QStringLiteral("auth"), DeferredSendStatus::WaitingForAuth,
                     fixture.currentTime.addSecs(-1));
        fixture.connectionProvider.settings = std::nullopt;
        fixture.service.start();
        dispatchNow(fixture.service);
        CHECK(fixture.submitter.attempts == 0);
        CHECK(requireSend(fixture.repository, QStringLiteral("auth")).status ==
              DeferredSendStatus::WaitingForAuth);
    }
}

TEST_CASE("startup recreates future scheduled notifications with remaining time",
          "[app][deferred-send][service][notification]")
{
    Fixture fixture;
    fixture.seed(QStringLiteral("future"), DeferredSendStatus::Scheduled,
                 fixture.currentTime.addSecs(30));
    std::optional<int> timeout;
    QObject::connect(&fixture.service, &DeferredSendService::undoableSendScheduled,
                     &fixture.service,
                     [&timeout](const QString&, const QString&, const QString&, const int value)
                     { timeout = value; });

    fixture.service.start();
    REQUIRE(timeout.has_value());
    CHECK(*timeout == 30'000);
    CHECK(fixture.submitter.attempts == 0);
}

TEST_CASE("startup dispatches overdue scheduled sends and recovers interrupted dispatch",
          "[app][deferred-send][service][notification]")
{
    SECTION("overdue")
    {
        Fixture fixture;
        fixture.seed(QStringLiteral("overdue"), DeferredSendStatus::Scheduled,
                     fixture.currentTime.addSecs(-1));
        fixture.service.start();
        settleCoroutines();
        CHECK(fixture.submitter.attempts == 1);
        CHECK(requireSend(fixture.repository, QStringLiteral("overdue")).status ==
              DeferredSendStatus::Submitted);
    }

    SECTION("interrupted dispatch")
    {
        Fixture fixture;
        const auto send = fixture.seed(QStringLiteral("interrupted"), DeferredSendStatus::Scheduled,
                                       fixture.currentTime.addSecs(-1));
        REQUIRE(std::holds_alternative<bool>(
            fixture.repository.claimForDispatch(send.sendId, fixture.currentTime)));
        fixture.service.start();
        dispatchNow(fixture.service);
        CHECK(fixture.submitter.attempts == 0);
        CHECK(requireSend(fixture.repository, QStringLiteral("interrupted")).status ==
              DeferredSendStatus::Unknown);
    }
}

TEST_CASE("a daemon restart does not restore an in-memory notification gate",
          "[app][deferred-send][service][notification]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("restart.sqlite3"));
    const auto now =
        QDateTime::fromString(QStringLiteral("2026-08-05T00:00:00.000Z"), Qt::ISODateWithMs);

    auto database = openDatabase(path);
    HistoryRepository historyRepository{database};
    DeferredSendRepository repository{database};
    ScriptedSubmitter submitter;
    FakeConnectionProvider connectionProvider;
    UndoManager undoManager{historyRepository};
    REQUIRE_FALSE(undoManager.load().has_value());
    const auto history = addHistory(historyRepository, QStringLiteral("restart"));
    REQUIRE_FALSE(repository.insert(pending(QStringLiteral("restart"), history.entryId,
                                            DeferredSendStatus::Scheduled, now.addSecs(-1))));
    static_cast<void>(undoManager.load());

    {
        DeferredSendService first{repository, submitter, connectionProvider, undoManager,
                                  [now] { return now; }};
        first.notificationWindowPresented(QStringLiteral("restart"));
        dispatchNow(first);
        CHECK(submitter.attempts == 0);
    }

    DeferredSendService second{repository, submitter, connectionProvider, undoManager,
                               [now] { return now; }};
    second.start();
    settleCoroutines();
    CHECK(submitter.attempts == 1);
    CHECK(requireSend(repository, QStringLiteral("restart")).status ==
          DeferredSendStatus::Submitted);
}

TEST_CASE("redo schedules a fresh notification window",
          "[app][deferred-send][service][notification]")
{
    Fixture fixture;
    const auto history = fixture.seed(QStringLiteral("redo"), DeferredSendStatus::Cancelled,
                                      fixture.currentTime.addSecs(-1));
    std::optional<int> timeout;
    QObject::connect(&fixture.service, &DeferredSendService::undoableSendScheduled,
                     &fixture.service,
                     [&timeout](const QString&, const QString&, const QString&, const int value)
                     { timeout = value; });

    const auto result = QCoro::waitFor(fixture.service.execute(
        *std::ranges::find_if(fixture.undoManager.entries(), [&history](const HistoryEntry& entry)
                              { return entry.entryId == history.historyEntryId; }),
        HistoryExecutionDirection::Redo));
    REQUIRE(result.outcome == HistoryExecutionOutcome::Success);
    REQUIRE(timeout.has_value());
    CHECK(*timeout == 30'000);
    CHECK(requireSend(fixture.repository, QStringLiteral("redo")).status ==
          DeferredSendStatus::Scheduled);

    fixture.service.notificationWindowPresented(QStringLiteral("redo"));
    dispatchNow(fixture.service);
    CHECK(fixture.submitter.attempts == 0);
}
