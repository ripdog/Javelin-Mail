#include "jmap/submission/ComposeService.h"

#include "jmap/JmapCore.h"
#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/Session.h"
#include "jmap/api/Transport.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/IdentityRepository.h"
#include "jmap/cache/MailboxRepository.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/sync/MutationJournal.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <utility>
#include <vector>

namespace
{
    class FakeTransport final : public javelin::jmap::api::AbstractTransport
    {
      public:
        std::vector<javelin::jmap::api::HttpRequest> requests;
        std::vector<javelin::jmap::api::TransportResult> results;
        std::function<void()> beforeReturn;

        QCoro::Task<javelin::jmap::api::TransportResult>
        send(javelin::jmap::api::HttpRequest request) override
        {
            requests.push_back(std::move(request));
            REQUIRE_FALSE(results.empty());
            auto result = std::move(results.front());
            results.erase(results.begin());
            if (beforeReturn)
                beforeReturn();
            co_return result;
        }
    };

    void ensureApplication()
    {
        if (QCoreApplication::instance() != nullptr)
        {
            return;
        }

        static int argc = 1;
        static char name[] = "javelin-tests";
        static char* argv[] = {name, nullptr};
        static QCoreApplication application(argc, argv);
        Q_UNUSED(application);
    }

    [[nodiscard]] javelin::jmap::api::Session sessionFor(const std::string& accountId,
                                                         const std::string& apiUrl)
    {
        javelin::jmap::api::Session session{
            .username = "shared-login@example.test",
            .apiUrl = apiUrl,
            .downloadUrl = apiUrl + "/download/{accountId}/{blobId}/{name}",
            .uploadUrl = apiUrl + "/upload/{accountId}",
            .eventSourceUrl = std::nullopt,
            .state = "session-state",
            .capabilities =
                {
                    .core = true,
                    .coreDetails = std::nullopt,
                    .mail = true,
                    .submission = true,
                    .contacts = false,
                    .calendars = false,
                    .websocket = std::nullopt,
                },
            .accounts = {},
            .primaryAccounts =
                {
                    .mailAccountId = accountId,
                    .submissionAccountId = accountId,
                    .contactsAccountId = std::nullopt,
                    .calendarsAccountId = std::nullopt,
                    .sieveAccountId = std::nullopt,
                },
        };
        session.accounts.emplace(accountId, javelin::jmap::api::Account{
                                                .id = accountId,
                                                .name = accountId,
                                                .isPersonal = true,
                                                .isReadOnly = false,
                                                .accountCapabilities =
                                                    {
                                                        .mail = true,
                                                        .submission = true,
                                                        .contacts = std::nullopt,
                                                        .calendars = std::nullopt,
                                                    },
                                            });
        return session;
    }

    void seedAccount(javelin::jmap::cache::DatabaseConnection& connection,
                     const std::string& accountId, const std::string& apiUrl,
                     const std::string& identityId, const std::string& fromAddress)
    {
        javelin::jmap::cache::SessionRepository sessions{connection};
        REQUIRE_FALSE(sessions.replace(accountId, sessionFor(accountId, apiUrl)).has_value());

        javelin::jmap::cache::MailboxRepository mailboxes{connection};
        REQUIRE_FALSE(mailboxes
                          .replaceAll(accountId,
                                      {
                                          javelin::jmap::domain::Mailbox{
                                              .id = accountId + "-drafts",
                                              .name = "Drafts",
                                              .parentId = std::nullopt,
                                              .role = "drafts",
                                              .sortOrder = 0,
                                              .totalEmails = 0,
                                              .unreadEmails = 0,
                                              .totalThreads = 0,
                                              .unreadThreads = 0,
                                              .isSubscribed = true,
                                              .myRights = {},
                                          },
                                          javelin::jmap::domain::Mailbox{
                                              .id = accountId + "-sent",
                                              .name = "Sent",
                                              .parentId = std::nullopt,
                                              .role = "sent",
                                              .sortOrder = 0,
                                              .totalEmails = 0,
                                              .unreadEmails = 0,
                                              .totalThreads = 0,
                                              .unreadThreads = 0,
                                              .isSubscribed = true,
                                              .myRights = {},
                                          },
                                      })
                          .has_value());

        javelin::jmap::cache::IdentityRepository identities{connection};
        REQUIRE_FALSE(identities
                          .replaceAll(accountId,
                                      {
                                          javelin::jmap::domain::Identity{
                                              .id = identityId,
                                              .name = accountId,
                                              .email = fromAddress,
                                              .replyTo = {},
                                              .bcc = {},
                                              .textSignature = std::nullopt,
                                              .htmlSignature = std::nullopt,
                                              .mayDelete = false,
                                          },
                                      })
                          .has_value());
    }

