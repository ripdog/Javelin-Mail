#include "jmap/sync/MailDeltaRefreshExecutor.h"

#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/MethodEnvelope.h"
#include "jmap/api/Transport.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailboxMessageReadRepository.h"
#include "jmap/cache/MailboxRepository.h"
#include "jmap/cache/MailboxWindowRepository.h"
#include "jmap/cache/NotificationRepository.h"
#include "jmap/cache/QueryWindowReadRepository.h"
#include "jmap/cache/SyncStateRepository.h"
#include "jmap/cache/ThreadRepository.h"
#include "jmap/sync/ConsistencyDomain.h"
#include "jmap/sync/EmailMutationJournal.h"
#include "jmap/sync/MailboxQueryDescriptor.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <functional>
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
        std::function<void(std::size_t)> onSend;

        [[nodiscard]] QCoro::Task<javelin::jmap::api::TransportResult>
        send(javelin::jmap::api::HttpRequest request) override
        {
            requests.push_back(request);
            if (onSend)
                onSend(requests.size());
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
            .requestLimits = std::nullopt,
        };
    }

    [[nodiscard]] javelin::jmap::domain::Mailbox
    mailbox(const std::string& id, const std::uint64_t unread,
            std::optional<std::string> role = std::nullopt)
    {
        return {
            .id = id,
            .name = id,
            .parentId = std::nullopt,
            .role = std::move(role),
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

    [[nodiscard]] std::string
    emailJsonWithIdentityAndMailboxes(const std::string& emailId, const std::string& threadId,
                                      const std::vector<std::string>& mailboxIds, const bool seen)
    {
        std::string result = "{\"id\":\"" + emailId + "\",\"blobId\":\"blob-" + emailId +
                             "\",\"threadId\":\"" + threadId + "\",\"mailboxIds\":{";
        for (std::size_t index = 0; index < mailboxIds.size(); ++index)
        {
            if (index != 0)
                result += ',';
            result += "\"" + mailboxIds[index] + "\":true";
        }
        result += "},\"keywords\":{";
        if (seen)
            result += "\"$seen\":true";
        result += "},\"size\":42,\"receivedAt\":\"2026-07-28T01:00:00Z\","
                  "\"subject\":\"Subject\",\"preview\":\"Preview\"}";
        return result;
    }

    [[nodiscard]] std::string emailJsonWithIdentity(const std::string& emailId,
                                                    const std::string& threadId,
                                                    const std::string& mailboxId, const bool seen)
    {
        return std::string{R"({"id":")"} + emailId + R"(","blobId":"blob-)" + emailId +
               R"(","threadId":")" + threadId + R"(","mailboxIds":{")" + mailboxId +
               R"(":true},"keywords":{)" + (seen ? std::string{R"("$seen":true)"} : std::string{}) +
               R"(},"size":42,"receivedAt":"2026-07-28T01:00:00Z","subject":"Subject","preview":"Preview"})";
    }

    [[nodiscard]] std::string emailJson(const std::string& mailboxId, const bool seen)
    {
        return emailJsonWithIdentity("email-1", "thread-1", mailboxId, seen);
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

    [[nodiscard]] javelin::jmap::api::TransportResult deltaResponse(const bool seen)
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

    [[nodiscard]] javelin::jmap::api::TransportResult
    updatedEmailResponseAtState(const std::string& state,
                                const std::vector<std::string>& updatedMailboxIds, const bool seen)
    {
        const auto envelope = javelin::jmap::api::ResponseEnvelope{
            .methodResponses =
                {
                    {.name = "Email/get",
                     .arguments =
                         getArguments(state, emailJsonWithIdentityAndMailboxes(
                                                 "email-1", "thread-1", updatedMailboxIds, seen)),
                     .callId = "relevant-updated-emails"},
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

    [[nodiscard]] javelin::jmap::api::TransportResult
    updatedEmailResponse(const std::string& updatedMailboxId, const bool seen)
    {
        const auto envelope = javelin::jmap::api::ResponseEnvelope{
            .methodResponses =
                {
                    {.name = "Email/get",
                     .arguments = getArguments("email-state-2", emailJson(updatedMailboxId, seen)),
                     .callId = "relevant-updated-emails"},
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

    [[nodiscard]] javelin::jmap::api::TransportResult
    emailDeltaResponseAtStates(const std::string& oldState, const std::string& newState,
                               const std::string& created, const std::string& updated,
                               const std::string& destroyed, const std::string& createdObjects = {})
    {
        const auto envelope = javelin::jmap::api::ResponseEnvelope{
            .methodResponses =
                {
                    {.name = "Email/changes",
                     .arguments = changesArguments(oldState, newState, created, updated, destroyed),
                     .callId = "email-changes"},
                    {.name = "Email/get",
                     .arguments = getArguments(newState, createdObjects),
                     .callId = "created-emails"},
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

    [[nodiscard]] javelin::jmap::api::TransportResult
    emailDeltaResponse(const std::string& created, const std::string& updated,
                       const std::string& destroyed, const std::string& createdObjects = {})
    {
        const auto envelope = javelin::jmap::api::ResponseEnvelope{
            .methodResponses =
                {
                    {.name = "Email/changes",
                     .arguments = changesArguments("email-state-1", "email-state-2", created,
                                                   updated, destroyed),
                     .callId = "email-changes"},
                    {.name = "Email/get",
                     .arguments = getArguments("email-state-2", createdObjects),
                     .callId = "created-emails"},
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

    [[nodiscard]] javelin::jmap::api::TransportResult
    recoverableEmailDeltaResponse(const std::string_view errorType)
    {
        const auto envelope = javelin::jmap::api::ResponseEnvelope{
            .methodResponses =
                {
                    {.name = "error",
                     .arguments = std::string{R"({"type":")"} + std::string{errorType} +
                                  R"(","description":"delta unavailable"})",
                     .callId = "email-changes"},
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

    [[nodiscard]] javelin::jmap::api::TransportResult
    rebaselineEmailResponse(const std::string& state, const std::string& objects,
                            const std::string& notFound = {},
                            const std::string& callId = "email-rebaseline-0")
    {
        const auto envelope = javelin::jmap::api::ResponseEnvelope{
            .methodResponses =
                {
                    {.name = "Email/get",
                     .arguments = std::string{R"({"accountId":"account-1","state":")"} + state +
                                  R"(","list":[)" + objects + R"(],"notFound":[)" + notFound +
                                  R"(]})",
                     .callId = callId},
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

    [[nodiscard]] javelin::jmap::api::TransportResult
    twoRebaselineEmailResponse(const std::string& firstState, const std::string& firstObject,
                               const std::string& secondState, const std::string& secondObject)
    {
        const auto envelope = javelin::jmap::api::ResponseEnvelope{
            .methodResponses =
                {
                    {.name = "Email/get",
                     .arguments = std::string{R"({"accountId":"account-1","state":")"} +
                                  firstState + R"(","list":[)" + firstObject +
                                  R"(],"notFound":[]})",
                     .callId = "email-rebaseline-0"},
                    {.name = "Email/get",
                     .arguments = std::string{R"({"accountId":"account-1","state":")"} +
                                  secondState + R"(","list":[)" + secondObject +
                                  R"(],"notFound":[]})",
                     .callId = "email-rebaseline-1"},
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

    void seedEmailState(javelin::jmap::cache::DatabaseConnection& connection)
    {
        javelin::jmap::cache::SyncStateRepository states{connection};
        REQUIRE_FALSE(states
                          .upsert({.accountId = "account-1", .objectType = "Email", .queryKey = {}},
                                  "email-state-1")
                          .has_value());
    }

    void seedNotificationMailboxes(javelin::jmap::cache::DatabaseConnection& connection,
                                   std::vector<std::string> mailboxIds)
    {
        javelin::jmap::cache::NotificationRepository notifications{connection};
        REQUIRE_FALSE(notifications.replaceActiveMailboxes("account-1", mailboxIds).has_value());
    }

    void seedImportProvenance(javelin::jmap::cache::DatabaseConnection& connection,
                              const std::string_view emailId)
    {
        QSqlQuery operation{connection.database()};
        REQUIRE(operation.exec(QStringLiteral(
            "INSERT INTO mail_import_operations(operation_id,account_id,mailbox_id,"
            "source_paths_json,recreate_hierarchy,status,scan_sealed,title,created_at) VALUES "
            "('import-1','account-1','inbox','[]',0,'complete',1,'Import','2026-08-27T00:00:00Z'"
            ")")));
        QSqlQuery item{connection.database()};
        item.prepare(QStringLiteral(
            "INSERT INTO mail_import_items(item_id,operation_id,ordinal,source_path,source_kind,"
            "decoded_size,source_canonical_path,source_size,source_mtime_ms,phase,created_email_id)"
            " "
            "VALUES('import-item-1','import-1',0,'message.eml','eml',1,'/tmp/message.eml',1,1,"
            "'created',:email_id)"));
        item.bindValue(QStringLiteral(":email_id"), QString::fromStdString(std::string{emailId}));
        REQUIRE(item.exec());
    }

    void seedImportProvenanceBatch(javelin::jmap::cache::DatabaseConnection& connection,
                                   const std::vector<std::string>& emailIds)
    {
        QSqlQuery operation{connection.database()};
        REQUIRE(operation.exec(QStringLiteral(
            "INSERT INTO mail_import_operations(operation_id,account_id,mailbox_id,"
            "source_paths_json,recreate_hierarchy,status,scan_sealed,title,created_at) VALUES "
            "('import-batch','account-1','inbox','[]',0,'complete',1,'Batch Import',"
            "'2026-08-27T00:00:00Z')")));

        QSqlQuery item{connection.database()};
        item.prepare(QStringLiteral(
            "INSERT INTO mail_import_items(item_id,operation_id,ordinal,source_path,source_kind,"
            "decoded_size,source_canonical_path,source_size,source_mtime_ms,phase,created_email_id)"
            " "
            "VALUES(:item_id,'import-batch',:ordinal,:source_path,'eml',1,:canonical_path,1,1,"
            "'created',:email_id)"));
        for (std::size_t index = 0; index < emailIds.size(); ++index)
        {
            const auto ordinal = static_cast<qulonglong>(index);
            const auto itemId = QStringLiteral("import-batch-item-%1").arg(ordinal);
            const auto sourcePath = QStringLiteral("message-%1.eml").arg(ordinal);
            item.bindValue(QStringLiteral(":item_id"), itemId);
            item.bindValue(QStringLiteral(":ordinal"), ordinal);
            item.bindValue(QStringLiteral(":source_path"), sourcePath);
            item.bindValue(QStringLiteral(":canonical_path"),
                           QStringLiteral("/tmp/%1").arg(sourcePath));
            item.bindValue(QStringLiteral(":email_id"), QString::fromStdString(emailIds[index]));
            REQUIRE(item.exec());
        }
    }

    void seedMail(javelin::jmap::cache::DatabaseConnection& connection)
    {
        javelin::jmap::cache::MailboxRepository mailboxes{connection};
        REQUIRE_FALSE(
            mailboxes.upsertMany("account-1", {mailbox("inbox", 0, "inbox"), mailbox("archive", 1)})
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

    [[nodiscard]] std::string
    seedMailboxWindow(javelin::jmap::cache::DatabaseConnection& connection,
                      const std::string& mailboxId)
    {
        const auto queryKey = javelin::jmap::sync::mailboxQueryKey({
            .mailboxId = mailboxId,
            .sortProperty = "receivedAt",
            .isAscending = false,
            .collapseThreads = true,
        });
        javelin::jmap::cache::MailboxWindowRepository windows{connection};
        REQUIRE_FALSE(windows
                          .replace({
                              .accountId = "account-1",
                              .mailboxId = mailboxId,
                              .queryKey = queryKey,
                              .requestedOffset = 0,
                              .requestedLimit = 100,
                              .position = 0,
                              .returnedLimit = 1,
                              .total = 1,
                              .queryState = mailboxId + "-query-state",
                              .coverage = javelin::jmap::cache::QueryWindowCoverage::Server,
                              .emailIds = {"email-1"},
                          })
                          .has_value());
        return queryKey;
    }

} // namespace

TEST_CASE("account Email rebaseline reconciles cached mail before advancing state",
          "[jmap][sync][mail-delta][rebaseline]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    const auto runCase = [](const std::string_view errorType)
    {
        auto database = makeDatabaseContext();
        seedAccount(database.connection);
        seedEmailState(database.connection);
        javelin::jmap::cache::EmailRepository emails{database.connection};
        REQUIRE_FALSE(emails.upsertMany("account-1", {email({"archive"}, {})}).has_value());
        javelin::jmap::cache::NotificationRepository notifications{database.connection};
        REQUIRE_FALSE(notifications.replaceActiveMailboxes("account-1", {"inbox"}).has_value());

        FakeTransport transport;
        transport.queuedResults.push_back(recoverableEmailDeltaResponse(errorType));
        transport.queuedResults.push_back(
            rebaselineEmailResponse("email-state-2", emailJson("inbox", true)));
        javelin::jmap::api::MethodCaller caller{transport};
        javelin::jmap::sync::MailDeltaRefreshExecutor executor{database.connection, caller,
                                                               requestContext()};
        const auto result = QCoro::waitFor(executor.refresh("account-1", {.email = true}));

        REQUIRE(std::holds_alternative<javelin::jmap::sync::MailDeltaRefreshSummary>(result));
        const auto& summary = std::get<javelin::jmap::sync::MailDeltaRefreshSummary>(result);
        CHECK_FALSE(summary.emailNeedsFullRefresh);
        CHECK(summary.emailChanged);
        CHECK(summary.queryAffectedMailboxIds == std::vector<std::string>{"archive", "inbox"});
        REQUIRE(transport.requests.size() == 2);
        CHECK(transport.requests.front().body.contains("\"Email/changes\""));
        CHECK(transport.requests.back().body.contains("\"Email/get\""));
        CHECK(transport.requests.back().body.contains("email-1"));
        CHECK_FALSE(transport.requests.back().body.contains("\"Email/query\""));

        const auto cachedResult = emails.find("account-1", "email-1");
        REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(cachedResult));
        const auto& cached = std::get<std::optional<javelin::jmap::domain::Email>>(cachedResult);
        REQUIRE(cached.has_value());
        CHECK(cached->mailboxIds == std::vector<std::string>{"inbox"});
        CHECK(cached->keywords == std::vector<std::string>{"$seen"});

        javelin::jmap::cache::SyncStateRepository states{database.connection};
        const auto stateResult =
            states.find({.accountId = "account-1", .objectType = "Email", .queryKey = {}});
        REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::SyncStateRecord>>(
            stateResult));
        REQUIRE(std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(stateResult)
                    .has_value());
        CHECK(std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(stateResult)
                  ->stateToken == "email-state-2");
        const auto activeMailboxes = notifications.activeMailboxIds("account-1");
        REQUIRE(std::holds_alternative<std::vector<std::string>>(activeMailboxes));
        CHECK(std::get<std::vector<std::string>>(activeMailboxes) ==
              std::vector<std::string>{"inbox"});
    };

    SECTION("cannotCalculateChanges")
    {
        runCase("cannotCalculateChanges");
    }
    SECTION("tooManyChanges")
    {
        runCase("tooManyChanges");
    }
}

TEST_CASE("account Email rebaseline removes locally retained notFound mail before advancing state",
          "[jmap][sync][mail-delta][rebaseline]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeDatabaseContext();
    seedAccount(database.connection);
    seedEmailState(database.connection);
    javelin::jmap::cache::EmailRepository emails{database.connection};
    REQUIRE_FALSE(emails.upsertMany("account-1", {email({"archive"}, {})}).has_value());

    FakeTransport transport;
    transport.queuedResults.push_back(recoverableEmailDeltaResponse("cannotCalculateChanges"));
    transport.queuedResults.push_back(rebaselineEmailResponse("email-state-2", {}, R"("email-1")"));
    javelin::jmap::api::MethodCaller caller{transport};
    javelin::jmap::sync::MailDeltaRefreshExecutor executor{database.connection, caller,
                                                           requestContext()};
    const auto result = QCoro::waitFor(executor.refresh("account-1", {.email = true}));

    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailDeltaRefreshSummary>(result));
    const auto& summary = std::get<javelin::jmap::sync::MailDeltaRefreshSummary>(result);
    CHECK_FALSE(summary.emailNeedsFullRefresh);
    CHECK(summary.emailChanged);
    CHECK(summary.queryAffectedMailboxIds == std::vector<std::string>{"archive"});

    const auto cachedResult = emails.find("account-1", "email-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(cachedResult));
    CHECK_FALSE(std::get<std::optional<javelin::jmap::domain::Email>>(cachedResult).has_value());

    javelin::jmap::cache::SyncStateRepository states{database.connection};
    const auto stateResult =
        states.find({.accountId = "account-1", .objectType = "Email", .queryKey = {}});
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::cache::SyncStateRecord>>(stateResult));
    REQUIRE(
        std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(stateResult).has_value());
    CHECK(std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(stateResult)->stateToken ==
          "email-state-2");
}

TEST_CASE("account Email rebaseline reapplies active optimistic mutations",
          "[jmap][sync][mail-delta][rebaseline][mutation]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeDatabaseContext();
    seedAccount(database.connection);
    seedEmailState(database.connection);

    javelin::jmap::cache::EmailRepository emails{database.connection};
    REQUIRE_FALSE(emails.upsertMany("account-1", {email({"archive"}, {})}).has_value());
    javelin::jmap::sync::EmailMutationJournal mutations{database.connection};
    REQUIRE_FALSE(mutations
                      .put({
                          .mutationId = "pending-unread",
                          .operationGroupId = std::nullopt,
                          .accountId = "account-1",
                          .status = javelin::jmap::sync::MutationStatus::Pending,
                          .patch =
                              {
                                  .emailId = "email-1",
                                  .addMailboxIds = {},
                                  .removeMailboxIds = {},
                                  .addKeywords = {},
                                  .removeKeywords = {"$seen"},
                                  .destroy = false,
                              },
                          .baseMailboxIds = std::vector<std::string>{"archive"},
                          .baseKeywords = std::vector<std::string>{"$seen"},
                          .baseState = "email-state-1",
                          .acceptedState = std::nullopt,
                          .errorJson = std::nullopt,
                      })
                      .has_value());

    FakeTransport transport;
    transport.queuedResults.push_back(recoverableEmailDeltaResponse("cannotCalculateChanges"));
    transport.queuedResults.push_back(
        rebaselineEmailResponse("email-state-2", emailJson("archive", true)));
    javelin::jmap::api::MethodCaller caller{transport};
    javelin::jmap::sync::MailDeltaRefreshExecutor executor{database.connection, caller,
                                                           requestContext()};
    const auto result = QCoro::waitFor(executor.refresh("account-1", {.email = true}));

    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailDeltaRefreshSummary>(result));
    const auto cachedResult = emails.find("account-1", "email-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(cachedResult));
    const auto& cached = std::get<std::optional<javelin::jmap::domain::Email>>(cachedResult);
    REQUIRE(cached.has_value());
    CHECK(cached->keywords.empty());

    const auto activeResult = mutations.listActive("account-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::EmailMutationRecord>>(
        activeResult));
    REQUIRE(std::get<std::vector<javelin::jmap::sync::EmailMutationRecord>>(activeResult).size() ==
            1);
}

TEST_CASE("account Email rebaseline is superseded by a concurrent Email mutation",
          "[jmap][sync][mail-delta][rebaseline][race]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeDatabaseContext();
    seedAccount(database.connection);
    seedEmailState(database.connection);

    javelin::jmap::cache::EmailRepository emails{database.connection};
    REQUIRE_FALSE(emails.upsertMany("account-1", {email({"archive"}, {})}).has_value());

    FakeTransport transport;
    transport.queuedResults.push_back(recoverableEmailDeltaResponse("cannotCalculateChanges"));
    transport.queuedResults.push_back(
        rebaselineEmailResponse("email-state-2", emailJson("inbox", true)));
    transport.onSend = [&database](const std::size_t requestNumber)
    {
        if (requestNumber != 2)
            return;
        javelin::jmap::sync::ConsistencyDomainRepository consistency{database.connection};
        const auto advanced = consistency.advanceMutation({
            .accountId = "account-1",
            .dataType = "Email",
        });
        REQUIRE(std::holds_alternative<std::uint64_t>(advanced));
    };

    javelin::jmap::api::MethodCaller caller{transport};
    javelin::jmap::sync::MailDeltaRefreshExecutor executor{database.connection, caller,
                                                           requestContext()};
    const auto result = QCoro::waitFor(executor.refresh("account-1", {.email = true}));

    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailDeltaRefreshSummary>(result));
    const auto& summary = std::get<javelin::jmap::sync::MailDeltaRefreshSummary>(result);
    CHECK(summary.superseded);

    const auto cachedResult = emails.find("account-1", "email-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(cachedResult));
    const auto& cached = std::get<std::optional<javelin::jmap::domain::Email>>(cachedResult);
    REQUIRE(cached.has_value());
    CHECK(cached->mailboxIds == std::vector<std::string>{"archive"});
    CHECK(cached->keywords.empty());

    javelin::jmap::cache::SyncStateRepository states{database.connection};
    const auto stateResult =
        states.find({.accountId = "account-1", .objectType = "Email", .queryKey = {}});
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::cache::SyncStateRecord>>(stateResult));
    REQUIRE(
        std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(stateResult).has_value());
    CHECK(std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(stateResult)->stateToken ==
          "email-state-1");
}

TEST_CASE("account Email rebaseline retries a bounded pass when Email state advances",
          "[jmap][sync][mail-delta][rebaseline][state-race]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeDatabaseContext();
    seedAccount(database.connection);
    seedEmailState(database.connection);

    auto first = email({"archive"}, {});
    auto second = email({"archive"}, {});
    second.id = "email-2";
    second.blobId = "blob-2";
    second.threadId = "thread-2";
    javelin::jmap::cache::EmailRepository emails{database.connection};
    REQUIRE_FALSE(emails.upsertMany("account-1", {first, second}).has_value());

    FakeTransport transport;
    transport.queuedResults.push_back(recoverableEmailDeltaResponse("cannotCalculateChanges"));
    transport.queuedResults.push_back(twoRebaselineEmailResponse(
        "email-state-2", emailJsonWithIdentity("email-1", "thread-1", "inbox", true),
        "email-state-3", emailJsonWithIdentity("email-2", "thread-2", "inbox", true)));
    transport.queuedResults.push_back(twoRebaselineEmailResponse(
        "email-state-3", emailJsonWithIdentity("email-1", "thread-1", "archive", false),
        "email-state-3", emailJsonWithIdentity("email-2", "thread-2", "archive", false)));

    auto context = requestContext();
    context.requestLimits = javelin::jmap::api::CoreRequestLimits{
        .maxSizeRequest = 1024 * 1024,
        .maxConcurrentRequests = 1,
        .maxCallsInRequest = 2,
        .maxObjectsInGet = 1,
        .maxObjectsInSet = 1,
    };
    javelin::jmap::api::MethodCaller caller{transport};
    javelin::jmap::sync::MailDeltaRefreshExecutor executor{database.connection, caller, context};
    const auto result = QCoro::waitFor(executor.refresh("account-1", {.email = true}));

    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailDeltaRefreshSummary>(result));
    REQUIRE(transport.requests.size() == 3);
    const auto firstCachedResult = emails.find("account-1", "email-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(firstCachedResult));
    const auto& firstCached =
        std::get<std::optional<javelin::jmap::domain::Email>>(firstCachedResult);
    REQUIRE(firstCached.has_value());
    CHECK(firstCached->mailboxIds == std::vector<std::string>{"archive"});
    CHECK(firstCached->keywords.empty());

    javelin::jmap::cache::SyncStateRepository states{database.connection};
    const auto stateResult =
        states.find({.accountId = "account-1", .objectType = "Email", .queryKey = {}});
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::cache::SyncStateRecord>>(stateResult));
    REQUIRE(
        std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(stateResult).has_value());
    CHECK(std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(stateResult)->stateToken ==
          "email-state-3");
}

TEST_CASE("notification enablement supersedes an in-flight Email delta and baselines atomically",
          "[jmap][sync][mail-delta][notification][baseline][race]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeDatabaseContext();
    seedAccount(database.connection);
    seedEmailState(database.connection);
    javelin::jmap::cache::MailboxRepository mailboxes{database.connection};
    REQUIRE_FALSE(mailboxes.upsertMany("account-1", {mailbox("inbox", 1, "inbox")}).has_value());

    FakeTransport transport;
    javelin::jmap::api::MethodCaller caller{transport};
    javelin::jmap::sync::MailDeltaRefreshExecutor executor{database.connection, caller,
                                                           requestContext()};

    transport.queuedResults.push_back(
        emailDeltaResponseAtStates("email-state-1", "email-state-2", R"("email-1")", {}, {},
                                   emailJsonWithIdentity("email-1", "thread-1", "inbox", false)));
    transport.onSend = [&database](const std::size_t requestNumber)
    {
        if (requestNumber != 1)
            return;
        javelin::jmap::sync::ConsistencyDomainRepository consistency{database.connection};
        const auto advanced =
            consistency.advanceMutation({.accountId = "account-1", .dataType = "Email"});
        REQUIRE(std::holds_alternative<std::uint64_t>(advanced));
    };
    const auto stale = QCoro::waitFor(executor.refresh("account-1", {.email = true}));
    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailDeltaRefreshSummary>(stale));
    CHECK(std::get<javelin::jmap::sync::MailDeltaRefreshSummary>(stale).superseded);

    javelin::jmap::cache::SyncStateRepository states{database.connection};
    const auto staleState =
        states.find({.accountId = "account-1", .objectType = "Email", .queryKey = {}});
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::cache::SyncStateRecord>>(staleState));
    REQUIRE(std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(staleState).has_value());
    CHECK(std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(staleState)->stateToken ==
          "email-state-1");

    transport.onSend = {};
    transport.queuedResults.push_back(
        emailDeltaResponseAtStates("email-state-1", "email-state-2", R"("email-1")", {}, {},
                                   emailJsonWithIdentity("email-1", "thread-1", "inbox", false)));
    const auto baseline = QCoro::waitFor(
        executor.refresh("account-1", {.email = true}, {}, std::vector<std::string>{"inbox"}));
    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailDeltaRefreshSummary>(baseline));
    const auto& baselineSummary = std::get<javelin::jmap::sync::MailDeltaRefreshSummary>(baseline);
    CHECK(baselineSummary.notificationBaselineEstablished);
    CHECK_FALSE(baselineSummary.notificationEventsCreated);

    javelin::jmap::cache::NotificationRepository notifications{database.connection};
    const auto activeMailboxes = notifications.activeMailboxIds("account-1");
    REQUIRE(std::holds_alternative<std::vector<std::string>>(activeMailboxes));
    CHECK(std::get<std::vector<std::string>>(activeMailboxes) == std::vector<std::string>{"inbox"});
    const auto historicalPending = notifications.listPendingEvents("account-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(
        historicalPending));
    CHECK(
        std::get<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(historicalPending)
            .empty());

    transport.queuedResults.push_back(
        emailDeltaResponseAtStates("email-state-2", "email-state-3", R"("email-2")", {}, {},
                                   emailJsonWithIdentity("email-2", "thread-2", "inbox", false)));
    const auto subsequent = QCoro::waitFor(executor.refresh("account-1", {.email = true}));
    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailDeltaRefreshSummary>(subsequent));
    CHECK(std::get<javelin::jmap::sync::MailDeltaRefreshSummary>(subsequent)
              .notificationEventsCreated);
    const auto pending = notifications.listPendingEvents("account-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(
        pending));
    const auto& events =
        std::get<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(pending);
    REQUIRE(events.size() == 1);
    CHECK(events.front().emailId == "email-2");
}

TEST_CASE("notification baseline completion survives Email changes fallback",
          "[jmap][sync][mail-delta][notification][baseline][fallback]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeDatabaseContext();
    seedAccount(database.connection);
    seedEmailState(database.connection);
    javelin::jmap::cache::MailboxRepository mailboxes{database.connection};
    REQUIRE_FALSE(mailboxes.upsertMany("account-1", {mailbox("inbox", 1, "inbox")}).has_value());

    FakeTransport transport;
    transport.queuedResults.push_back(recoverableEmailDeltaResponse("cannotCalculateChanges"));
    transport.queuedResults.push_back(rebaselineEmailResponse("email-state-2", {}));
    javelin::jmap::api::MethodCaller caller{transport};
    javelin::jmap::sync::MailDeltaRefreshExecutor executor{database.connection, caller,
                                                           requestContext()};

    const auto result = QCoro::waitFor(
        executor.refresh("account-1", {.email = true}, {}, std::vector<std::string>{"inbox"}));
    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailDeltaRefreshSummary>(result));
    const auto& summary = std::get<javelin::jmap::sync::MailDeltaRefreshSummary>(result);
    CHECK(summary.notificationBaselineEstablished);
    CHECK_FALSE(summary.notificationEventsCreated);

    javelin::jmap::cache::NotificationRepository notifications{database.connection};
    const auto activeMailboxes = notifications.activeMailboxIds("account-1");
    REQUIRE(std::holds_alternative<std::vector<std::string>>(activeMailboxes));
    CHECK(std::get<std::vector<std::string>>(activeMailboxes) == std::vector<std::string>{"inbox"});
    const auto pending = notifications.listPendingEvents("account-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(
        pending));
    CHECK(
        std::get<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(pending).empty());
}

TEST_CASE("notification mailbox activation failure rolls back the Email baseline",
          "[jmap][sync][mail-delta][notification][mailboxes][atomicity]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeDatabaseContext();
    seedAccount(database.connection);
    seedEmailState(database.connection);
    javelin::jmap::cache::MailboxRepository mailboxes{database.connection};
    REQUIRE_FALSE(mailboxes.upsertMany("account-1", {mailbox("inbox", 1, "inbox")}).has_value());

    QSqlQuery failActivation{database.connection.database()};
    REQUIRE(failActivation.exec(QStringLiteral(
        "CREATE TRIGGER fail_notification_mailbox_insert BEFORE INSERT ON "
        "mail_notification_mailboxes BEGIN SELECT RAISE(FAIL,'forced activation failure'); END")));

    FakeTransport transport;
    transport.queuedResults.push_back(
        emailDeltaResponseAtStates("email-state-1", "email-state-2", R"("email-1")", {}, {},
                                   emailJsonWithIdentity("email-1", "thread-1", "inbox", false)));
    javelin::jmap::api::MethodCaller caller{transport};
    javelin::jmap::sync::MailDeltaRefreshExecutor executor{database.connection, caller,
                                                           requestContext()};
    const auto failed = QCoro::waitFor(
        executor.refresh("account-1", {.email = true}, {}, std::vector<std::string>{"inbox"}));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(failed));

    javelin::jmap::cache::SyncStateRepository states{database.connection};
    const auto stateResult =
        states.find({.accountId = "account-1", .objectType = "Email", .queryKey = {}});
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::cache::SyncStateRecord>>(stateResult));
    REQUIRE(
        std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(stateResult).has_value());
    CHECK(std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(stateResult)->stateToken ==
          "email-state-1");

    javelin::jmap::cache::EmailRepository emails{database.connection};
    const auto cached = emails.find("account-1", "email-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(cached));
    CHECK_FALSE(std::get<std::optional<javelin::jmap::domain::Email>>(cached).has_value());

    REQUIRE(failActivation.exec(QStringLiteral("DROP TRIGGER fail_notification_mailbox_insert")));
    transport.queuedResults.push_back(
        emailDeltaResponseAtStates("email-state-1", "email-state-2", R"("email-1")", {}, {},
                                   emailJsonWithIdentity("email-1", "thread-1", "inbox", false)));
    const auto retried = QCoro::waitFor(
        executor.refresh("account-1", {.email = true}, {}, std::vector<std::string>{"inbox"}));
    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailDeltaRefreshSummary>(retried));
    CHECK(std::get<javelin::jmap::sync::MailDeltaRefreshSummary>(retried)
              .notificationBaselineEstablished);
    javelin::jmap::cache::NotificationRepository notifications{database.connection};
    const auto activeMailboxes = notifications.activeMailboxIds("account-1");
    REQUIRE(std::holds_alternative<std::vector<std::string>>(activeMailboxes));
    CHECK(std::get<std::vector<std::string>>(activeMailboxes) == std::vector<std::string>{"inbox"});
}

TEST_CASE("account Email delta creates one event for a multi-mailbox unread arrival",
          "[jmap][sync][mail-delta][notification]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeDatabaseContext();
    seedAccount(database.connection);
    seedEmailState(database.connection);
    javelin::jmap::cache::MailboxRepository mailboxes{database.connection};
    REQUIRE_FALSE(
        mailboxes.upsertMany("account-1", {mailbox("inbox", 1, "inbox"), mailbox("archive", 1)})
            .has_value());
    seedNotificationMailboxes(database.connection, {"archive", "inbox"});

    FakeTransport transport;
    transport.queuedResults.push_back(emailDeltaResponse(
        R"("email-1")", {}, {},
        emailJsonWithIdentityAndMailboxes("email-1", "thread-1", {"archive", "inbox"}, false)));
    javelin::jmap::api::MethodCaller caller{transport};
    javelin::jmap::sync::MailDeltaRefreshExecutor executor{database.connection, caller,
                                                           requestContext()};
    const auto result = QCoro::waitFor(executor.refresh("account-1", {.email = true}));

    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailDeltaRefreshSummary>(result));
    const auto& summary = std::get<javelin::jmap::sync::MailDeltaRefreshSummary>(result);
    CHECK(summary.notificationEventsCreated);
    javelin::jmap::cache::NotificationRepository notifications{database.connection};
    const auto pendingResult = notifications.listPendingEvents("account-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(
        pendingResult));
    const auto& pending =
        std::get<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(pendingResult);
    REQUIRE(pending.size() == 1);
    CHECK(pending.front().emailId == "email-1");
    CHECK(pending.front().mailboxId == "inbox");
}

TEST_CASE("duplicate state-change catch-up after notification commit does not duplicate the event",
          "[jmap][sync][mail-delta][notification][replay]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeDatabaseContext();
    seedAccount(database.connection);
    seedEmailState(database.connection);
    javelin::jmap::cache::MailboxRepository mailboxes{database.connection};
    REQUIRE_FALSE(mailboxes.upsertMany("account-1", {mailbox("inbox", 1, "inbox")}).has_value());
    seedNotificationMailboxes(database.connection, {"inbox"});

    FakeTransport firstTransport;
    firstTransport.queuedResults.push_back(
        emailDeltaResponseAtStates("email-state-1", "email-state-2", R"("email-1")", {}, {},
                                   emailJsonWithIdentity("email-1", "thread-1", "inbox", false)));
    javelin::jmap::api::MethodCaller firstCaller{firstTransport};
    javelin::jmap::sync::MailDeltaRefreshExecutor firstExecutor{database.connection, firstCaller,
                                                                requestContext()};
    const auto first = QCoro::waitFor(firstExecutor.refresh("account-1", {.email = true}));
    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailDeltaRefreshSummary>(first));
    CHECK(std::get<javelin::jmap::sync::MailDeltaRefreshSummary>(first).notificationEventsCreated);

    javelin::jmap::cache::NotificationRepository notifications{database.connection};
    const auto firstPending = notifications.listPendingEvents("account-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(
        firstPending));
    REQUIRE(std::get<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(firstPending)
                .size() == 1);

    FakeTransport replayTransport;
    replayTransport.queuedResults.push_back(
        emailDeltaResponseAtStates("email-state-2", "email-state-2", {}, {}, {}));
    javelin::jmap::api::MethodCaller replayCaller{replayTransport};
    javelin::jmap::sync::MailDeltaRefreshExecutor replayExecutor{database.connection, replayCaller,
                                                                 requestContext()};
    const auto replay = QCoro::waitFor(replayExecutor.refresh("account-1", {.email = true}));
    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailDeltaRefreshSummary>(replay));
    CHECK_FALSE(
        std::get<javelin::jmap::sync::MailDeltaRefreshSummary>(replay).notificationEventsCreated);
    REQUIRE(replayTransport.requests.size() == 1);
    CHECK(replayTransport.requests.front().body.contains("email-state-2"));

    const auto replayPending = notifications.listPendingEvents("account-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(
        replayPending));
    const auto& pending =
        std::get<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(replayPending);
    REQUIRE(pending.size() == 1);
    CHECK(pending.front().emailId == "email-1");
}

TEST_CASE("account Email delta creates a first event for genuine server mailbox entry",
          "[jmap][sync][mail-delta][notification]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeDatabaseContext();
    seedAccount(database.connection);
    seedMail(database.connection);
    seedNotificationMailboxes(database.connection, {"inbox"});

    FakeTransport transport;
    transport.queuedResults.push_back(emailDeltaResponse({}, R"("email-1")", {}));
    transport.queuedResults.push_back(updatedEmailResponse("inbox", false));
    javelin::jmap::api::MethodCaller caller{transport};
    javelin::jmap::sync::MailDeltaRefreshExecutor executor{database.connection, caller,
                                                           requestContext()};
    const auto result = QCoro::waitFor(executor.refresh("account-1", {.email = true}));

    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailDeltaRefreshSummary>(result));
    CHECK(std::get<javelin::jmap::sync::MailDeltaRefreshSummary>(result).notificationEventsCreated);
    javelin::jmap::cache::NotificationRepository notifications{database.connection};
    const auto pendingResult = notifications.listPendingEvents("account-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(
        pendingResult));
    CHECK(std::get<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(pendingResult)
              .size() == 1);
}

