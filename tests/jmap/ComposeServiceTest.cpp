#include "jmap/submission/ComposeService.h"

#include "jmap/JmapCore.h"
#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/Session.h"
#include "jmap/api/Transport.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/IdentityRepository.h"
#include "jmap/cache/MailboxRepository.h"
#include "jmap/cache/MailboxWindowRepository.h"
#include "jmap/cache/QueryService.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/sync/MutationJournal.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
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
            if (request.dispatched)
                request.dispatched();
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

    [[nodiscard]] javelin::jmap::api::Session
    secondarySessionFor(const std::string& ownerAccountId, const std::string& targetAccountId,
                        const std::string& apiUrl)
    {
        auto session = sessionFor(ownerAccountId, apiUrl);
        session.accounts.at(ownerAccountId).accountCapabilities.submission = false;
        session.accounts.emplace(targetAccountId, javelin::jmap::api::Account{
                                                      .id = targetAccountId,
                                                      .name = targetAccountId,
                                                      .isPersonal = false,
                                                      .isReadOnly = false,
                                                      .accountCapabilities =
                                                          {
                                                              .mail = true,
                                                              .submission = true,
                                                              .contacts = std::nullopt,
                                                              .calendars = std::nullopt,
                                                          },
                                                  });
        session.primaryAccounts.submissionAccountId = targetAccountId;
        return session;
    }

    void seedAccount(javelin::jmap::cache::DatabaseConnection& connection,
                     const std::string& accountId, const std::string& apiUrl,
                     const std::string& identityId, const std::string& fromAddress,
                     std::optional<std::string> textSignature = std::nullopt,
                     std::optional<std::string> htmlSignature = std::nullopt)
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
                                              .textSignature = std::move(textSignature),
                                              .htmlSignature = std::move(htmlSignature),
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

TEST_CASE("compose uses owner credentials for a secondary submission account",
          "[jmap][submission][compose][multi-account]")
{
    ensureApplication();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("compose-secondary-account-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    REQUIRE_FALSE(
        sessions
            .replace("owner-account", secondarySessionFor("owner-account", "sending-account",
                                                          "https://example.test/jmap"))
            .has_value());

    FakeTransport transport;
    transport.results.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArrayLiteral(
            R"({"methodResponses":[["Identity/get",{"accountId":"sending-account","state":"i1","list":[{"id":"identity-1","name":"Alice","email":"alice@example.test","replyTo":null,"bcc":null,"textSignature":"","htmlSignature":"","mayDelete":true}],"notFound":[]},"identities"]],"sessionState":"s2"})"),
    });
    javelin::jmap::api::HttpJmapMethodTransport methodTransport{transport};
    javelin::jmap::JmapCore core{connection, transport, methodTransport};
    javelin::jmap::submission::ComposeService service{connection, transport, methodTransport, core};

    const auto result = QCoro::waitFor(service.loadSenderIdentities(
        {
            .sessionUrl = "https://example.test/session",
            .loginEmail = "shared-login@example.test",
            .apiKey = "owner-secret",
        },
        "sending-account"));

    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::domain::Identity>>(result));
    REQUIRE(transport.requests.size() == 1);
    QByteArray authorization;
    for (const auto& header : transport.requests.front().headers)
    {
        if (header.name.compare("Authorization", Qt::CaseInsensitive) == 0)
            authorization = header.value;
    }
    CHECK(authorization == QByteArrayLiteral("Bearer owner-secret"));
    CHECK(transport.requests.front().body.contains("\"accountId\":\"sending-account\""));
}

