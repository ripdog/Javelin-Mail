#include "jmap/sync/MailDeltaRefreshExecutor.h"

#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/MethodEnvelope.h"
#include "jmap/api/Transport.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailboxRepository.h"
#include "jmap/cache/MailboxWindowRepository.h"
#include "jmap/cache/SyncStateRepository.h"
#include "jmap/sync/EmailMutationJournal.h"
#include "jmap/sync/MailboxQueryDescriptor.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace
{

    class ApplicationGuard
    {
      public:
        ApplicationGuard()
        {
            if (QCoreApplication::instance() != nullptr)
                return;
            static int argc = 1;
            static char appName[] = "javelin-tests";
            static char* argv[] = {appName, nullptr};
            m_application = std::make_unique<QCoreApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };

    class FakeTransport final : public javelin::jmap::api::AbstractTransport
    {
      public:
        FakeTransport() : methodTransport(*this)
        {
        }

        [[nodiscard]] operator javelin::jmap::api::JmapMethodTransport&()
        {
            return methodTransport;
        }

        javelin::jmap::api::HttpJmapMethodTransport methodTransport;
        std::vector<javelin::jmap::api::HttpRequest> requests;
        std::vector<javelin::jmap::api::TransportResult> queuedResults;

        [[nodiscard]] QCoro::Task<javelin::jmap::api::TransportResult>
        send(javelin::jmap::api::HttpRequest request) override
        {
            requests.push_back(request);
            REQUIRE_FALSE(queuedResults.empty());
            auto result = std::move(queuedResults.front());
            queuedResults.erase(queuedResults.begin());
            co_return result;
        }
    };

    struct TestDatabaseContext
    {
        QTemporaryDir temporaryDir;
        javelin::jmap::cache::DatabaseConnection connection;
    };

    [[nodiscard]] TestDatabaseContext makeDatabaseContext()
    {
        static int counter = 0;
        TestDatabaseContext context;
        REQUIRE(context.temporaryDir.isValid());
        auto opened = javelin::jmap::cache::DatabaseConnection::open({
            .connectionName = QStringLiteral("mail-delta-refresh-%1").arg(++counter),
            .databasePath = context.temporaryDir.filePath(QStringLiteral("cache.sqlite3")),
        });
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&opened))
            FAIL(error->message.toStdString());
        context.connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
        return context;
    }

    void seedAccount(javelin::jmap::cache::DatabaseConnection& connection)
    {
        QSqlQuery query{connection.database()};
        query.prepare(
            QStringLiteral("INSERT INTO accounts(account_id,email_address,session_url,is_primary) "
                           "VALUES('account-1','alice@example.com','https://example.com/jmap',1)"));
        REQUIRE(query.exec());
    }

    [[nodiscard]] javelin::jmap::api::ApiRequestContext requestContext()
    {
        return {
            .credentials =
                {
                    .accountId = "account-1",
                    .emailAddress = "alice@example.com",
                    .sessionUrl = "https://example.com/jmap",
                    .token =
                        {
                            .accessToken = "token",
                            .refreshToken = std::nullopt,
                            .expiry = std::nullopt,
                        },
                },
            .apiUrl = "https://example.com/api",
        };
    }

    [[nodiscard]] javelin::jmap::domain::Mailbox mailbox(const std::string& id,
                                                         const std::uint64_t unread)
    {
        return {
            .id = id,
            .name = id,
            .parentId = std::nullopt,
            .role = std::nullopt,
            .sortOrder = 0,
            .totalEmails = 1,
            .unreadEmails = unread,
            .totalThreads = 1,
            .unreadThreads = unread,
            .isSubscribed = true,
            .myRights = {},
        };
    }

    [[nodiscard]] javelin::jmap::domain::Email email(std::vector<std::string> mailboxIds,
                                                     std::vector<std::string> keywords)
    {
        return {
            .id = "email-1",
            .blobId = "blob-1",
            .threadId = "thread-1",
            .mailboxIds = std::move(mailboxIds),
            .keywords = std::move(keywords),
            .size = 42,
            .receivedAt = "2026-07-28T01:00:00Z",
            .sentAt = std::nullopt,
            .messageId = {},
            .inReplyTo = {},
            .references = {},
            .hasAttachment = false,
            .subject = "Subject",
            .from = {},
            .to = {},
            .cc = {},
            .bcc = {},
            .replyTo = {},
            .preview = "Preview",
        };
    }

    [[nodiscard]] std::string emailJson(const std::string& mailboxId, const bool seen)
    {
        return std::string{
                   R"({"id":"email-1","blobId":"blob-1","threadId":"thread-1","mailboxIds":{")"} +
               mailboxId + R"(":true},"keywords":{)" +
               (seen ? std::string{R"("$seen":true)"} : std::string{}) +
               R"(},"size":42,"receivedAt":"2026-07-28T01:00:00Z","subject":"Subject","preview":"Preview"})";
    }

    [[nodiscard]] std::string mailboxJson(const std::string& id, const std::uint64_t unread)
    {
        return std::string{R"({"id":")"} + id + R"(","name":")" + id +
               R"(","sortOrder":0,"totalEmails":1,"unreadEmails":)" + std::to_string(unread) +
               R"(,"totalThreads":1,"unreadThreads":)" + std::to_string(unread) +
               R"(,"isSubscribed":true,"myRights":{"mayReadItems":true}})";
    }

    [[nodiscard]] std::string getArguments(const std::string& state, const std::string& objects)
    {
        return std::string{R"({"accountId":"account-1","state":")"} + state + R"(","list":[)" +
               objects + R"(],"notFound":[]})";
    }

    [[nodiscard]] std::string changesArguments(const std::string& oldState,
                                               const std::string& newState,
                                               const std::string& created,
                                               const std::string& updated,
                                               const std::string& destroyed)
    {
        return std::string{R"({"accountId":"account-1","oldState":")"} + oldState +
               R"(","newState":")" + newState + R"(","hasMoreChanges":false,"created":[)" +
               created + R"(],"updated":[)" + updated + R"(],"destroyed":[)" + destroyed + R"(]})";
    }

    [[nodiscard]] javelin::jmap::api::TransportResult
    deltaResponse(const std::string& updatedMailboxId, const bool seen)
    {
        const auto envelope = javelin::jmap::api::ResponseEnvelope{
            .methodResponses =
                {
                    {.name = "Mailbox/changes",
                     .arguments = changesArguments("mailbox-state-1", "mailbox-state-2", {},
                                                   R"("archive")", {}),
                     .callId = "mailbox-changes"},
                    {.name = "Mailbox/get",
                     .arguments = getArguments("mailbox-state-2", {}),
                     .callId = "created-mailboxes"},
                    {.name = "Mailbox/get",
                     .arguments =
                         getArguments("mailbox-state-2", mailboxJson("archive", seen ? 0 : 1)),
                     .callId = "updated-mailboxes"},
                    {.name = "Email/changes",
                     .arguments =
                         changesArguments("email-state-1", "email-state-2", {}, R"("email-1")", {}),
                     .callId = "email-changes"},
                    {.name = "Email/get",
                     .arguments = getArguments("email-state-2", {}),
                     .callId = "created-emails"},
                    {.name = "Email/get",
                     .arguments = getArguments("email-state-2", emailJson(updatedMailboxId, seen)),
                     .callId = "updated-emails"},
                },
            .createdIds = std::nullopt,
            .sessionState = "session-state",
        };
        const auto serialized = javelin::jmap::api::serializeResponseEnvelope(envelope);
        REQUIRE(serialized.has_value());
        return javelin::jmap::api::HttpResponse{
            .statusCode = 200,
            .body = QByteArray::fromStdString(*serialized),
        };
    }

    void seedMail(javelin::jmap::cache::DatabaseConnection& connection)
    {
        javelin::jmap::cache::MailboxRepository mailboxes{connection};
        REQUIRE_FALSE(
            mailboxes.upsertMany("account-1", {mailbox("inbox", 0), mailbox("archive", 1)})
                .has_value());
        javelin::jmap::cache::EmailRepository emails{connection};
        REQUIRE_FALSE(emails.upsertMany("account-1", {email({"archive"}, {})}).has_value());
        javelin::jmap::cache::SyncStateRepository states{connection};
        REQUIRE_FALSE(
            states
                .upsert({.accountId = "account-1", .objectType = "Mailbox", .queryKey = {}},
                        "mailbox-state-1")
                .has_value());
        REQUIRE_FALSE(states
                          .upsert({.accountId = "account-1", .objectType = "Email", .queryKey = {}},
                                  "email-state-1")
                          .has_value());
    }

} // namespace