TEST_CASE("account Email delta does not turn seen churn into new mail",
          "[jmap][sync][mail-delta][notification]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeDatabaseContext();
    seedAccount(database.connection);
    seedMail(database.connection);
    javelin::jmap::cache::EmailRepository emails{database.connection};
    REQUIRE_FALSE(emails.upsertMany("account-1", {email({"inbox"}, {"$seen"})}).has_value());
    seedNotificationMailboxes(database.connection, {"inbox"});

    FakeTransport transport;
    transport.queuedResults.push_back(emailDeltaResponse({}, R"("email-1")", {}));
    transport.queuedResults.push_back(updatedEmailResponse("inbox", false));
    javelin::jmap::api::MethodCaller caller{transport};
    javelin::jmap::sync::MailDeltaRefreshExecutor executor{database.connection, caller,
                                                           requestContext()};
    const auto result = QCoro::waitFor(executor.refresh("account-1", {.email = true}));

    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailDeltaRefreshSummary>(result));
    CHECK_FALSE(
        std::get<javelin::jmap::sync::MailDeltaRefreshSummary>(result).notificationEventsCreated);
    javelin::jmap::cache::NotificationRepository notifications{database.connection};
    const auto pendingResult = notifications.listPendingEvents("account-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(
        pendingResult));
    CHECK(std::get<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(pendingResult)
              .empty());
}