TEST_CASE("new compose sessions use the requested editor mode", "[jmap][submission][compose]")
{
    ensureApplication();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("compose-editor-mode-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    seedAccount(connection, "account-2", "https://account-2.example.test/jmap", "identity-2",
                "sender@example.test");

    FakeTransport transport;
    javelin::jmap::api::HttpJmapMethodTransport methodTransport{transport};
    javelin::jmap::JmapCore core{connection, transport, methodTransport};
    javelin::jmap::submission::ComposeService service{connection, transport, methodTransport, core};

    const auto result = QCoro::waitFor(service.open(
        {
            .sessionUrl = "https://account-2.example.test/.well-known/jmap",
            .loginEmail = "shared-login@example.test",
            .apiKey = "account-2-secret",
        },
        {
            .accountId = "account-2",
            .mode = javelin::jmap::submission::ComposeMode::NewMessage,
            .initialEditorMode = javelin::jmap::submission::BodyEditorMode::PlainText,
            .referenceEmailId = std::nullopt,
            .draftEmailId = std::nullopt,
            .initialTo = {{.name = std::nullopt, .email = "alice@example.test"}},
            .initialCc = {{.name = std::nullopt, .email = "bob@example.test"}},
            .initialBcc = {{.name = std::nullopt, .email = "carol@example.test"}},
            .initialSubject = "Mail link subject",
            .initialBody = "Mail link body",
            .useExistingWorkingCopy = true,
            .composeSessionId = std::nullopt,
        }));

    REQUIRE(std::holds_alternative<javelin::jmap::submission::DraftSnapshot>(result));
    const auto& snapshot = std::get<javelin::jmap::submission::DraftSnapshot>(result);
    CHECK(snapshot.editorMode == javelin::jmap::submission::BodyEditorMode::PlainText);
    REQUIRE(snapshot.to.size() == 1);
    CHECK(snapshot.to.front().email == "alice@example.test");
    REQUIRE(snapshot.cc.size() == 1);
    CHECK(snapshot.cc.front().email == "bob@example.test");
    REQUIRE(snapshot.bcc.size() == 1);
    CHECK(snapshot.bcc.front().email == "carol@example.test");
    CHECK(snapshot.subject == std::optional<std::string>{"Mail link subject"});
    CHECK(snapshot.plainTextBody == "Mail link body");
    CHECK(snapshot.htmlBody == "<p>Mail link body</p>");
    CHECK(transport.requests.empty());
}

TEST_CASE("new compose sessions place the selected Identity signature after an empty authored area",
          "[jmap][submission][compose][signature]")
{
    ensureApplication();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("compose-identity-signature-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    seedAccount(connection, "account-2", "https://account-2.example.test/jmap", "identity-2",
                "sender@example.test", "Regards,\nAlice", "<p>Regards,<br>Alice</p>");

    FakeTransport transport;
    javelin::jmap::api::HttpJmapMethodTransport methodTransport{transport};
    javelin::jmap::JmapCore core{connection, transport, methodTransport};
    javelin::jmap::submission::ComposeService service{connection, transport, methodTransport, core};

    const auto result = QCoro::waitFor(service.open(
        {
            .sessionUrl = "https://account-2.example.test/.well-known/jmap",
            .loginEmail = "shared-login@example.test",
            .apiKey = "account-2-secret",
        },
        {
            .accountId = "account-2",
            .mode = javelin::jmap::submission::ComposeMode::NewMessage,
            .initialEditorMode = javelin::jmap::submission::BodyEditorMode::RichText,
            .referenceEmailId = std::nullopt,
            .draftEmailId = std::nullopt,
            .initialTo = {},
            .useExistingWorkingCopy = false,
            .composeSessionId = "signature-compose",
        }));

    REQUIRE(std::holds_alternative<javelin::jmap::submission::DraftSnapshot>(result));
    const auto& snapshot = std::get<javelin::jmap::submission::DraftSnapshot>(result);
    CHECK(snapshot.identityId == "identity-2");
    CHECK(snapshot.plainTextBody == "\nRegards,\nAlice");
    CHECK(snapshot.htmlBody == "<p><br/></p><p>Regards,<br>Alice</p>");
    CHECK(transport.requests.empty());
}