TEST_CASE("account mail delta applies an external seen change without invalidating mailbox queries",
          "[jmap][sync][mail-delta]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeDatabaseContext();
    seedAccount(database.connection);
    seedMail(database.connection);
    const auto archiveQueryKey = javelin::jmap::sync::mailboxQueryKey({
        .mailboxId = "archive",
        .sortProperty = "receivedAt",
        .isAscending = false,
        .collapseThreads = true,
    });
    javelin::jmap::cache::MailboxWindowRepository windows{database.connection};
    REQUIRE_FALSE(windows
                      .replace({
                          .accountId = "account-1",
                          .mailboxId = "archive",
                          .queryKey = archiveQueryKey,
                          .requestedOffset = 0,
                          .requestedLimit = 100,
                          .position = 0,
                          .returnedLimit = 1,
                          .total = 1,
                          .queryState = "archive-query-state",
                          .coverage = javelin::jmap::cache::QueryWindowCoverage::LocallyProjected,
                          .emailIds = {"email-1"},
                      })
                      .has_value());
    auto projectionResult = javelin::jmap::cache::DatabaseTransaction::begin(
        database.connection, QStringLiteral("Project archive test window"));
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseTransaction>(projectionResult));
    auto projection =
        std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(projectionResult));
    REQUIRE_FALSE(
        windows
            .invalidateMailbox(projection, "account-1", "archive",
                               javelin::jmap::cache::QueryWindowCoverage::LocallyProjected)
            .has_value());
    REQUIRE_FALSE(projection.commit().has_value());

    FakeTransport transport;
    transport.queuedResults.push_back(deltaResponse("archive", true));
    javelin::jmap::api::MethodCaller caller{transport};
    javelin::jmap::sync::MailDeltaRefreshExecutor executor{database.connection, caller,
                                                           requestContext()};
    const auto result =
        QCoro::waitFor(executor.refresh("account-1", {.mailbox = true, .email = true}));

    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailDeltaRefreshSummary>(result));
    const auto& summary = std::get<javelin::jmap::sync::MailDeltaRefreshSummary>(result);
    CHECK(summary.mailboxChanged);
    CHECK(summary.emailChanged);
    CHECK(summary.changedMailboxIds == std::vector<std::string>{"archive"});
    CHECK(summary.queryAffectedMailboxIds.empty());
    REQUIRE(transport.requests.size() == 1);
    CHECK(transport.requests.front().body.contains("\"Mailbox/changes\""));
    CHECK(transport.requests.front().body.contains("\"Email/changes\""));
    CHECK_FALSE(transport.requests.front().body.contains("\"Email/query\""));
    CHECK_FALSE(transport.requests.front().body.contains("\"Email/queryChanges\""));

    javelin::jmap::cache::EmailRepository emails{database.connection};
    const auto cachedResult = emails.find("account-1", "email-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(cachedResult));
    const auto& cached = std::get<std::optional<javelin::jmap::domain::Email>>(cachedResult);
    REQUIRE(cached.has_value());
    CHECK(cached->keywords == std::vector<std::string>{"$seen"});
    const auto windowResult = windows.find("account-1", archiveQueryKey, 0, 100);
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::MailboxWindowRecord>>(
        windowResult));
    const auto& window =
        std::get<std::optional<javelin::jmap::cache::MailboxWindowRecord>>(windowResult);
    REQUIRE(window.has_value());
    CHECK(window->coverage == javelin::jmap::cache::QueryWindowCoverage::LocallyProjected);
    CHECK(window->queryState == "archive-query-state");
}