TEST_CASE("account Email delta suppresses server confirmation of local mailbox operations",
          "[jmap][sync][mail-delta][notification][mutation]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    struct OperationCase
    {
        std::string name;
        std::vector<std::string> baseMailboxIds{};
        std::vector<std::string> currentMailboxIds{};
        std::vector<std::string> baseKeywords{};
        std::vector<std::string> currentKeywords{};
        std::vector<std::string> addMailboxIds{};
        std::vector<std::string> removeMailboxIds{};
        std::vector<std::string> addKeywords{};
        std::vector<std::string> removeKeywords{};
        std::vector<std::string> notificationMailboxIds{};
    };

    const std::vector<OperationCase> cases{
        {.name = "Move",
         .baseMailboxIds = {"archive"},
         .currentMailboxIds = {"inbox"},
         .addMailboxIds = {"inbox"},
         .removeMailboxIds = {"archive"},
         .notificationMailboxIds = {"inbox"}},
        {.name = "Archive",
         .baseMailboxIds = {"inbox"},
         .currentMailboxIds = {"archive"},
         .addMailboxIds = {"archive"},
         .removeMailboxIds = {"inbox"},
         .notificationMailboxIds = {"archive"}},
        {.name = "Restore",
         .baseMailboxIds = {"archive"},
         .currentMailboxIds = {"inbox"},
         .addMailboxIds = {"inbox"},
         .removeMailboxIds = {"archive"},
         .notificationMailboxIds = {"inbox"}},
        {.name = "Junk",
         .baseMailboxIds = {"inbox"},
         .currentMailboxIds = {"junk"},
         .baseKeywords = {"$notjunk"},
         .currentKeywords = {"$junk"},
         .addMailboxIds = {"junk"},
         .removeMailboxIds = {"inbox"},
         .addKeywords = {"$junk"},
         .removeKeywords = {"$notjunk"},
         .notificationMailboxIds = {"junk"}},
        {.name = "Not Junk",
         .baseMailboxIds = {"junk"},
         .currentMailboxIds = {"inbox"},
         .baseKeywords = {"$junk"},
         .currentKeywords = {"$notjunk"},
         .addMailboxIds = {"inbox"},
         .removeMailboxIds = {"junk"},
         .addKeywords = {"$notjunk"},
         .removeKeywords = {"$junk"},
         .notificationMailboxIds = {"inbox"}},
        {.name = "Mailbox add",
         .baseMailboxIds = {"archive"},
         .currentMailboxIds = {"archive", "inbox"},
         .addMailboxIds = {"inbox"},
         .notificationMailboxIds = {"inbox"}},
        {.name = "Mailbox remove",
         .baseMailboxIds = {"archive", "inbox"},
         .currentMailboxIds = {"archive"},
         .removeMailboxIds = {"inbox"},
         .notificationMailboxIds = {"archive"}},
    };

    for (const auto& operation : cases)
    {
        CAPTURE(operation.name);
        auto database = makeDatabaseContext();
        seedAccount(database.connection);
        seedEmailState(database.connection);
        javelin::jmap::cache::MailboxRepository mailboxes{database.connection};
        REQUIRE_FALSE(mailboxes
                          .upsertMany("account-1", {mailbox("inbox", 1, "inbox"),
                                                    mailbox("archive", 1, "archive"),
                                                    mailbox("junk", 1, "junk")})
                          .has_value());
        seedNotificationMailboxes(database.connection, operation.notificationMailboxIds);
        javelin::jmap::cache::EmailRepository emails{database.connection};
        REQUIRE_FALSE(emails
                          .upsertMany("account-1", {email(operation.currentMailboxIds,
                                                          operation.currentKeywords)})
                          .has_value());
        javelin::jmap::sync::EmailMutationJournal journal{database.connection};
        REQUIRE_FALSE(journal
                          .put({
                              .mutationId = "local-operation",
                              .operationGroupId = std::nullopt,
                              .accountId = "account-1",
                              .status = javelin::jmap::sync::MutationStatus::Pending,
                              .patch =
                                  {
                                      .emailId = "email-1",
                                      .addMailboxIds = operation.addMailboxIds,
                                      .removeMailboxIds = operation.removeMailboxIds,
                                      .addKeywords = operation.addKeywords,
                                      .removeKeywords = operation.removeKeywords,
                                      .destroy = false,
                                  },
                              .baseMailboxIds = operation.baseMailboxIds,
                              .baseKeywords = operation.baseKeywords,
                              .baseState = "email-state-1",
                              .acceptedState = std::nullopt,
                              .errorJson = std::nullopt,
                          })
                          .has_value());

        FakeTransport transport;
        transport.queuedResults.push_back(emailDeltaResponse({}, R"("email-1")", {}));
        transport.queuedResults.push_back(
            updatedEmailResponseAtState("email-state-2", operation.currentMailboxIds, false));
        javelin::jmap::api::MethodCaller caller{transport};
        javelin::jmap::sync::MailDeltaRefreshExecutor executor{database.connection, caller,
                                                               requestContext()};
        const auto result = QCoro::waitFor(executor.refresh("account-1", {.email = true}));

        REQUIRE(std::holds_alternative<javelin::jmap::sync::MailDeltaRefreshSummary>(result));
        CHECK_FALSE(std::get<javelin::jmap::sync::MailDeltaRefreshSummary>(result)
                        .notificationEventsCreated);
        javelin::jmap::cache::NotificationRepository notifications{database.connection};
        const auto pendingResult = notifications.listPendingEvents("account-1");
        REQUIRE(
            std::holds_alternative<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(
                pendingResult));
        CHECK(
            std::get<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(pendingResult)
                .empty());
    }
}