TEST_CASE("plain text compose creates no HTML body alternative", "[jmap][submission][plain-text]")
{
    ensureApplication();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("compose-plain-text-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    seedAccount(connection, "account-2", "https://account-2.example.test/jmap", "identity-2",
                "sender@example.test");
    const std::string draftsQueryKey =
        "mailbox:account-2-drafts|sort:receivedAt:desc|collapseThreads:true";
    javelin::jmap::cache::MailboxWindowRepository windows{connection};
    REQUIRE_FALSE(windows
                      .replace({
                          .accountId = "account-2",
                          .mailboxId = "account-2-drafts",
                          .queryKey = draftsQueryKey,
                          .requestedOffset = 0,
                          .requestedLimit = 100,
                          .position = 0,
                          .returnedLimit = 0,
                          .total = 0,
                          .queryState = "draft-query-1",
                          .emailIds = {},
                      })
                      .has_value());

    FakeTransport transport;
    transport.results = {draftCreatedResponse()};
    javelin::jmap::api::HttpJmapMethodTransport methodTransport{transport};
    javelin::jmap::JmapCore core{connection, transport, methodTransport};
    javelin::jmap::submission::ComposeService service{connection, transport, methodTransport, core};
    const auto result = QCoro::waitFor(service.saveDraft(
        {
            .sessionUrl = "https://account-2.example.test/.well-known/jmap",
            .loginEmail = "shared-login@example.test",
            .apiKey = "account-2-secret",
        },
        {
            .composeSessionId = "compose-plain",
            .accountId = "account-2",
            .draftEmailId = std::nullopt,
            .mode = javelin::jmap::submission::ComposeMode::NewMessage,
            .editorMode = javelin::jmap::submission::BodyEditorMode::PlainText,
            .identityId = "identity-2",
            .to = {{.name = std::nullopt, .email = "recipient@example.test"}},
            .cc = {},
            .bcc = {},
            .subject = "Plain text",
            .plainTextBody = "First line\nSecond line",
            .htmlBody = "<p>This stale HTML must not be sent.</p>",
            .threading = {},
            .attachments = {},
        }));

    REQUIRE(std::holds_alternative<javelin::jmap::submission::DraftSaveSummary>(result));
    const auto& summary = std::get<javelin::jmap::submission::DraftSaveSummary>(result);
    CHECK(summary.affectedMailboxIds == std::vector<std::string>{"account-2-drafts"});
    javelin::jmap::cache::QueryService queries{connection};
    const auto projectedResult = queries.loadMailboxWindow("account-2", draftsQueryKey, 0, 100, {});
    const auto* projected =
        std::get_if<std::optional<javelin::jmap::cache::MailboxWindowPage>>(&projectedResult);
    REQUIRE(projected != nullptr);
    REQUIRE(projected->has_value());
    CHECK((*projected)->coverage == javelin::jmap::cache::QueryWindowCoverage::LocallyProjected);
    REQUIRE((*projected)->items.size() == 1);
    CHECK((*projected)->items.front().emailId == "draft-2");
    REQUIRE(transport.requests.size() == 1);
    CHECK(transport.requests.front().body.contains("\"type\":\"text/plain\""));
    CHECK_FALSE(transport.requests.front().body.contains("\"type\":\"text/html\""));
    CHECK_FALSE(transport.requests.front().body.contains("html-body"));
    CHECK_FALSE(transport.requests.front().body.contains("stale HTML"));
}

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
        javelin::jmap::cache::EmailRepository emails{connection};
        if (transport.requests.size() == 1)
        {
            const auto draftIds = emails.listMailboxEmailIds("account-2", "account-2-drafts");
            REQUIRE(std::holds_alternative<std::vector<std::string>>(draftIds));
            const auto& values = std::get<std::vector<std::string>>(draftIds);
            REQUIRE(values.size() == 1);
            CHECK(values.front().starts_with("local-"));
            return;
        }
        if (transport.requests.size() != 2)
            return;
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

    const javelin::jmap::LiveConnectionSettings liveSettings{
        .sessionUrl = "https://account-2.example.test/.well-known/jmap",
        .loginEmail = "shared-login@example.test",
        .apiKey = "account-2-secret",
    };
    auto preparedResult = QCoro::waitFor(service.prepareSend(
        liveSettings, javelin::jmap::submission::DraftSnapshot{
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
    REQUIRE(std::holds_alternative<javelin::jmap::submission::PreparedSend>(preparedResult));
    REQUIRE(transport.requests.size() == 1);
    const auto plainPartPosition =
        transport.requests.front().body.indexOf(QByteArrayLiteral("\"partId\":\"text-body\""));
    const auto htmlPartPosition =
        transport.requests.front().body.indexOf(QByteArrayLiteral("\"partId\":\"html-body\""));
    REQUIRE(plainPartPosition >= 0);
    REQUIRE(htmlPartPosition >= 0);
    CHECK(plainPartPosition < htmlPartPosition);

    bool submissionDispatched = false;
    const auto result = QCoro::waitFor(service.submitPreparedSend(
        liveSettings, std::get<javelin::jmap::submission::PreparedSend>(std::move(preparedResult)),
        [&submissionDispatched] { submissionDispatched = true; }));

    REQUIRE(std::holds_alternative<javelin::jmap::submission::SendSummary>(result));
    CHECK(submissionDispatched);
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

TEST_CASE("draft replacement creates the new draft before destroying the old one",
          "[jmap][submission][draft][consistency]")
{
    ensureApplication();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("compose-draft-replacement-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    seedAccount(connection, "account-2", "https://account-2.example.test/jmap", "identity-2",
                "sender@example.test");
    javelin::jmap::cache::EmailRepository emails{connection};
    REQUIRE_FALSE(emails
                      .upsertMany("account-2", {{
                                                   .id = "draft-old",
                                                   .blobId = "blob-old",
                                                   .threadId = "thread-old",
                                                   .mailboxIds = {"account-2-drafts"},
                                                   .keywords = {"$seen", "$draft"},
                                                   .size = 10,
                                                   .receivedAt = "2026-07-17T00:00:00Z",
                                                   .sentAt = std::nullopt,
                                                   .messageId = {},
                                                   .inReplyTo = {},
                                                   .references = {},
                                                   .hasAttachment = false,
                                                   .subject = "Old",
                                                   .from = {},
                                                   .to = {},
                                                   .cc = {},
                                                   .bcc = {},
                                                   .replyTo = {},
                                                   .preview = std::nullopt,
                                               }})
                      .has_value());

    FakeTransport transport;
    transport.results = {
        javelin::jmap::api::HttpResponse{
            .statusCode = 200,
            .body = QByteArrayLiteral(
                R"({"methodResponses":[["Email/set",{"accountId":"account-2","oldState":"email-2","newState":"email-3","created":{"draft":{"id":"draft-3","blobId":"blob-3","threadId":"thread-old","size":43}},"updated":{},"destroyed":[],"notCreated":{},"notUpdated":{},"notDestroyed":{}},"draft-save"]],"sessionState":"session-3"})"),
        },
        javelin::jmap::api::HttpResponse{
            .statusCode = 200,
            .body = QByteArrayLiteral(
                R"({"methodResponses":[["Email/set",{"accountId":"account-2","oldState":"email-3","newState":"email-4","created":{},"updated":{},"destroyed":["draft-old"],"notCreated":{},"notUpdated":{},"notDestroyed":{}},"draft-replace-destroy"]],"sessionState":"session-4"})"),
        },
    };
    transport.beforeReturn = [&transport, &emails]
    {
        const auto oldDraft = emails.find("account-2", "draft-old");
        REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(oldDraft));
        CHECK_FALSE(std::get<std::optional<javelin::jmap::domain::Email>>(oldDraft).has_value());
        if (transport.requests.size() == 1)
        {
            const auto ids = emails.listMailboxEmailIds("account-2", "account-2-drafts");
            REQUIRE(std::holds_alternative<std::vector<std::string>>(ids));
            const auto& values = std::get<std::vector<std::string>>(ids);
            REQUIRE(values.size() == 1);
            CHECK(values.front().starts_with("local-"));
        }
        else
        {
            const auto accepted = emails.find("account-2", "draft-3");
            REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(accepted));
            REQUIRE(std::get<std::optional<javelin::jmap::domain::Email>>(accepted).has_value());
        }
    };
    javelin::jmap::api::HttpJmapMethodTransport methodTransport{transport};
    javelin::jmap::JmapCore core{connection, transport, methodTransport};
    javelin::jmap::submission::ComposeService service{connection, transport, methodTransport, core};
    const auto result = QCoro::waitFor(service.saveDraft(
        {
            .sessionUrl = "https://account-2.example.test/.well-known/jmap",
            .loginEmail = "shared-login@example.test",
            .apiKey = "account-2-secret",
        },
        {
            .composeSessionId = "compose-2",
            .accountId = "account-2",
            .draftEmailId = "draft-old",
            .mode = javelin::jmap::submission::ComposeMode::NewMessage,
            .editorMode = javelin::jmap::submission::BodyEditorMode::RichText,
            .identityId = "identity-2",
            .to = {{.name = std::nullopt, .email = "recipient@example.test"}},
            .cc = {},
            .bcc = {},
            .subject = "Replacement",
            .plainTextBody = "Body",
            .htmlBody = "<p>Body</p>",
            .threading = {},
            .attachments = {},
        }));
    REQUIRE(std::holds_alternative<javelin::jmap::submission::DraftSaveSummary>(result));
    CHECK(std::get<javelin::jmap::submission::DraftSaveSummary>(result).draftEmailId == "draft-3");
    REQUIRE(transport.requests.size() == 2);
    CHECK(transport.requests.front().body.contains("\"create\""));
    CHECK_FALSE(transport.requests.front().body.contains("\"destroy\":[\"draft-old\"]"));
    CHECK(transport.requests.back().body.contains("\"destroy\":[\"draft-old\"]"));
}

TEST_CASE("an ambiguous draft creation keeps the local draft projection",
          "[jmap][submission][draft][consistency]")
{
    ensureApplication();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("compose-draft-unknown-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    seedAccount(connection, "account-2", "https://account-2.example.test/jmap", "identity-2",
                "sender@example.test");
    FakeTransport transport;
    transport.results = {
        javelin::jmap::api::TransportError{
            .code = javelin::jmap::api::TransportErrorCode::NetworkFailure,
            .message = "Connection closed after dispatch",
        },
    };
    javelin::jmap::api::HttpJmapMethodTransport methodTransport{transport};
    javelin::jmap::JmapCore core{connection, transport, methodTransport};
    javelin::jmap::submission::ComposeService service{connection, transport, methodTransport, core};
    const auto result = QCoro::waitFor(service.saveDraft(
        {
            .sessionUrl = "https://account-2.example.test/.well-known/jmap",
            .loginEmail = "shared-login@example.test",
            .apiKey = "account-2-secret",
        },
        {
            .composeSessionId = "compose-2",
            .accountId = "account-2",
            .draftEmailId = std::nullopt,
            .mode = javelin::jmap::submission::ComposeMode::NewMessage,
            .editorMode = javelin::jmap::submission::BodyEditorMode::RichText,
            .identityId = "identity-2",
            .to = {{.name = std::nullopt, .email = "recipient@example.test"}},
            .cc = {},
            .bcc = {},
            .subject = "Uncertain draft",
            .plainTextBody = "Body",
            .htmlBody = "<p>Body</p>",
            .threading = {},
            .attachments = {},
        }));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(result));
    javelin::jmap::cache::EmailRepository emails{connection};
    const auto ids = emails.listMailboxEmailIds("account-2", "account-2-drafts");
    REQUIRE(std::holds_alternative<std::vector<std::string>>(ids));
    const auto& values = std::get<std::vector<std::string>>(ids);
    REQUIRE(values.size() == 1);
    CHECK(values.front().starts_with("local-"));
    javelin::jmap::sync::MutationJournalRepository journal{connection};
    const auto unknown = journal.listByStatus({.accountId = "account-2", .dataType = "Email"},
                                              javelin::jmap::sync::MutationStatus::Unknown, 10);
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::MutationRecord>>(unknown));
    CHECK(std::get<std::vector<javelin::jmap::sync::MutationRecord>>(unknown).size() == 1);

    const auto retry = QCoro::waitFor(service.saveDraft(
        {
            .sessionUrl = "https://account-2.example.test/.well-known/jmap",
            .loginEmail = "shared-login@example.test",
            .apiKey = "account-2-secret",
        },
        {
            .composeSessionId = "compose-2",
            .accountId = "account-2",
            .draftEmailId = values.front(),
            .mode = javelin::jmap::submission::ComposeMode::NewMessage,
            .editorMode = javelin::jmap::submission::BodyEditorMode::RichText,
            .identityId = "identity-2",
            .to = {{.name = std::nullopt, .email = "recipient@example.test"}},
            .cc = {},
            .bcc = {},
            .subject = "Uncertain draft",
            .plainTextBody = "Body",
            .htmlBody = "<p>Body</p>",
            .threading = {},
            .attachments = {},
        }));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(retry));
    CHECK(std::get<javelin::jmap::OperationError>(retry).code ==
          javelin::jmap::OperationErrorCode::Conflict);
    CHECK(transport.requests.size() == 1);
}