TEST_CASE("account mail delta targets only old and new mailboxes for an external move",
          "[jmap][sync][mail-delta]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeDatabaseContext();
    seedAccount(database.connection);
    seedMail(database.connection);

    javelin::jmap::cache::EmailRepository emails{database.connection};
    REQUIRE_FALSE(emails.upsertMany("account-1", {email({"inbox"}, {})}).has_value());

    FakeTransport transport;
    transport.queuedResults.push_back(deltaResponse("archive", false));
    javelin::jmap::api::MethodCaller caller{transport};
    javelin::jmap::sync::MailDeltaRefreshExecutor executor{database.connection, caller,
                                                           requestContext()};
    const auto result =
        QCoro::waitFor(executor.refresh("account-1", {.mailbox = true, .email = true}));

    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailDeltaRefreshSummary>(result));
    const auto& summary = std::get<javelin::jmap::sync::MailDeltaRefreshSummary>(result);
    CHECK(summary.queryAffectedMailboxIds == std::vector<std::string>{"archive", "inbox"});
    REQUIRE(transport.requests.size() == 1);
}

TEST_CASE("account mail delta rebases retained accepted overlays over an external unread change",
          "[jmap][sync][mail-delta]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeDatabaseContext();
    seedAccount(database.connection);
    seedMail(database.connection);

    javelin::jmap::cache::EmailRepository emails{database.connection};
    REQUIRE_FALSE(emails.upsertMany("account-1", {email({"archive"}, {"$seen"})}).has_value());
    javelin::jmap::sync::EmailMutationJournal journal{database.connection};
    REQUIRE_FALSE(journal
                      .put({
                          .mutationId = "accepted-archive-overlay",
                          .operationGroupId = std::nullopt,
                          .accountId = "account-1",
                          .status = javelin::jmap::sync::MutationStatus::Accepted,
                          .patch =
                              {
                                  .emailId = "email-1",
                                  .addMailboxIds = {"archive"},
                                  .removeMailboxIds = {},
                                  .addKeywords = {},
                                  .removeKeywords = {},
                                  .destroy = false,
                              },
                          .baseMailboxIds = std::vector<std::string>{"archive"},
                          .baseKeywords = std::vector<std::string>{"$seen"},
                          .baseState = "email-state-1",
                          .acceptedState = "email-state-1",
                          .errorJson = std::nullopt,
                      })
                      .has_value());

    FakeTransport transport;
    transport.queuedResults.push_back(deltaResponse("archive", false));
    javelin::jmap::api::MethodCaller caller{transport};
    javelin::jmap::sync::MailDeltaRefreshExecutor executor{database.connection, caller,
                                                           requestContext()};
    const auto result =
        QCoro::waitFor(executor.refresh("account-1", {.mailbox = true, .email = true}));

    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailDeltaRefreshSummary>(result));
    const auto& summary = std::get<javelin::jmap::sync::MailDeltaRefreshSummary>(result);
    CHECK_FALSE(summary.superseded);
    CHECK(summary.emailChanged);
    CHECK(summary.queryAffectedMailboxIds.empty());
    REQUIRE(transport.requests.size() == 1);

    const auto cachedResult = emails.find("account-1", "email-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(cachedResult));
    const auto& cached = std::get<std::optional<javelin::jmap::domain::Email>>(cachedResult);
    REQUIRE(cached.has_value());
    CHECK(cached->keywords.empty());

    javelin::jmap::cache::SyncStateRepository states{database.connection};
    const auto stateResult =
        states.find({.accountId = "account-1", .objectType = "Email", .queryKey = {}});
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::cache::SyncStateRecord>>(stateResult));
    const auto& state = std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(stateResult);
    REQUIRE(state.has_value());
    CHECK(state->stateToken == "email-state-2");
}