TEST_CASE("account Email delta suppresses Javelin-imported created mail",
          "[jmap][sync][mail-delta][notification][import]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeDatabaseContext();
    seedAccount(database.connection);
    seedEmailState(database.connection);
    javelin::jmap::cache::MailboxRepository mailboxes{database.connection};
    REQUIRE_FALSE(mailboxes.upsertMany("account-1", {mailbox("inbox", 1, "inbox")}).has_value());
    seedNotificationMailboxes(database.connection, {"inbox"});
    seedImportProvenance(database.connection, "email-1");

    FakeTransport transport;
    transport.queuedResults.push_back(
        emailDeltaResponse(R"("email-1")", {}, {}, emailJson("inbox", false)));
    javelin::jmap::api::MethodCaller caller{transport};
    javelin::jmap::sync::MailDeltaRefreshExecutor executor{database.connection, caller,
                                                           requestContext()};
    const auto result = QCoro::waitFor(executor.refresh("account-1", {.email = true}));

    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailDeltaRefreshSummary>(result));
    CHECK_FALSE(
        std::get<javelin::jmap::sync::MailDeltaRefreshSummary>(result).notificationEventsCreated);
    javelin::jmap::cache::NotificationRepository notifications{database.connection};
    const auto pendingResult = notifications.listPendingEvents("account-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(
        pendingResult));
    CHECK(std::get<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(pendingResult)
              .empty());
}