TEST_CASE("compose save rejects a replaced attachment source",
          "[jmap][submission][compose][attachment]")
{
    ensureApplication();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("compose-attachment-replacement-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));

    const auto sourcePath = directory.filePath(QStringLiteral("attachment.bin"));
    const QByteArray original = QByteArrayLiteral("old");
    QFile source{sourcePath};
    REQUIRE(source.open(QIODevice::WriteOnly));
    REQUIRE(source.write(original) == original.size());
    source.close();
    const auto originalHash =
        QString::fromLatin1(QCryptographicHash::hash(original, QCryptographicHash::Sha256).toHex())
            .toStdString();
    REQUIRE(source.open(QIODevice::WriteOnly | QIODevice::Truncate));
    REQUIRE(source.write(QByteArrayLiteral("new")) == original.size());
    source.close();

    FakeTransport transport;
    javelin::jmap::api::HttpJmapMethodTransport methodTransport{transport};
    javelin::jmap::JmapCore core{connection, transport, methodTransport};
    javelin::jmap::submission::ComposeService service{connection, transport, methodTransport, core};
    const auto result = QCoro::waitFor(service.saveDraft(
        {
            .sessionUrl = "https://account-1.example.test/.well-known/jmap",
            .loginEmail = "alice@example.test",
            .apiKey = "account-1-secret",
        },
        {
            .composeSessionId = "compose-attachment-replacement",
            .accountId = "account-1",
            .revision = 7,
            .draftEmailId = std::nullopt,
            .mode = javelin::jmap::submission::ComposeMode::NewMessage,
            .editorMode = javelin::jmap::submission::BodyEditorMode::RichText,
            .identityId = "identity-1",
            .to = {},
            .cc = {},
            .bcc = {},
            .subject = std::nullopt,
            .plainTextBody = "Body",
            .htmlBody = "<p>Body</p>",
            .threading = {},
            .attachments =
                {
                    {
                        .localFilePath = sourcePath.toStdString(),
                        .displayName = "attachment.bin",
                        .mediaType = "application/octet-stream",
                        .size = static_cast<std::uint64_t>(original.size()),
                        .blobId = std::nullopt,
                        .inlineDisposition = false,
                        .contentId = std::nullopt,
                        .contentHash = originalHash,
                    },
                },
        }));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(result));
    CHECK(std::get<javelin::jmap::OperationError>(result).code ==
          javelin::jmap::OperationErrorCode::Conflict);
    CHECK(transport.requests.empty());
}