    [[nodiscard]] javelin::jmap::api::HttpResponse draftCreatedResponse()
    {
        return {
            .statusCode = 200,
            .body = QByteArrayLiteral(
                R"({"methodResponses":[["Email/set",{"accountId":"account-2","oldState":"email-1","newState":"email-2","created":{"draft":{"id":"draft-2","blobId":"blob-2","threadId":"thread-2","size":42}},"updated":{},"destroyed":[],"notCreated":{},"notUpdated":{},"notDestroyed":{}},"draft-save"]],"sessionState":"session-2"})"),
        };
    }

    [[nodiscard]] javelin::jmap::api::HttpResponse submittedResponse()
    {
        return {
            .statusCode = 200,
            .body = QByteArrayLiteral(
                R"({"methodResponses":[["EmailSubmission/set",{"accountId":"account-2","oldState":"submission-1","newState":"submission-2","created":{"send":{"id":"submission-2"}},"notCreated":{}},"send-message"],["Email/set",{"accountId":"account-2","oldState":"email-2","newState":"email-3","created":{},"updated":{"draft-2":null},"destroyed":[],"notCreated":{},"notUpdated":{},"notDestroyed":{}},"send-message"]],"sessionState":"session-3"})"),
        };
    }

} // namespace

TEST_CASE("compose sending uses the account selected with the From identity",
          "[jmap][submission][multiple-accounts]")
{
    ensureApplication();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());

    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("compose-multiple-accounts-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));

    seedAccount(connection, "account-1", "https://account-1.example.test/jmap", "identity-1",
                "sender@example.test");
    seedAccount(connection, "account-2", "https://account-2.example.test/jmap", "identity-2",
                "sender@example.test");

    FakeTransport transport;
    transport.results = {draftCreatedResponse(), submittedResponse()};
    transport.beforeReturn = [&transport, &connection]
    {
        if (transport.requests.size() != 2)
            return;
        javelin::jmap::cache::EmailRepository emails{connection};
        const auto found = emails.find("account-2", "draft-2");
        REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(found));
        const auto& email = std::get<std::optional<javelin::jmap::domain::Email>>(found);
        REQUIRE(email.has_value());
        CHECK(std::ranges::find(email->mailboxIds, "account-2-sent") != email->mailboxIds.end());
        CHECK(std::ranges::find(email->mailboxIds, "account-2-drafts") == email->mailboxIds.end());
        CHECK(std::ranges::find(email->keywords, "$draft") == email->keywords.end());
    };
    javelin::jmap::api::HttpJmapMethodTransport methodTransport{transport};
    javelin::jmap::JmapCore core{connection, transport, methodTransport};
    javelin::jmap::submission::ComposeService service{connection, transport, methodTransport, core};

    const auto result = QCoro::waitFor(service.send(
        {
            .sessionUrl = "https://account-2.example.test/.well-known/jmap",
            .loginEmail = "shared-login@example.test",
            .apiKey = "account-2-secret",
        },
        javelin::jmap::submission::DraftSnapshot{
            .composeSessionId = "compose-2",
            .accountId = "account-2",
            .draftEmailId = std::nullopt,
            .mode = javelin::jmap::submission::ComposeMode::NewMessage,
            .editorMode = javelin::jmap::submission::BodyEditorMode::RichText,
            .identityId = "identity-2",
            .to = {{.name = std::nullopt, .email = "recipient@example.test"}},
            .cc = {},
            .bcc = {},
            .subject = "Multiple account test",
            .plainTextBody = "Body",
            .htmlBody = "<p>Body</p>",
            .threading = {},
            .attachments = {},
        }));

    REQUIRE(std::holds_alternative<javelin::jmap::submission::SendSummary>(result));
    const auto& summary = std::get<javelin::jmap::submission::SendSummary>(result);
    CHECK(summary.accountId == "account-2");
    CHECK(summary.submissionId == std::optional<std::string>{"submission-2"});

    REQUIRE(transport.requests.size() == 2);
    for (const auto& request : transport.requests)
    {
        CHECK(request.url == QUrl{QStringLiteral("https://account-2.example.test/jmap")});
        CHECK(request.body.contains("\"accountId\":\"account-2\""));
        CHECK_FALSE(request.body.contains("\"accountId\":\"account-1\""));

        bool usedSelectedCredentials = false;
        for (const auto& header : request.headers)
        {
            usedSelectedCredentials =
                usedSelectedCredentials ||
                (header.name == QByteArrayLiteral("Authorization") &&
                 header.value == QByteArrayLiteral("Bearer account-2-secret"));
        }
        CHECK(usedSelectedCredentials);
    }
    CHECK(transport.requests.at(1).body.contains("\"identityId\":\"identity-2\""));
    CHECK_FALSE(transport.requests.at(1).body.contains("identity-1"));
}