TEST_CASE("account Email delta suppresses a batch of Javelin-imported unread mail",
          "[jmap][sync][mail-delta][notification][import][batch]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeDatabaseContext();
    seedAccount(database.connection);
    seedEmailState(database.connection);
    javelin::jmap::cache::MailboxRepository mailboxes{database.connection};
    REQUIRE_FALSE(mailboxes.upsertMany("account-1", {mailbox("inbox", 64, "inbox")}).has_value());
    seedNotificationMailboxes(database.connection, {"inbox"});

    constexpr std::size_t importedCount = 64;
    std::vector<std::string> importedIds;
    importedIds.reserve(importedCount);
    std::string createdIds;
    std::string createdObjects;
    for (std::size_t index = 0; index < importedCount; ++index)
    {
        const auto id = "imported-" + std::to_string(index);
        importedIds.push_back(id);
        if (index != 0)
        {
            createdIds += ',';
            createdObjects += ',';
        }
        createdIds += '"' + id + '"';
        createdObjects += emailJsonWithIdentity(id, "thread-" + id, "inbox", false);
    }
    seedImportProvenanceBatch(database.connection, importedIds);

    FakeTransport transport;
    transport.queuedResults.push_back(emailDeltaResponse(createdIds, {}, {}, createdObjects));
    javelin::jmap::api::MethodCaller caller{transport};
    javelin::jmap::sync::MailDeltaRefreshExecutor executor{database.connection, caller,
                                                           requestContext()};
    const auto result = QCoro::waitFor(executor.refresh("account-1", {.email = true}));

    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailDeltaRefreshSummary>(result));
    CHECK_FALSE(
        std::get<javelin::jmap::sync::MailDeltaRefreshSummary>(result).notificationEventsCreated);
    javelin::jmap::cache::NotificationRepository notifications{database.connection};
    const auto pendingResult = notifications.listPendingEvents("account-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(
        pendingResult));
    CHECK(std::get<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(pendingResult)
              .empty());

    QSqlQuery counts{database.connection.database()};
    REQUIRE(counts.exec(QStringLiteral(
        "SELECT (SELECT COUNT(*) FROM emails WHERE account_id='account-1'),"
        "(SELECT COUNT(*) FROM mail_notification_state WHERE account_id='account-1')")));
    REQUIRE(counts.next());
    CHECK(counts.value(0).toULongLong() == importedCount);
    CHECK(counts.value(1).toULongLong() == 0);
}

TEST_CASE("notification event failure rolls back Email state and consumption",
          "[jmap][sync][mail-delta][notification][atomicity]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeDatabaseContext();
    seedAccount(database.connection);
    seedEmailState(database.connection);
    javelin::jmap::cache::MailboxRepository mailboxes{database.connection};
    REQUIRE_FALSE(mailboxes.upsertMany("account-1", {mailbox("inbox", 1, "inbox")}).has_value());
    seedNotificationMailboxes(database.connection, {"inbox"});

    QSqlQuery failOutbox{database.connection.database()};
    REQUIRE(failOutbox.exec(QStringLiteral(
        "CREATE TRIGGER fail_notification_outbox BEFORE INSERT ON mail_notification_event_outbox "
        "BEGIN SELECT RAISE(FAIL,'forced notification outbox failure'); END")));

    FakeTransport transport;
    transport.queuedResults.push_back(
        emailDeltaResponse(R"("email-1")", {}, {}, emailJson("inbox", false)));
    javelin::jmap::api::MethodCaller caller{transport};
    javelin::jmap::sync::MailDeltaRefreshExecutor executor{database.connection, caller,
                                                           requestContext()};
    const auto result = QCoro::waitFor(executor.refresh("account-1", {.email = true}));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(result));

    javelin::jmap::cache::SyncStateRepository states{database.connection};
    const auto stateResult =
        states.find({.accountId = "account-1", .objectType = "Email", .queryKey = {}});
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::cache::SyncStateRecord>>(stateResult));
    const auto& state = std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(stateResult);
    REQUIRE(state.has_value());
    CHECK(state->stateToken == "email-state-1");

    javelin::jmap::cache::EmailRepository emails{database.connection};
    const auto cached = emails.find("account-1", "email-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(cached));
    CHECK_FALSE(std::get<std::optional<javelin::jmap::domain::Email>>(cached).has_value());

    QSqlQuery notificationRows{database.connection.database()};
    REQUIRE(notificationRows.exec(QStringLiteral(
        "SELECT (SELECT COUNT(*) FROM mail_notification_state WHERE account_id='account-1'),"
        "(SELECT COUNT(*) FROM mail_notification_event_outbox WHERE account_id='account-1')")));
    REQUIRE(notificationRows.next());
    CHECK(notificationRows.value(0).toInt() == 0);
    CHECK(notificationRows.value(1).toInt() == 0);
}