TEST_CASE("an ambiguous submission keeps the optimistic Sent projection durable",
          "[jmap][submission][consistency]")
{
    ensureApplication();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("compose-ambiguous-submission-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    seedAccount(connection, "account-2", "https://account-2.example.test/jmap", "identity-2",
                "sender@example.test");

    FakeTransport transport;
    transport.results = {
        draftCreatedResponse(),
        javelin::jmap::api::TransportError{
            .code = javelin::jmap::api::TransportErrorCode::NetworkFailure,
            .message = "Connection closed after dispatch",
        },
    };
    javelin::jmap::api::HttpJmapMethodTransport methodTransport{transport};
    javelin::jmap::JmapCore core{connection, transport, methodTransport};
    javelin::jmap::submission::ComposeService service{connection, transport, methodTransport, core};
    const auto result = QCoro::waitFor(service.send(
        {
            .sessionUrl = "https://account-2.example.test/.well-known/jmap",
            .loginEmail = "shared-login@example.test",
            .apiKey = "account-2-secret",
        },
        javelin::jmap::submission::DraftSnapshot{
            .composeSessionId = "compose-2",
            .accountId = "account-2",
            .draftEmailId = std::nullopt,
            .mode = javelin::jmap::submission::ComposeMode::NewMessage,
            .editorMode = javelin::jmap::submission::BodyEditorMode::RichText,
            .identityId = "identity-2",
            .to = {{.name = std::nullopt, .email = "recipient@example.test"}},
            .cc = {},
            .bcc = {},
            .subject = "Ambiguous send",
            .plainTextBody = "Body",
            .htmlBody = "<p>Body</p>",
            .threading = {},
            .attachments = {},
        }));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(result));

    javelin::jmap::cache::EmailRepository emails{connection};
    const auto found = emails.find("account-2", "draft-2");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(found));
    const auto& email = std::get<std::optional<javelin::jmap::domain::Email>>(found);
    REQUIRE(email.has_value());
    CHECK(std::ranges::find(email->mailboxIds, "account-2-sent") != email->mailboxIds.end());
    CHECK(std::ranges::find(email->mailboxIds, "account-2-drafts") == email->mailboxIds.end());

    javelin::jmap::sync::MutationJournalRepository journal{connection};
    const auto emailMutations =
        journal.listByStatus({.accountId = "account-2", .dataType = "Email"},
                             javelin::jmap::sync::MutationStatus::Unknown, 10);
    const auto submissionMutations =
        journal.listByStatus({.accountId = "account-2", .dataType = "EmailSubmission"},
                             javelin::jmap::sync::MutationStatus::Unknown, 10);
    REQUIRE(
        std::holds_alternative<std::vector<javelin::jmap::sync::MutationRecord>>(emailMutations));
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::MutationRecord>>(
        submissionMutations));
    CHECK(std::get<std::vector<javelin::jmap::sync::MutationRecord>>(emailMutations).size() == 1);
    CHECK(std::get<std::vector<javelin::jmap::sync::MutationRecord>>(submissionMutations).size() ==
          1);
}