TEST_CASE("notification consumption prevents a second event after later re-entry",
          "[jmap][sync][mail-delta][notification][dedup]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeDatabaseContext();
    seedAccount(database.connection);
    seedMail(database.connection);
    javelin::jmap::cache::MailboxRepository mailboxes{database.connection};
    REQUIRE_FALSE(mailboxes.upsertMany("account-1", {mailbox("other", 1)}).has_value());
    seedNotificationMailboxes(database.connection, {"inbox"});

    FakeTransport transport;
    javelin::jmap::api::MethodCaller caller{transport};
    javelin::jmap::sync::MailDeltaRefreshExecutor executor{database.connection, caller,
                                                           requestContext()};

    transport.queuedResults.push_back(
        emailDeltaResponseAtStates("email-state-1", "email-state-2", {}, R"("email-1")", {}));
    transport.queuedResults.push_back(
        updatedEmailResponseAtState("email-state-2", {"inbox"}, false));
    const auto first = QCoro::waitFor(executor.refresh("account-1", {.email = true}));
    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailDeltaRefreshSummary>(first));
    CHECK(std::get<javelin::jmap::sync::MailDeltaRefreshSummary>(first).notificationEventsCreated);

    QSqlQuery delivered{database.connection.database()};
    REQUIRE(delivered.exec(QStringLiteral(
        "DELETE FROM mail_notification_event_outbox WHERE account_id='account-1' AND "
        "email_id='email-1'")));

    transport.queuedResults.push_back(
        emailDeltaResponseAtStates("email-state-2", "email-state-3", {}, R"("email-1")", {}));
    transport.queuedResults.push_back(
        updatedEmailResponseAtState("email-state-3", {"other"}, false));
    const auto left = QCoro::waitFor(executor.refresh("account-1", {.email = true}));
    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailDeltaRefreshSummary>(left));
    CHECK_FALSE(
        std::get<javelin::jmap::sync::MailDeltaRefreshSummary>(left).notificationEventsCreated);

    transport.queuedResults.push_back(
        emailDeltaResponseAtStates("email-state-3", "email-state-4", {}, R"("email-1")", {}));
    transport.queuedResults.push_back(
        updatedEmailResponseAtState("email-state-4", {"inbox"}, false));
    const auto restored = QCoro::waitFor(executor.refresh("account-1", {.email = true}));
    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailDeltaRefreshSummary>(restored));
    CHECK_FALSE(
        std::get<javelin::jmap::sync::MailDeltaRefreshSummary>(restored).notificationEventsCreated);

    javelin::jmap::cache::NotificationRepository notifications{database.connection};
    const auto pendingResult = notifications.listPendingEvents("account-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(
        pendingResult));
    CHECK(std::get<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(pendingResult)
              .empty());
}

TEST_CASE("account mail delta applies an external seen change without invalidating mailbox queries",
          "[jmap][sync][mail-delta]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeDatabaseContext();
    seedAccount(database.connection);
    seedMail(database.connection);
    const auto archiveQueryKey = seedMailboxWindow(database.connection, "archive");
    javelin::jmap::cache::MailboxWindowRepository windows{database.connection};
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
    transport.queuedResults.push_back(deltaResponse(true));
    transport.queuedResults.push_back(updatedEmailResponse("archive", true));
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
    REQUIRE(transport.requests.size() == 2);
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
    CHECK(window->materialization == javelin::jmap::cache::QueryWindowMaterialization::Complete);
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
    const auto inboxQueryKey = seedMailboxWindow(database.connection, "inbox");
    const auto archiveQueryKey = seedMailboxWindow(database.connection, "archive");

    FakeTransport transport;
    transport.queuedResults.push_back(deltaResponse(false));
    transport.queuedResults.push_back(updatedEmailResponse("inbox", false));
    javelin::jmap::api::MethodCaller caller{transport};
    javelin::jmap::sync::MailDeltaRefreshExecutor executor{database.connection, caller,
                                                           requestContext()};
    const auto result =
        QCoro::waitFor(executor.refresh("account-1", {.mailbox = true, .email = true}));

    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailDeltaRefreshSummary>(result));
    const auto& summary = std::get<javelin::jmap::sync::MailDeltaRefreshSummary>(result);
    CHECK(summary.queryAffectedMailboxIds == std::vector<std::string>{"archive", "inbox"});
    REQUIRE(transport.requests.size() == 2);

    javelin::jmap::cache::MailboxWindowRepository windows{database.connection};
    for (const auto& queryKey : {archiveQueryKey, inboxQueryKey})
    {
        const auto windowResult = windows.find("account-1", queryKey, 0, 100);
        REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::MailboxWindowRecord>>(
            windowResult));
        const auto& window =
            std::get<std::optional<javelin::jmap::cache::MailboxWindowRecord>>(windowResult);
        REQUIRE(window.has_value());
        CHECK(window->coverage == javelin::jmap::cache::QueryWindowCoverage::Stale);
        CHECK(window->materialization == javelin::jmap::cache::QueryWindowMaterialization::Partial);
    }

    javelin::jmap::cache::MailboxMessageReadRepository mailboxMessages{database.connection};
    javelin::jmap::cache::QueryWindowReadRepository queryWindows{database.connection,
                                                                 mailboxMessages};
    const auto inboxPageResult =
        queryWindows.loadMailboxWindow("account-1", inboxQueryKey, 0, 100, {});
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::MailboxWindowPage>>(
        inboxPageResult));
    const auto& inboxPage =
        std::get<std::optional<javelin::jmap::cache::MailboxWindowPage>>(inboxPageResult);
    REQUIRE(inboxPage.has_value());
    REQUIRE(inboxPage->items.size() == 1);
    CHECK(inboxPage->items.front().emailId == "email-1");

    const auto archivePageResult =
        queryWindows.loadMailboxWindow("account-1", archiveQueryKey, 0, 100, {});
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::MailboxWindowPage>>(
        archivePageResult));
    const auto& archivePage =
        std::get<std::optional<javelin::jmap::cache::MailboxWindowPage>>(archivePageResult);
    REQUIRE(archivePage.has_value());
    CHECK(archivePage->items.empty());
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
    transport.queuedResults.push_back(deltaResponse(false));
    transport.queuedResults.push_back(updatedEmailResponse("archive", false));
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
    REQUIRE(transport.requests.size() == 2);

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

TEST_CASE("account mail delta skips an updated unmaterialized Thread child",
          "[jmap][sync][mail-delta][thread-materialization]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeDatabaseContext();
    seedAccount(database.connection);
    seedEmailState(database.connection);
    javelin::jmap::cache::ThreadRepository threads{database.connection};
    REQUIRE_FALSE(
        threads.upsertMany("account-1", {{.id = "thread-1", .emailIds = {"email-1"}}}).has_value());

    FakeTransport transport;
    transport.queuedResults.push_back(emailDeltaResponse({}, R"("email-1")", {}));
    javelin::jmap::api::MethodCaller caller{transport};
    javelin::jmap::sync::MailDeltaRefreshExecutor executor{database.connection, caller,
                                                           requestContext()};
    const auto result = QCoro::waitFor(executor.refresh("account-1", {.email = true}));

    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailDeltaRefreshSummary>(result));
    const auto& summary = std::get<javelin::jmap::sync::MailDeltaRefreshSummary>(result);
    CHECK(summary.emailChanged);
    CHECK_FALSE(summary.emailNeedsFullRefresh);
    REQUIRE(transport.requests.size() == 1);
    CHECK_FALSE(transport.requests.front().body.contains("/updated"));
    CHECK_FALSE(transport.requests.front().body.contains("relevant-updated-emails"));

    javelin::jmap::cache::EmailRepository emails{database.connection};
    const auto cached = emails.find("account-1", "email-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(cached));
    CHECK_FALSE(std::get<std::optional<javelin::jmap::domain::Email>>(cached).has_value());
    const auto coverage = threads.coverage("account-1", "thread-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::ThreadCoverage>>(coverage));
    REQUIRE(std::get<std::optional<javelin::jmap::cache::ThreadCoverage>>(coverage).has_value());
    CHECK_FALSE(std::get<std::optional<javelin::jmap::cache::ThreadCoverage>>(coverage)
                    ->childEmailsComplete);
    javelin::jmap::cache::SyncStateRepository states{database.connection};
    const auto stateResult =
        states.find({.accountId = "account-1", .objectType = "Email", .queryKey = {}});
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::cache::SyncStateRecord>>(stateResult));
    const auto& state = std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(stateResult);
    REQUIRE(state.has_value());
    CHECK(state->stateToken == "email-state-2");
}

TEST_CASE("account mail delta fetches an uncached updated Email tracked by a mailbox window",
          "[jmap][sync][mail-delta][query-window]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeDatabaseContext();
    seedAccount(database.connection);
    seedEmailState(database.connection);
    const auto archiveQueryKey = seedMailboxWindow(database.connection, "archive");

    FakeTransport transport;
    transport.queuedResults.push_back(emailDeltaResponse({}, R"("email-1")", {}));
    transport.queuedResults.push_back(updatedEmailResponse("inbox", false));
    javelin::jmap::api::MethodCaller caller{transport};
    javelin::jmap::sync::MailDeltaRefreshExecutor executor{database.connection, caller,
                                                           requestContext()};
    const auto result = QCoro::waitFor(executor.refresh("account-1", {.email = true}));

    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailDeltaRefreshSummary>(result));
    const auto& summary = std::get<javelin::jmap::sync::MailDeltaRefreshSummary>(result);
    CHECK_FALSE(summary.emailNeedsFullRefresh);
    CHECK(summary.queryAffectedMailboxIds == std::vector<std::string>{"archive", "inbox"});
    REQUIRE(transport.requests.size() == 2);
    CHECK(transport.requests.back().body.contains("relevant-updated-emails"));
    CHECK(transport.requests.back().body.contains("email-1"));

    javelin::jmap::cache::EmailRepository emails{database.connection};
    const auto cached = emails.find("account-1", "email-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(cached));
    REQUIRE(std::get<std::optional<javelin::jmap::domain::Email>>(cached).has_value());
    CHECK(std::get<std::optional<javelin::jmap::domain::Email>>(cached)->mailboxIds ==
          std::vector<std::string>{"inbox"});
    javelin::jmap::cache::MailboxWindowRepository windows{database.connection};
    const auto window = windows.find("account-1", archiveQueryKey, 0, 100);
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::cache::MailboxWindowRecord>>(window));
    REQUIRE(std::get<std::optional<javelin::jmap::cache::MailboxWindowRecord>>(window).has_value());
    CHECK(std::get<std::optional<javelin::jmap::cache::MailboxWindowRecord>>(window)->coverage ==
          javelin::jmap::cache::QueryWindowCoverage::Stale);
}

TEST_CASE("account mail delta makes an uncached tracked destruction sparse-cache safe",
          "[jmap][sync][mail-delta][thread-materialization]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeDatabaseContext();
    seedAccount(database.connection);
    seedEmailState(database.connection);
    const auto archiveQueryKey = seedMailboxWindow(database.connection, "archive");
    javelin::jmap::cache::ThreadRepository threads{database.connection};
    REQUIRE_FALSE(
        threads.upsertMany("account-1", {{.id = "thread-1", .emailIds = {"email-1"}}}).has_value());

    FakeTransport transport;
    transport.queuedResults.push_back(emailDeltaResponse({}, {}, R"("email-1")"));
    javelin::jmap::api::MethodCaller caller{transport};
    javelin::jmap::sync::MailDeltaRefreshExecutor executor{database.connection, caller,
                                                           requestContext()};
    const auto result = QCoro::waitFor(executor.refresh("account-1", {.email = true}));

    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailDeltaRefreshSummary>(result));
    const auto& summary = std::get<javelin::jmap::sync::MailDeltaRefreshSummary>(result);
    CHECK(summary.emailChanged);
    CHECK_FALSE(summary.emailNeedsFullRefresh);
    CHECK(summary.queryAffectedMailboxIds == std::vector<std::string>{"archive"});
    REQUIRE(transport.requests.size() == 1);

    const auto membership = threads.findMembership("account-1", "thread-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::ThreadMembershipRecord>>(
        membership));
    REQUIRE(std::get<std::optional<javelin::jmap::cache::ThreadMembershipRecord>>(membership)
                .has_value());
    CHECK(std::get<std::optional<javelin::jmap::cache::ThreadMembershipRecord>>(membership)
              ->freshness == javelin::jmap::cache::ThreadMembershipFreshness::Stale);
    javelin::jmap::cache::MailboxWindowRepository windows{database.connection};
    const auto window = windows.find("account-1", archiveQueryKey, 0, 100);
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::cache::MailboxWindowRecord>>(window));
    REQUIRE(std::get<std::optional<javelin::jmap::cache::MailboxWindowRecord>>(window).has_value());
    CHECK(std::get<std::optional<javelin::jmap::cache::MailboxWindowRecord>>(window)->coverage ==
          javelin::jmap::cache::QueryWindowCoverage::Stale);
}

TEST_CASE("account mail delta marks cached Thread membership stale for a created Email",
          "[jmap][sync][mail-delta][thread-materialization]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeDatabaseContext();
    seedAccount(database.connection);
    seedEmailState(database.connection);
    javelin::jmap::cache::ThreadRepository threads{database.connection};
    REQUIRE_FALSE(threads.upsertMany("account-1", {{.id = "thread-1", .emailIds = {"older-email"}}})
                      .has_value());

    FakeTransport transport;
    transport.queuedResults.push_back(
        emailDeltaResponse(R"("email-1")", {}, {}, emailJson("archive", false)));
    javelin::jmap::api::MethodCaller caller{transport};
    javelin::jmap::sync::MailDeltaRefreshExecutor executor{database.connection, caller,
                                                           requestContext()};
    const auto result = QCoro::waitFor(executor.refresh("account-1", {.email = true}));

    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailDeltaRefreshSummary>(result));
    CHECK_FALSE(
        std::get<javelin::jmap::sync::MailDeltaRefreshSummary>(result).emailNeedsFullRefresh);
    const auto membership = threads.findMembership("account-1", "thread-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::ThreadMembershipRecord>>(
        membership));
    REQUIRE(std::get<std::optional<javelin::jmap::cache::ThreadMembershipRecord>>(membership)
                .has_value());
    CHECK(std::get<std::optional<javelin::jmap::cache::ThreadMembershipRecord>>(membership)
              ->freshness == javelin::jmap::cache::ThreadMembershipFreshness::Stale);
}
