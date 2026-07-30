#include "jmap/contacts/ContactService.h"

#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/PatchObject.h"
#include "jmap/api/Transport.h"
#include "jmap/cache/ContactRepository.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/cache/SyncStateRepository.h"
#include "jmap/contacts/AddressBookMutationJournal.h"
#include "jmap/contacts/ContactMutationJournal.h"
#include "jmap/sync/ConsistencyDomain.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <utility>

namespace
{
    class FakeTransport final : public javelin::jmap::api::AbstractTransport
    {
      public:
        javelin::jmap::api::HttpRequest lastRequest;
        std::vector<javelin::jmap::api::HttpRequest> requests;
        std::vector<javelin::jmap::api::TransportResult> results;
        std::function<void()> beforeReturn;

        QCoro::Task<javelin::jmap::api::TransportResult>
        send(javelin::jmap::api::HttpRequest request) override
        {
            requests.push_back(request);
            lastRequest = std::move(request);
            REQUIRE_FALSE(results.empty());
            auto result = std::move(results.front());
            results.erase(results.begin());
            if (auto callback = std::exchange(beforeReturn, {}))
                callback();
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

    [[nodiscard]] javelin::jmap::api::Session session()
    {
        javelin::jmap::api::Session value;
        value.username = "alice@example.test";
        value.apiUrl = "https://example.test/jmap";
        value.downloadUrl = "https://example.test/download/{accountId}/{blobId}/{name}";
        value.uploadUrl = "https://example.test/upload/{accountId}";
        value.state = "s1";
        value.capabilities.core = true;
        value.capabilities.coreDetails = javelin::jmap::api::CoreCapability{
            .maxSizeUpload = std::nullopt,
            .maxConcurrentUpload = std::nullopt,
            .maxSizeRequest = std::nullopt,
            .maxConcurrentRequests = std::nullopt,
            .maxCallsInRequest = std::nullopt,
            .maxObjectsInGet = 1,
            .maxObjectsInSet = std::nullopt,
            .collationAlgorithms = {},
        };
        value.capabilities.contacts = true;
        value.primaryAccounts.contactsAccountId = "a1";
        value.accounts.emplace(
            "a1", javelin::jmap::api::Account{
                      .id = "a1",
                      .name = "Personal",
                      .isPersonal = true,
                      .isReadOnly = false,
                      .accountCapabilities = {.mail = false,
                                              .submission = false,
                                              .contacts =
                                                  javelin::jmap::api::ContactsCapability{
                                                      .maxAddressBooksPerCard = std::nullopt,
                                                      .mayCreateAddressBook = true},
                                              .calendars = std::nullopt},
                  });
        return value;
    }

    [[nodiscard]] javelin::jmap::api::AddressBook addressBook(std::string name = "Personal")
    {
        return {
            .id = "book-1",
            .name = std::move(name),
            .description = std::nullopt,
            .sortOrder = 0,
            .isDefault = true,
            .isSubscribed = true,
            .shareWith = std::nullopt,
            .myRights = {.mayRead = true, .mayWrite = true, .mayShare = false, .mayDelete = true}};
    }

    [[nodiscard]] javelin::jmap::contacts::ContactSummary
    cachedContact(std::string id, std::string name, std::string email, std::string accountId = "a1")
    {
        return {.accountId = std::move(accountId),
                .id = id,
                .uid = "uid-" + id,
                .kind = "individual",
                .displayName = name,
                .organization = std::nullopt,
                .emails = {{.key = "email-1",
                            .address = email,
                            .label = std::nullopt,
                            .preference = std::nullopt,
                            .contexts = {}}},
                .addressBookIds = {"book-1"},
                .isImportant = false,
                .document =
                    R"({"id":")" + id + R"(","uid":"uid-)" + id +
                    R"(","kind":"individual","addressBookIds":{"book-1":true},"name":{"full":")" +
                    name + R"("},"emails":{"email-1":{"address":")" + email + R"("}}})"};
    }

    [[nodiscard]] javelin::jmap::contacts::ContactSummary
    cachedGroup(std::string id, std::string name, std::vector<std::string> members = {})
    {
        auto documentResult =
            javelin::jmap::contacts::createContactGroupDocument(name, "uid-" + id, "book-1");
        REQUIRE(std::holds_alternative<std::string>(documentResult));
        auto document = std::get<std::string>(std::move(documentResult));
        for (const auto& member : members)
        {
            const auto patchResult =
                javelin::jmap::contacts::contactGroupMembershipPatch(member, true);
            REQUIRE(std::holds_alternative<std::string>(patchResult));
            const auto patched =
                javelin::jmap::api::applyPatchObject(document, std::get<std::string>(patchResult));
            REQUIRE(std::holds_alternative<std::string>(patched));
            document = std::get<std::string>(patched);
        }
        const auto summary = javelin::jmap::contacts::summarizeContact(
            "a1", {.id = id, .uid = "uid-" + id, .kind = "group", .document = document});
        REQUIRE(summary.has_value());
        return *summary;
    }

    [[nodiscard]] bool groupContains(const javelin::jmap::contacts::ContactSummary& group,
                                     std::string_view memberUid)
    {
        const auto parsed = javelin::jmap::contacts::contactEditorData(group.document);
        const auto* data = std::get_if<javelin::jmap::contacts::ContactEditorData>(&parsed);
        return data != nullptr &&
               std::ranges::find(data->members, memberUid) != data->members.end();
    }

    [[nodiscard]] javelin::jmap::api::Session sessionWithContactDestination()
    {
        auto value = session();
        value.accounts.emplace(
            "a2", javelin::jmap::api::Account{
                      .id = "a2",
                      .name = "Shared",
                      .isPersonal = false,
                      .isReadOnly = false,
                      .accountCapabilities = {.mail = false,
                                              .submission = false,
                                              .contacts =
                                                  javelin::jmap::api::ContactsCapability{
                                                      .maxAddressBooksPerCard = std::nullopt,
                                                      .mayCreateAddressBook = true},
                                              .calendars = std::nullopt},
                  });
        return value;
    }
} // namespace

TEST_CASE("contact service greedily refreshes every contact into the cache",
          "[jmap][contacts][service]")
{
    ensureApplication();
    QTemporaryDir directory;
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("contact-service-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    REQUIRE_FALSE(sessions.replace("a1", session()).has_value());

    FakeTransport transport;
    transport.results.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body =
            QByteArray{
                R"({"methodResponses":[["AddressBook/get",{"accountId":"a1","state":"b1","list":[{"id":"book-1","name":"Personal","description":null,"sortOrder":0,"isDefault":true,"isSubscribed":true,"shareWith":null,"myRights":{"mayRead":true,"mayWrite":true,"mayShare":false,"mayDelete":true}}],"notFound":[]},"address-books"],["ContactCard/get",{"accountId":"a1","state":"c1","list":[{"id":"card-1","uid":"uid-1","kind":"individual","addressBookIds":{"book-1":true},"name":{"full":"Joe Bloggs"},"emails":{"e1":{"address":"joe@example.test"}}}],"notFound":[]},"contact-cards"]],"sessionState":"s2"})"},
    });
    javelin::jmap::api::HttpJmapMethodTransport methodTransport{transport};
    javelin::jmap::cache::ContactRepository contacts{connection};
    QString changedAccount;
    QObject::connect(&contacts, &javelin::jmap::cache::ContactRepository::contactsChanged,
                     [&changedAccount](const QString& accountId) { changedAccount = accountId; });
    javelin::jmap::contacts::ContactService service{connection, contacts, transport,
                                                    methodTransport};
    const auto result =
        QCoro::waitFor(service.refreshAll({.sessionUrl = "https://example.test/.well-known/jmap",
                                           .loginEmail = "alice@example.test",
                                           .apiKey = "secret"},
                                          "a1"));
    REQUIRE(std::holds_alternative<javelin::jmap::contacts::ContactRefreshSummary>(result));
    CHECK(std::get<javelin::jmap::contacts::ContactRefreshSummary>(result).contactCount == 1);
    CHECK(transport.lastRequest.body.contains("AddressBook/get"));
    CHECK(transport.lastRequest.body.contains("ContactCard/get"));
    CHECK(changedAccount == QStringLiteral("a1"));

    const auto found = contacts.findByEmail("joe@example.test");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::contacts::ContactSummary>>(found));
    REQUIRE(std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(found).has_value());
}

TEST_CASE("contact refresh discards a snapshot superseded by a ContactCard mutation",
          "[jmap][contacts][service][consistency]")
{
    ensureApplication();
    QTemporaryDir directory;
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("contact-service-consistency-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    REQUIRE_FALSE(sessions.replace("a1", session()).has_value());

    FakeTransport transport;
    transport.results.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body =
            QByteArray{
                R"({"methodResponses":[["AddressBook/get",{"accountId":"a1","state":"b1","list":[{"id":"book-1","name":"Stale","description":null,"sortOrder":0,"isDefault":true,"isSubscribed":true,"shareWith":null,"myRights":{"mayRead":true,"mayWrite":true,"mayShare":false,"mayDelete":true}}],"notFound":[]},"address-books"],["ContactCard/get",{"accountId":"a1","state":"c1","list":[{"id":"card-1","uid":"uid-1","kind":"individual","addressBookIds":{"book-1":true},"name":{"full":"Stale Contact"},"emails":{}}],"notFound":[]},"contact-cards"]],"sessionState":"s2"})"},
    });
    transport.beforeReturn = [&connection]
    {
        javelin::jmap::sync::ConsistencyDomainRepository consistency{connection};
        const auto generation =
            consistency.advanceMutation({.accountId = "a1", .dataType = "ContactCard"});
        REQUIRE(std::holds_alternative<std::uint64_t>(generation));
    };

    javelin::jmap::api::HttpJmapMethodTransport methodTransport{transport};
    javelin::jmap::cache::ContactRepository contacts{connection};
    javelin::jmap::contacts::ContactService service{connection, contacts, transport,
                                                    methodTransport};
    const auto result =
        QCoro::waitFor(service.refreshAll({.sessionUrl = "https://example.test/.well-known/jmap",
                                           .loginEmail = "alice@example.test",
                                           .apiKey = "secret"},
                                          "a1"));

    REQUIRE(std::holds_alternative<javelin::jmap::contacts::ContactRefreshSummary>(result));
    CHECK(std::get<javelin::jmap::contacts::ContactRefreshSummary>(result).contactCount == 0);
    const auto cached = contacts.listContacts("a1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::contacts::ContactSummary>>(cached));
    CHECK(std::get<std::vector<javelin::jmap::contacts::ContactSummary>>(cached).empty());
}

TEST_CASE("successful ContactCard sets advance only the ContactCard consistency domain",
          "[jmap][contacts][service][consistency]")
{
    ensureApplication();
    QTemporaryDir directory;
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("contact-service-mutation-consistency-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    REQUIRE_FALSE(sessions.replace("a1", session()).has_value());

    FakeTransport transport;
    transport.results.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body =
            QByteArray{
                R"({"methodResponses":[["ContactCard/set",{"accountId":"a1","oldState":"c1","newState":"c2","created":{},"updated":{"card-1":null},"destroyed":[],"notCreated":{},"notUpdated":{},"notDestroyed":{}},"contacts-set"]],"sessionState":"s2"})"},
    });
    javelin::jmap::api::HttpJmapMethodTransport methodTransport{transport};
    javelin::jmap::cache::ContactRepository contacts{connection};
    REQUIRE_FALSE(contacts
                      .replaceAll("a1", {addressBook()},
                                  {cachedContact("card-1", "Original", "original@example.test")},
                                  "b1", "c1")
                      .has_value());
    transport.beforeReturn = [&connection, &contacts]
    {
        const auto projected = contacts.findContact("a1", "card-1");
        REQUIRE(std::holds_alternative<std::optional<javelin::jmap::contacts::ContactSummary>>(
            projected));
        REQUIRE(std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(projected)
                    .has_value());
        CHECK(std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(projected)
                  ->displayName == "Updated");
        javelin::jmap::contacts::ContactMutationJournal journal{connection, contacts};
        const auto mutations = journal.listForContact("a1", "card-1");
        REQUIRE(std::holds_alternative<std::vector<javelin::jmap::contacts::ContactMutationRecord>>(
            mutations));
        REQUIRE(std::get<std::vector<javelin::jmap::contacts::ContactMutationRecord>>(mutations)
                    .size() == 1);
        CHECK(std::get<std::vector<javelin::jmap::contacts::ContactMutationRecord>>(mutations)
                  .front()
                  .status == javelin::jmap::sync::MutationStatus::InFlight);
    };
    javelin::jmap::contacts::ContactService service{connection, contacts, transport,
                                                    methodTransport};
    javelin::jmap::api::ContactCardSetRequest request{
        .accountId = "a1",
        .ifInState = "c1",
        .create = {},
        .update = {{"card-1", {.json = R"({"name/full":"Updated"})"}}},
        .destroy = {},
    };
    const auto result = QCoro::waitFor(
        service.setContactCards({.sessionUrl = "https://example.test/.well-known/jmap",
                                 .loginEmail = "alice@example.test",
                                 .apiKey = "secret"},
                                "a1", std::move(request)));
    REQUIRE(std::holds_alternative<javelin::jmap::contacts::ContactMutationSummary>(result));

    javelin::jmap::sync::ConsistencyDomainRepository consistency{connection};
    const auto contactGeneration =
        consistency.mutationGeneration({.accountId = "a1", .dataType = "ContactCard"});
    const auto addressBookGeneration =
        consistency.mutationGeneration({.accountId = "a1", .dataType = "AddressBook"});
    REQUIRE(std::holds_alternative<std::uint64_t>(contactGeneration));
    REQUIRE(std::holds_alternative<std::uint64_t>(addressBookGeneration));
    CHECK(std::get<std::uint64_t>(contactGeneration) == 1);
    CHECK(std::get<std::uint64_t>(addressBookGeneration) == 0);
    const auto accepted = contacts.findContact("a1", "card-1");
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::contacts::ContactSummary>>(accepted));
    REQUIRE(std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(accepted).has_value());
    CHECK(std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(accepted)->displayName ==
          "Updated");
    javelin::jmap::contacts::ContactMutationJournal journal{connection, contacts};
    const auto mutations = journal.listForContact("a1", "card-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::contacts::ContactMutationRecord>>(
        mutations));
    CHECK(std::get<std::vector<javelin::jmap::contacts::ContactMutationRecord>>(mutations).empty());
}

TEST_CASE("contact group membership uses exact optimistic member patches",
          "[jmap][contacts][groups][consistency]")
{
    ensureApplication();
    QTemporaryDir directory;
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("contact-group-membership-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    REQUIRE_FALSE(sessions.replace("a1", session()).has_value());
    javelin::jmap::cache::ContactRepository contacts{connection};
    REQUIRE_FALSE(
        contacts.replaceAll("a1", {addressBook()}, {cachedGroup("group-1", "Friends")}, "b1", "c1")
            .has_value());

    FakeTransport transport;
    transport.results.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body =
            QByteArray{
                R"({"methodResponses":[["ContactCard/set",{"accountId":"a1","oldState":"c1","newState":"c2","created":{},"updated":{"group-1":{}},"destroyed":[],"notCreated":{},"notUpdated":{},"notDestroyed":{}},"contacts-set"]],"sessionState":"s2"})"},
    });
    transport.beforeReturn = [&connection, &contacts]
    {
        const auto projected = contacts.findContact("a1", "group-1");
        REQUIRE(std::holds_alternative<std::optional<javelin::jmap::contacts::ContactSummary>>(
            projected));
        const auto& group =
            std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(projected);
        REQUIRE(group.has_value());
        CHECK(groupContains(*group, "uid-card-1"));
        CHECK(groupContains(*group, "uid-card-2"));
        javelin::jmap::contacts::ContactMutationJournal journal{connection, contacts};
        const auto mutations = journal.listForContact("a1", "group-1");
        REQUIRE(std::holds_alternative<std::vector<javelin::jmap::contacts::ContactMutationRecord>>(
            mutations));
        REQUIRE(std::get<std::vector<javelin::jmap::contacts::ContactMutationRecord>>(mutations)
                    .size() == 1);
        CHECK(std::get<std::vector<javelin::jmap::contacts::ContactMutationRecord>>(mutations)
                  .front()
                  .status == javelin::jmap::sync::MutationStatus::InFlight);
    };
    javelin::jmap::api::HttpJmapMethodTransport methodTransport{transport};
    javelin::jmap::contacts::ContactService service{connection, contacts, transport,
                                                    methodTransport};
    const auto accepted = QCoro::waitFor(
        service.setGroupMembership({.sessionUrl = "https://example.test/.well-known/jmap",
                                    .loginEmail = "alice@example.test",
                                    .apiKey = "secret"},
                                   "a1",
                                   {.accountId = "a1",
                                    .groupId = "group-1",
                                    .memberUids = {"uid-card-1", "uid-card-2"},
                                    .included = true}));
    REQUIRE(std::holds_alternative<javelin::jmap::contacts::ContactMutationSummary>(accepted));
    REQUIRE_FALSE(transport.requests.empty());
    CHECK(transport.requests.back().body.contains("\"members/uid-card-1\":true"));
    CHECK(transport.requests.back().body.contains("\"members/uid-card-2\":true"));
    auto cached = contacts.findContact("a1", "group-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::contacts::ContactSummary>>(cached));
    REQUIRE(std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(cached).has_value());
    CHECK(groupContains(*std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(cached),
                        "uid-card-1"));
    CHECK(groupContains(*std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(cached),
                        "uid-card-2"));

    transport.results.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body =
            QByteArray{
                R"({"methodResponses":[["ContactCard/set",{"accountId":"a1","oldState":"c2","newState":"c2","created":{},"updated":{},"destroyed":[],"notCreated":{},"notUpdated":{"group-1":{"type":"forbidden"}},"notDestroyed":{}},"contacts-set"]],"sessionState":"s3"})"},
    });
    const auto rejected = QCoro::waitFor(
        service.setGroupMembership({.sessionUrl = "https://example.test/.well-known/jmap",
                                    .loginEmail = "alice@example.test",
                                    .apiKey = "secret"},
                                   "a1",
                                   {.accountId = "a1",
                                    .groupId = "group-1",
                                    .memberUids = {"uid-card-1", "uid-card-2"},
                                    .included = false}));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(rejected));
    CHECK(transport.requests.back().body.contains("\"members/uid-card-1\":null"));
    CHECK(transport.requests.back().body.contains("\"members/uid-card-2\":null"));
    cached = contacts.findContact("a1", "group-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::contacts::ContactSummary>>(cached));
    REQUIRE(std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(cached).has_value());
    CHECK(groupContains(*std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(cached),
                        "uid-card-1"));
    CHECK(groupContains(*std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(cached),
                        "uid-card-2"));
    javelin::jmap::contacts::ContactMutationJournal journal{connection, contacts};
    const auto mutations = journal.listForContact("a1", "group-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::contacts::ContactMutationRecord>>(
        mutations));
    REQUIRE(
        std::get<std::vector<javelin::jmap::contacts::ContactMutationRecord>>(mutations).size() ==
        1);
    CHECK(std::get<std::vector<javelin::jmap::contacts::ContactMutationRecord>>(mutations)
              .front()
              .status == javelin::jmap::sync::MutationStatus::Rejected);

    transport.results.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body =
            QByteArray{
                R"({"methodResponses":[["error",{"type":"stateMismatch","description":"Contacts changed on the server."},"contacts-set"]],"sessionState":"s3"})"},
    });
    const auto stateMismatch = QCoro::waitFor(service.setGroupMembership(
        {.sessionUrl = "https://example.test/.well-known/jmap",
         .loginEmail = "alice@example.test",
         .apiKey = "secret"},
        "a1",
        {.accountId = "a1", .groupId = "group-1", .memberUids = {"uid-card-3"}, .included = true}));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(stateMismatch));
    const auto& conflict = std::get<javelin::jmap::OperationError>(stateMismatch);
    CHECK(conflict.code == javelin::jmap::OperationErrorCode::Conflict);
    CHECK(conflict.message == QStringLiteral("Contacts changed on the server."));
    CHECK(conflict.protocolType == "stateMismatch");
    cached = contacts.findContact("a1", "group-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::contacts::ContactSummary>>(cached));
    REQUIRE(std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(cached).has_value());
    CHECK_FALSE(groupContains(
        *std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(cached), "uid-card-3"));

    transport.results.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body =
            QByteArray{
                R"({"methodResponses":[["ContactCard/get",{"accountId":"a1","state":"c2","list":[],"notFound":[]},"contacts-set"],["ContactCard/set",{"accountId":"a1","oldState":"c2","newState":"c3","updated":{"group-1":null}},"other-call"]],"sessionState":"s4"})"},
    });
    const auto mismatchedResponse = QCoro::waitFor(service.setGroupMembership(
        {.sessionUrl = "https://example.test/.well-known/jmap",
         .loginEmail = "alice@example.test",
         .apiKey = "secret"},
        "a1",
        {.accountId = "a1", .groupId = "group-1", .memberUids = {"uid-card-4"}, .included = true}));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(mismatchedResponse));
    const auto& protocolError = std::get<javelin::jmap::OperationError>(mismatchedResponse);
    CHECK(protocolError.code == javelin::jmap::OperationErrorCode::ProtocolViolation);
    CHECK(protocolError.message ==
          QStringLiteral("The Contacts response omitted ContactCard/set(contacts-set); received "
                         "ContactCard/get(contacts-set), ContactCard/set(other-call)."));
}

TEST_CASE("AddressBook mutations project, reconcile rejection, and preserve uncertainty",
          "[jmap][contacts][service][consistency]")
{
    ensureApplication();
    QTemporaryDir directory;
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("address-book-optimistic-mutation-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    REQUIRE_FALSE(sessions.replace("a1", session()).has_value());
    javelin::jmap::cache::ContactRepository contacts{connection};
    REQUIRE_FALSE(contacts.replaceAll("a1", {addressBook()}, {}, "b1", "c1").has_value());

    FakeTransport transport;
    transport.results.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body =
            QByteArray{
                R"({"methodResponses":[["AddressBook/set",{"accountId":"a1","oldState":"b1","newState":"b2","created":{},"updated":{"book-1":{}},"destroyed":[],"notCreated":{},"notUpdated":{},"notDestroyed":{}},"contacts-set"]],"sessionState":"s2"})"},
    });
    transport.beforeReturn = [&connection, &contacts]
    {
        const auto books = contacts.listAddressBooks("a1");
        REQUIRE(std::holds_alternative<std::vector<javelin::jmap::api::AddressBook>>(books));
        REQUIRE(std::get<std::vector<javelin::jmap::api::AddressBook>>(books).size() == 1);
        CHECK(std::get<std::vector<javelin::jmap::api::AddressBook>>(books).front().name ==
              "Renamed");
        javelin::jmap::contacts::AddressBookMutationJournal journal{connection, contacts};
        const auto records = journal.listForAddressBook("a1", "book-1");
        REQUIRE(
            std::holds_alternative<std::vector<javelin::jmap::contacts::AddressBookMutationRecord>>(
                records));
        REQUIRE(std::get<std::vector<javelin::jmap::contacts::AddressBookMutationRecord>>(records)
                    .size() == 1);
        CHECK(std::get<std::vector<javelin::jmap::contacts::AddressBookMutationRecord>>(records)
                  .front()
                  .status == javelin::jmap::sync::MutationStatus::InFlight);
    };
    javelin::jmap::api::HttpJmapMethodTransport methodTransport{transport};
    javelin::jmap::contacts::ContactService service{connection, contacts, transport,
                                                    methodTransport};
    const auto accepted = QCoro::waitFor(
        service.setAddressBooks({.sessionUrl = "https://example.test/.well-known/jmap",
                                 .loginEmail = "alice@example.test",
                                 .apiKey = "secret"},
                                "a1",
                                {.accountId = "a1",
                                 .ifInState = std::nullopt,
                                 .create = {},
                                 .update = {{"book-1", {.json = R"({"name":"Renamed"})"}}},
                                 .destroy = {},
                                 .onDestroyRemoveContents = false,
                                 .onSuccessSetIsDefault = std::nullopt}));
    REQUIRE(std::holds_alternative<javelin::jmap::contacts::ContactMutationSummary>(accepted));
    REQUIRE_FALSE(transport.requests.empty());
    CHECK(transport.requests.front().body.contains("\"ifInState\":\"b1\""));
    auto books = contacts.listAddressBooks("a1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::api::AddressBook>>(books));
    CHECK(std::get<std::vector<javelin::jmap::api::AddressBook>>(books).front().name == "Renamed");

    transport.results.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body =
            QByteArray{
                R"({"methodResponses":[["AddressBook/set",{"accountId":"a1","oldState":"b2","newState":"b3","created":{"create-1":{"id":"book-2"}},"updated":{},"destroyed":[],"notCreated":{},"notUpdated":{},"notDestroyed":{}},"contacts-set"]],"sessionState":"s-create"})"},
    });
    transport.beforeReturn = [&contacts]
    {
        const auto projected = contacts.listAddressBooks("a1");
        REQUIRE(std::holds_alternative<std::vector<javelin::jmap::api::AddressBook>>(projected));
        CHECK(std::ranges::any_of(std::get<std::vector<javelin::jmap::api::AddressBook>>(projected),
                                  [](const auto& book) { return book.id.starts_with("local-"); }));
    };
    const auto created = QCoro::waitFor(service.setAddressBooks(
        {.sessionUrl = "https://example.test/.well-known/jmap",
         .loginEmail = "alice@example.test",
         .apiKey = "secret"},
        "a1",
        {.accountId = "a1",
         .ifInState = std::nullopt,
         .create = {{"create-1", {.json = R"({"name":"Projects","isSubscribed":true})"}}},
         .update = {},
         .destroy = {},
         .onDestroyRemoveContents = false,
         .onSuccessSetIsDefault = std::nullopt}));
    REQUIRE(std::holds_alternative<javelin::jmap::contacts::ContactMutationSummary>(created));
    CHECK(std::get<javelin::jmap::contacts::ContactMutationSummary>(created).createdId ==
          std::optional<std::string>{"book-2"});
    CHECK(std::get<javelin::jmap::contacts::ContactMutationSummary>(created).createdIds ==
          std::vector<javelin::jmap::contacts::CreatedContactMapping>{
              {.creationId = "create-1", .serverId = "book-2"}});
    books = contacts.listAddressBooks("a1");
    const auto& createdBooks = std::get<std::vector<javelin::jmap::api::AddressBook>>(books);
    CHECK(std::ranges::none_of(createdBooks,
                               [](const auto& book) { return book.id.starts_with("local-"); }));
    CHECK(std::ranges::any_of(createdBooks, [](const auto& book)
                              { return book.id == "book-2" && book.name == "Projects"; }));

    transport.results.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body =
            QByteArray{
                R"({"methodResponses":[["AddressBook/set",{"accountId":"a1","oldState":"b3","newState":"b3","created":{},"updated":{},"destroyed":[],"notCreated":{},"notUpdated":{"book-1":{"type":"forbidden"}},"notDestroyed":{}},"contacts-set"]],"sessionState":"s3"})"},
    });
    const auto rejected = QCoro::waitFor(
        service.setAddressBooks({.sessionUrl = "https://example.test/.well-known/jmap",
                                 .loginEmail = "alice@example.test",
                                 .apiKey = "secret"},
                                "a1",
                                {.accountId = "a1",
                                 .ifInState = std::nullopt,
                                 .create = {},
                                 .update = {{"book-1", {.json = R"({"name":"Rejected"})"}}},
                                 .destroy = {},
                                 .onDestroyRemoveContents = false,
                                 .onSuccessSetIsDefault = std::nullopt}));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(rejected));
    CHECK(std::get<javelin::jmap::OperationError>(rejected).code ==
          javelin::jmap::OperationErrorCode::Conflict);
    books = contacts.listAddressBooks("a1");
    const auto& rejectedBooks = std::get<std::vector<javelin::jmap::api::AddressBook>>(books);
    const auto restoredBook =
        std::ranges::find(rejectedBooks, "book-1", &javelin::jmap::api::AddressBook::id);
    REQUIRE(restoredBook != rejectedBooks.end());
    CHECK(restoredBook->name == "Renamed");

    transport.results.push_back(javelin::jmap::api::TransportError{
        .code = javelin::jmap::api::TransportErrorCode::NetworkFailure,
        .message = "Connection closed after dispatch",
    });
    const auto uncertain = QCoro::waitFor(
        service.setAddressBooks({.sessionUrl = "https://example.test/.well-known/jmap",
                                 .loginEmail = "alice@example.test",
                                 .apiKey = "secret"},
                                "a1",
                                {.accountId = "a1",
                                 .ifInState = std::nullopt,
                                 .create = {},
                                 .update = {{"book-1", {.json = R"({"name":"Uncertain"})"}}},
                                 .destroy = {},
                                 .onDestroyRemoveContents = false,
                                 .onSuccessSetIsDefault = std::nullopt}));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(uncertain));
    books = contacts.listAddressBooks("a1");
    const auto& uncertainBooks = std::get<std::vector<javelin::jmap::api::AddressBook>>(books);
    const auto uncertainBook =
        std::ranges::find(uncertainBooks, "book-1", &javelin::jmap::api::AddressBook::id);
    REQUIRE(uncertainBook != uncertainBooks.end());
    CHECK(uncertainBook->name == "Uncertain");
    javelin::jmap::contacts::AddressBookMutationJournal journal{connection, contacts};
    const auto records = journal.listForAddressBook("a1", "book-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::contacts::AddressBookMutationRecord>>(
        records));
    CHECK(std::ranges::any_of(
        std::get<std::vector<javelin::jmap::contacts::AddressBookMutationRecord>>(records),
        [](const auto& record)
        { return record.status == javelin::jmap::sync::MutationStatus::Unknown; }));
}

TEST_CASE("rejected ContactCard sets restore the confirmed contact",
          "[jmap][contacts][service][consistency]")
{
    ensureApplication();
    QTemporaryDir directory;
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("contact-service-mutation-rejection-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    REQUIRE_FALSE(sessions.replace("a1", session()).has_value());
    javelin::jmap::cache::ContactRepository contacts{connection};
    REQUIRE_FALSE(contacts
                      .replaceAll("a1", {addressBook()},
                                  {cachedContact("card-1", "Original", "original@example.test")},
                                  "b1", "c1")
                      .has_value());

    FakeTransport transport;
    transport.results.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body =
            QByteArray{
                R"({"methodResponses":[["ContactCard/set",{"accountId":"a1","oldState":"c1","newState":"c1","created":{},"updated":{},"destroyed":[],"notCreated":{},"notUpdated":{"card-1":{"type":"forbidden"}},"notDestroyed":{}},"contacts-set"]],"sessionState":"s2"})"},
    });
    javelin::jmap::api::HttpJmapMethodTransport methodTransport{transport};
    javelin::jmap::contacts::ContactService service{connection, contacts, transport,
                                                    methodTransport};
    const auto result = QCoro::waitFor(
        service.setContactCards({.sessionUrl = "https://example.test/.well-known/jmap",
                                 .loginEmail = "alice@example.test",
                                 .apiKey = "secret"},
                                "a1",
                                {.accountId = "a1",
                                 .ifInState = "c1",
                                 .create = {},
                                 .update = {{"card-1", {.json = R"({"name/full":"Rejected"})"}}},
                                 .destroy = {}}));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(result));
    INFO(std::get<javelin::jmap::OperationError>(result).message.toStdString());
    CHECK(std::get<javelin::jmap::OperationError>(result).code ==
          javelin::jmap::OperationErrorCode::Conflict);

    const auto restored = contacts.findContact("a1", "card-1");
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::contacts::ContactSummary>>(restored));
    REQUIRE(std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(restored).has_value());
    CHECK(std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(restored)->displayName ==
          "Original");
    javelin::jmap::contacts::ContactMutationJournal journal{connection, contacts};
    const auto mutations = journal.listForContact("a1", "card-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::contacts::ContactMutationRecord>>(
        mutations));
    REQUIRE(
        std::get<std::vector<javelin::jmap::contacts::ContactMutationRecord>>(mutations).size() ==
        1);
    CHECK(std::get<std::vector<javelin::jmap::contacts::ContactMutationRecord>>(mutations)
              .front()
              .status == javelin::jmap::sync::MutationStatus::Rejected);
}

TEST_CASE("partial ContactCard results reconcile each object independently",
          "[jmap][contacts][service][consistency]")
{
    ensureApplication();
    QTemporaryDir directory;
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("contact-service-mutation-partial-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    REQUIRE_FALSE(sessions.replace("a1", session()).has_value());
    javelin::jmap::cache::ContactRepository contacts{connection};
    REQUIRE_FALSE(contacts
                      .replaceAll("a1", {addressBook()},
                                  {cachedContact("card-1", "One", "one@example.test"),
                                   cachedContact("card-2", "Two", "two@example.test")},
                                  "b1", "c1")
                      .has_value());

    FakeTransport transport;
    transport.results.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body =
            QByteArray{
                R"({"methodResponses":[["ContactCard/set",{"accountId":"a1","oldState":"c1","newState":"c2","created":{},"updated":{"card-1":{}},"destroyed":[],"notCreated":{},"notUpdated":{"card-2":{"type":"forbidden"}},"notDestroyed":{}},"contacts-set"]],"sessionState":"s2"})"},
    });
    javelin::jmap::api::HttpJmapMethodTransport methodTransport{transport};
    javelin::jmap::contacts::ContactService service{connection, contacts, transport,
                                                    methodTransport};
    const auto result = QCoro::waitFor(
        service.setContactCards({.sessionUrl = "https://example.test/.well-known/jmap",
                                 .loginEmail = "alice@example.test",
                                 .apiKey = "secret"},
                                "a1",
                                {.accountId = "a1",
                                 .ifInState = "c1",
                                 .create = {},
                                 .update =
                                     {
                                         {"card-1", {.json = R"({"name/full":"Accepted"})"}},
                                         {"card-2", {.json = R"({"name/full":"Rejected"})"}},
                                     },
                                 .destroy = {}}));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(result));
    INFO(std::get<javelin::jmap::OperationError>(result).message.toStdString());
    CHECK(std::get<javelin::jmap::OperationError>(result).code ==
          javelin::jmap::OperationErrorCode::Conflict);

    const auto accepted = contacts.findContact("a1", "card-1");
    const auto rejected = contacts.findContact("a1", "card-2");
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::contacts::ContactSummary>>(accepted));
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::contacts::ContactSummary>>(rejected));
    REQUIRE(std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(accepted).has_value());
    REQUIRE(std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(rejected).has_value());
    CHECK(std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(accepted)->displayName ==
          "Accepted");
    CHECK(std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(rejected)->displayName ==
          "Two");

    javelin::jmap::contacts::ContactMutationJournal journal{connection, contacts};
    const auto acceptedMutations = journal.listForContact("a1", "card-1");
    const auto rejectedMutations = journal.listForContact("a1", "card-2");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::contacts::ContactMutationRecord>>(
        acceptedMutations));
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::contacts::ContactMutationRecord>>(
        rejectedMutations));
    CHECK(std::get<std::vector<javelin::jmap::contacts::ContactMutationRecord>>(acceptedMutations)
              .empty());
    REQUIRE(std::get<std::vector<javelin::jmap::contacts::ContactMutationRecord>>(rejectedMutations)
                .size() == 1);
    CHECK(std::get<std::vector<javelin::jmap::contacts::ContactMutationRecord>>(rejectedMutations)
              .front()
              .status == javelin::jmap::sync::MutationStatus::Rejected);
}

TEST_CASE("ambiguous ContactCard outcomes preserve the optimistic contact",
          "[jmap][contacts][service][consistency]")
{
    ensureApplication();
    QTemporaryDir directory;
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("contact-service-mutation-unknown-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    REQUIRE_FALSE(sessions.replace("a1", session()).has_value());
    javelin::jmap::cache::ContactRepository contacts{connection};
    REQUIRE_FALSE(contacts
                      .replaceAll("a1", {addressBook()},
                                  {cachedContact("card-1", "Original", "original@example.test")},
                                  "b1", "c1")
                      .has_value());

    FakeTransport transport;
    transport.results.push_back(javelin::jmap::api::TransportError{
        .code = javelin::jmap::api::TransportErrorCode::NetworkFailure,
        .message = "Connection closed after dispatch",
    });
    javelin::jmap::api::HttpJmapMethodTransport methodTransport{transport};
    javelin::jmap::contacts::ContactService service{connection, contacts, transport,
                                                    methodTransport};
    const auto result = QCoro::waitFor(
        service.setContactCards({.sessionUrl = "https://example.test/.well-known/jmap",
                                 .loginEmail = "alice@example.test",
                                 .apiKey = "secret"},
                                "a1",
                                {.accountId = "a1",
                                 .ifInState = "c1",
                                 .create = {},
                                 .update = {{"card-1", {.json = R"({"name/full":"Uncertain"})"}}},
                                 .destroy = {}}));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(result));

    const auto projected = contacts.findContact("a1", "card-1");
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::contacts::ContactSummary>>(projected));
    REQUIRE(
        std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(projected).has_value());
    CHECK(
        std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(projected)->displayName ==
        "Uncertain");
    javelin::jmap::contacts::ContactMutationJournal journal{connection, contacts};
    const auto mutations = journal.listForContact("a1", "card-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::contacts::ContactMutationRecord>>(
        mutations));
    REQUIRE(
        std::get<std::vector<javelin::jmap::contacts::ContactMutationRecord>>(mutations).size() ==
        1);
    CHECK(std::get<std::vector<javelin::jmap::contacts::ContactMutationRecord>>(mutations)
              .front()
              .status == javelin::jmap::sync::MutationStatus::Unknown);

    transport.results.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body =
            QByteArray{
                R"({"methodResponses":[["AddressBook/get",{"accountId":"a1","state":"b2","list":[{"id":"book-1","name":"Personal","description":null,"sortOrder":0,"isDefault":true,"isSubscribed":true,"shareWith":null,"myRights":{"mayRead":true,"mayWrite":true,"mayShare":false,"mayDelete":true}}],"notFound":[]},"address-books"],["ContactCard/get",{"accountId":"a1","state":"c2","list":[{"id":"card-1","uid":"uid-card-1","kind":"individual","addressBookIds":{"book-1":true},"name":{"full":"Original"},"emails":{"email-1":{"address":"original@example.test"}}}],"notFound":[]},"contact-cards"]],"sessionState":"s3"})"},
    });
    const auto staleRefresh =
        QCoro::waitFor(service.refreshAll({.sessionUrl = "https://example.test/.well-known/jmap",
                                           .loginEmail = "alice@example.test",
                                           .apiKey = "secret"},
                                          "a1"));
    REQUIRE(std::holds_alternative<javelin::jmap::contacts::ContactRefreshSummary>(staleRefresh));
    const auto stillProjected = contacts.findContact("a1", "card-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::contacts::ContactSummary>>(
        stillProjected));
    REQUIRE(std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(stillProjected)
                .has_value());
    CHECK(std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(stillProjected)
              ->displayName == "Uncertain");

    transport.results.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body =
            QByteArray{
                R"({"methodResponses":[["AddressBook/get",{"accountId":"a1","state":"b3","list":[{"id":"book-1","name":"Personal","description":null,"sortOrder":0,"isDefault":true,"isSubscribed":true,"shareWith":null,"myRights":{"mayRead":true,"mayWrite":true,"mayShare":false,"mayDelete":true}}],"notFound":[]},"address-books"],["ContactCard/get",{"accountId":"a1","state":"c3","list":[{"id":"card-1","uid":"uid-card-1","kind":"individual","addressBookIds":{"book-1":true},"name":{"full":"Uncertain"},"emails":{"email-1":{"address":"original@example.test"}}}],"notFound":[]},"contact-cards"]],"sessionState":"s4"})"},
    });
    const auto confirmedRefresh =
        QCoro::waitFor(service.refreshAll({.sessionUrl = "https://example.test/.well-known/jmap",
                                           .loginEmail = "alice@example.test",
                                           .apiKey = "secret"},
                                          "a1"));
    REQUIRE(
        std::holds_alternative<javelin::jmap::contacts::ContactRefreshSummary>(confirmedRefresh));
    const auto remaining = journal.listActive("a1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::contacts::ContactMutationRecord>>(
        remaining));
    CHECK(std::get<std::vector<javelin::jmap::contacts::ContactMutationRecord>>(remaining).empty());
}

TEST_CASE("accepted ContactCard creation replaces its temporary projection",
          "[jmap][contacts][service][consistency]")
{
    ensureApplication();
    QTemporaryDir directory;
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("contact-service-mutation-create-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    REQUIRE_FALSE(sessions.replace("a1", session()).has_value());
    javelin::jmap::cache::ContactRepository contacts{connection};
    REQUIRE_FALSE(contacts.replaceAll("a1", {addressBook()}, {}, "b1", "c1").has_value());

    FakeTransport transport;
    transport.results.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body =
            QByteArray{
                R"({"methodResponses":[["ContactCard/set",{"accountId":"a1","oldState":"c1","newState":"c2","created":{"create-1":{"id":"card-2","updated":"2026-07-17T00:00:00Z"}},"updated":{},"destroyed":[],"notCreated":{},"notUpdated":{},"notDestroyed":{}},"contacts-set"]],"sessionState":"s2"})"},
    });
    transport.beforeReturn = [&contacts]
    {
        const auto projected = contacts.listContacts("a1");
        REQUIRE(std::holds_alternative<std::vector<javelin::jmap::contacts::ContactSummary>>(
            projected));
        const auto& values =
            std::get<std::vector<javelin::jmap::contacts::ContactSummary>>(projected);
        REQUIRE(values.size() == 1);
        CHECK(values.front().id.starts_with("local-"));
        CHECK(values.front().displayName == "New Contact");
    };
    javelin::jmap::api::HttpJmapMethodTransport methodTransport{transport};
    javelin::jmap::contacts::ContactService service{connection, contacts, transport,
                                                    methodTransport};
    const auto result = QCoro::waitFor(service.setContactCards(
        {.sessionUrl = "https://example.test/.well-known/jmap",
         .loginEmail = "alice@example.test",
         .apiKey = "secret"},
        "a1",
        {.accountId = "a1",
         .ifInState = "c1",
         .create =
             {{"create-1",
               {.json =
                    R"({"uid":"uid-new","kind":"individual","addressBookIds":{"book-1":true},"name":{"full":"New Contact"},"emails":{}})"}}},
         .update = {},
         .destroy = {}}));
    if (const auto* operationError = std::get_if<javelin::jmap::OperationError>(&result))
    {
        FAIL(operationError->message.toStdString());
    }
    REQUIRE(std::holds_alternative<javelin::jmap::contacts::ContactMutationSummary>(result));
    CHECK(std::get<javelin::jmap::contacts::ContactMutationSummary>(result).createdId ==
          std::optional<std::string>{"card-2"});
    CHECK(std::get<javelin::jmap::contacts::ContactMutationSummary>(result).createdIds ==
          std::vector<javelin::jmap::contacts::CreatedContactMapping>{
              {.creationId = "create-1", .serverId = "card-2"}});

    const auto accepted = contacts.listContacts("a1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::contacts::ContactSummary>>(accepted));
    const auto& values = std::get<std::vector<javelin::jmap::contacts::ContactSummary>>(accepted);
    REQUIRE(values.size() == 1);
    CHECK(values.front().id == "card-2");
    CHECK(values.front().displayName == "New Contact");
}

TEST_CASE("ContactCard move-by-copy projects and reconciles both accounts atomically",
          "[jmap][contacts][service][consistency][copy]")
{
    ensureApplication();
    QTemporaryDir directory;
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("contact-service-copy-group-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    REQUIRE_FALSE(sessions.replace("a1", sessionWithContactDestination()).has_value());
    javelin::jmap::cache::ContactRepository contacts{connection};
    REQUIRE_FALSE(contacts
                      .replaceAll("a1", {addressBook()},
                                  {cachedContact("card-1", "Original", "original@example.test")},
                                  "b1", "source-1")
                      .has_value());
    REQUIRE_FALSE(
        contacts.replaceAll("a2", {addressBook()}, {}, "b2", "destination-1").has_value());

    FakeTransport transport;
    transport.results.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body =
            QByteArray{
                R"({"methodResponses":[["ContactCard/copy",{"fromAccountId":"a1","accountId":"a2","oldState":"destination-1","newState":"destination-2","created":{"copy-1":{"id":"card-2"}},"notCreated":{}},"contacts-copy"],["ContactCard/set",{"accountId":"a1","oldState":"source-1","newState":"source-2","created":{},"updated":{},"destroyed":["card-1"],"notCreated":{},"notUpdated":{},"notDestroyed":{}},"contacts-copy"]],"sessionState":"s2"})"},
    });
    transport.beforeReturn = [&contacts]
    {
        const auto source = contacts.findContact("a1", "card-1");
        const auto destination = contacts.listContacts("a2");
        REQUIRE(
            std::holds_alternative<std::optional<javelin::jmap::contacts::ContactSummary>>(source));
        REQUIRE(std::holds_alternative<std::vector<javelin::jmap::contacts::ContactSummary>>(
            destination));
        CHECK_FALSE(
            std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(source).has_value());
        const auto& projected =
            std::get<std::vector<javelin::jmap::contacts::ContactSummary>>(destination);
        REQUIRE(projected.size() == 1);
        CHECK(projected.front().id.starts_with("local-"));
        CHECK(projected.front().displayName == "Original");
    };
    javelin::jmap::api::HttpJmapMethodTransport methodTransport{transport};
    javelin::jmap::contacts::ContactService service{connection, contacts, transport,
                                                    methodTransport};
    const auto result = QCoro::waitFor(
        service.copyContactCards({.sessionUrl = "https://example.test/.well-known/jmap",
                                  .loginEmail = "alice@example.test",
                                  .apiKey = "secret"},
                                 "a1",
                                 {.fromAccountId = "a1",
                                  .accountId = "a2",
                                  .ifFromInState = std::nullopt,
                                  .ifInState = std::nullopt,
                                  .create = {{"copy-1", {.json = R"({"id":"card-1"})"}}},
                                  .onSuccessDestroyOriginal = true,
                                  .destroyFromIfInState = std::nullopt}));
    REQUIRE(std::holds_alternative<javelin::jmap::contacts::ContactMutationSummary>(result));
    CHECK(transport.lastRequest.body.contains(R"("ifFromInState":"source-1")"));
    CHECK(transport.lastRequest.body.contains(R"("ifInState":"destination-1")"));
    CHECK(transport.lastRequest.body.contains(R"("destroyFromIfInState":"source-1")"));

    const auto source = contacts.findContact("a1", "card-1");
    const auto destination = contacts.findContact("a2", "card-2");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::contacts::ContactSummary>>(source));
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::contacts::ContactSummary>>(
        destination));
    CHECK_FALSE(
        std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(source).has_value());
    REQUIRE(
        std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(destination).has_value());
    CHECK(std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(destination)
              ->displayName == "Original");
}

TEST_CASE("a failed source deletion restores only the source side of ContactCard copy",
          "[jmap][contacts][service][consistency][copy]")
{
    ensureApplication();
    QTemporaryDir directory;
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("contact-service-copy-delete-rejected-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    REQUIRE_FALSE(sessions.replace("a1", sessionWithContactDestination()).has_value());
    javelin::jmap::cache::ContactRepository contacts{connection};
    REQUIRE_FALSE(contacts
                      .replaceAll("a1", {addressBook()},
                                  {cachedContact("card-1", "Original", "original@example.test")},
                                  "b1", "source-1")
                      .has_value());
    REQUIRE_FALSE(
        contacts.replaceAll("a2", {addressBook()}, {}, "b2", "destination-1").has_value());

    FakeTransport transport;
    transport.results.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body =
            QByteArray{
                R"({"methodResponses":[["ContactCard/copy",{"fromAccountId":"a1","accountId":"a2","oldState":"destination-1","newState":"destination-2","created":{"copy-1":{"id":"card-2"}},"notCreated":{}},"contacts-copy"],["ContactCard/set",{"accountId":"a1","oldState":"source-1","newState":"source-1","created":{},"updated":{},"destroyed":[],"notCreated":{},"notUpdated":{},"notDestroyed":{"card-1":{"type":"forbidden"}}},"contacts-copy"]],"sessionState":"s2"})"},
    });
    javelin::jmap::api::HttpJmapMethodTransport methodTransport{transport};
    javelin::jmap::contacts::ContactService service{connection, contacts, transport,
                                                    methodTransport};
    const auto result = QCoro::waitFor(
        service.copyContactCards({.sessionUrl = "https://example.test/.well-known/jmap",
                                  .loginEmail = "alice@example.test",
                                  .apiKey = "secret"},
                                 "a1",
                                 {.fromAccountId = "a1",
                                  .accountId = "a2",
                                  .ifFromInState = std::nullopt,
                                  .ifInState = std::nullopt,
                                  .create = {{"copy-1", {.json = R"({"id":"card-1"})"}}},
                                  .onSuccessDestroyOriginal = true,
                                  .destroyFromIfInState = std::nullopt}));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(result));
    CHECK(std::get<javelin::jmap::OperationError>(result).code ==
          javelin::jmap::OperationErrorCode::Conflict);

    const auto source = contacts.findContact("a1", "card-1");
    const auto destination = contacts.findContact("a2", "card-2");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::contacts::ContactSummary>>(source));
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::contacts::ContactSummary>>(
        destination));
    REQUIRE(std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(source).has_value());
    REQUIRE(
        std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(destination).has_value());
}

TEST_CASE("an unconfirmed source deletion remains optimistically hidden after ContactCard copy",
          "[jmap][contacts][service][consistency][copy]")
{
    ensureApplication();
    QTemporaryDir directory;
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("contact-service-copy-delete-unknown-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    REQUIRE_FALSE(sessions.replace("a1", sessionWithContactDestination()).has_value());
    javelin::jmap::cache::ContactRepository contacts{connection};
    REQUIRE_FALSE(contacts
                      .replaceAll("a1", {addressBook()},
                                  {cachedContact("card-1", "Original", "original@example.test")},
                                  "b1", "source-1")
                      .has_value());
    REQUIRE_FALSE(
        contacts.replaceAll("a2", {addressBook()}, {}, "b2", "destination-1").has_value());

    FakeTransport transport;
    transport.results.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body =
            QByteArray{
                R"({"methodResponses":[["ContactCard/copy",{"fromAccountId":"a1","accountId":"a2","oldState":"destination-1","newState":"destination-2","created":{"copy-1":{"id":"card-2"}},"notCreated":{}},"contacts-copy"]],"sessionState":"s2"})"},
    });
    javelin::jmap::api::HttpJmapMethodTransport methodTransport{transport};
    javelin::jmap::contacts::ContactService service{connection, contacts, transport,
                                                    methodTransport};
    const auto result = QCoro::waitFor(
        service.copyContactCards({.sessionUrl = "https://example.test/.well-known/jmap",
                                  .loginEmail = "alice@example.test",
                                  .apiKey = "secret"},
                                 "a1",
                                 {.fromAccountId = "a1",
                                  .accountId = "a2",
                                  .ifFromInState = std::nullopt,
                                  .ifInState = std::nullopt,
                                  .create = {{"copy-1", {.json = R"({"id":"card-1"})"}}},
                                  .onSuccessDestroyOriginal = true,
                                  .destroyFromIfInState = std::nullopt}));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(result));
    CHECK(std::get<javelin::jmap::OperationError>(result).code ==
          javelin::jmap::OperationErrorCode::ProtocolViolation);

    const auto source = contacts.findContact("a1", "card-1");
    const auto destination = contacts.findContact("a2", "card-2");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::contacts::ContactSummary>>(source));
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::contacts::ContactSummary>>(
        destination));
    CHECK_FALSE(
        std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(source).has_value());
    REQUIRE(
        std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(destination).has_value());
    javelin::jmap::contacts::ContactMutationJournal journal{connection, contacts};
    const auto sourceMutations = journal.listForContact("a1", "card-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::contacts::ContactMutationRecord>>(
        sourceMutations));
    REQUIRE(std::get<std::vector<javelin::jmap::contacts::ContactMutationRecord>>(sourceMutations)
                .size() == 1);
    CHECK(std::get<std::vector<javelin::jmap::contacts::ContactMutationRecord>>(sourceMutations)
              .front()
              .status == javelin::jmap::sync::MutationStatus::Unknown);
}

TEST_CASE("contact service incrementally applies paged changes with bounded gets",
          "[jmap][contacts][service][sync]")
{
    ensureApplication();
    QTemporaryDir directory;
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("contact-service-incremental-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    REQUIRE_FALSE(sessions.replace("a1", session()).has_value());
    javelin::jmap::cache::ContactRepository contacts{connection};
    REQUIRE_FALSE(contacts
                      .replaceAll("a1", {addressBook()},
                                  {cachedContact("c1", "Old One", "one@example.test"),
                                   cachedContact("c2", "Old Two", "two@example.test")},
                                  "b1", "c1")
                      .has_value());

    FakeTransport transport;
    transport.results = {
        javelin::jmap::api::HttpResponse{
            .statusCode = 200,
            .body =
                QByteArray{
                    R"({"methodResponses":[["AddressBook/get",{"accountId":"a1","state":"b2","list":[{"id":"book-1","name":"Renamed","description":null,"sortOrder":0,"isDefault":true,"isSubscribed":true,"shareWith":null,"myRights":{"mayRead":true,"mayWrite":true,"mayShare":false,"mayDelete":true}}],"notFound":[]},"contacts-method"]],"sessionState":"s2"})"}},
        javelin::jmap::api::HttpResponse{
            .statusCode = 200,
            .body =
                QByteArray{
                    R"({"methodResponses":[["ContactCard/changes",{"accountId":"a1","oldState":"c1","newState":"c-mid","hasMoreChanges":true,"created":["c3"],"updated":["c1"],"destroyed":["c2"]},"contacts-method"]],"sessionState":"s2"})"}},
        javelin::jmap::api::HttpResponse{
            .statusCode = 200,
            .body =
                QByteArray{
                    R"({"methodResponses":[["ContactCard/get",{"accountId":"a1","state":"c-mid","list":[{"id":"c3","uid":"uid-c3","kind":"individual","addressBookIds":{"book-1":true},"name":{"full":"New Three"},"emails":{"e1":{"address":"three@example.test"}}}],"notFound":[]},"contacts-method"]],"sessionState":"s2"})"}},
        javelin::jmap::api::HttpResponse{
            .statusCode = 200,
            .body =
                QByteArray{
                    R"({"methodResponses":[["ContactCard/get",{"accountId":"a1","state":"c-mid","list":[{"id":"c1","uid":"uid-c1","kind":"individual","addressBookIds":{"book-1":true},"name":{"full":"Updated One"},"emails":{"e1":{"address":"one@example.test"}}}],"notFound":[]},"contacts-method"]],"sessionState":"s2"})"}},
        javelin::jmap::api::HttpResponse{
            .statusCode = 200,
            .body =
                QByteArray{
                    R"({"methodResponses":[["ContactCard/changes",{"accountId":"a1","oldState":"c-mid","newState":"c2","hasMoreChanges":false,"created":[],"updated":["c1"],"destroyed":[]},"contacts-method"]],"sessionState":"s2"})"}},
        javelin::jmap::api::HttpResponse{
            .statusCode = 200,
            .body =
                QByteArray{
                    R"({"methodResponses":[["ContactCard/get",{"accountId":"a1","state":"c2","list":[],"notFound":["c1"]},"contacts-method"]],"sessionState":"s2"})"}},
    };
    javelin::jmap::api::HttpJmapMethodTransport methodTransport{transport};
    javelin::jmap::contacts::ContactService service{connection, contacts, transport,
                                                    methodTransport};
    const auto result =
        QCoro::waitFor(service.refreshAll({.sessionUrl = "https://example.test/.well-known/jmap",
                                           .loginEmail = "alice@example.test",
                                           .apiKey = "secret"},
                                          "a1"));
    REQUIRE(std::holds_alternative<javelin::jmap::contacts::ContactRefreshSummary>(result));
    CHECK(std::get<javelin::jmap::contacts::ContactRefreshSummary>(result).contactCount == 1);
    REQUIRE(transport.requests.size() == 6);
    CHECK(transport.requests[1].body.contains("ContactCard/changes"));
    CHECK(transport.requests[2].body.contains(R"("ids":["c3"])"));
    CHECK(transport.requests[3].body.contains(R"("ids":["c1"])"));

    const auto cached = contacts.listContacts("a1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::contacts::ContactSummary>>(cached));
    REQUIRE(std::get<std::vector<javelin::jmap::contacts::ContactSummary>>(cached).size() == 1);
    CHECK(std::get<std::vector<javelin::jmap::contacts::ContactSummary>>(cached).front().id ==
          "c3");
    const auto books = contacts.listAddressBooks("a1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::api::AddressBook>>(books));
    CHECK(std::get<std::vector<javelin::jmap::api::AddressBook>>(books).front().name == "Renamed");
    javelin::jmap::cache::SyncStateRepository states{connection};
    const auto state =
        states.find({.accountId = "a1", .objectType = "ContactCard", .queryKey = {}});
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::SyncStateRecord>>(state));
    REQUIRE(std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(state).has_value());
    CHECK(std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(state)->stateToken ==
          "c2");
}

TEST_CASE("contact service falls back to a full fetch when changes cannot be calculated",
          "[jmap][contacts][service][sync]")
{
    ensureApplication();
    QTemporaryDir directory;
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("contact-service-fallback-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    REQUIRE_FALSE(sessions.replace("a1", session()).has_value());
    javelin::jmap::cache::ContactRepository contacts{connection};
    REQUIRE_FALSE(contacts
                      .replaceAll("a1", {addressBook()},
                                  {cachedContact("old", "Old", "old@example.test")}, "b1", "c1")
                      .has_value());

    FakeTransport transport;
    transport.results = {
        javelin::jmap::api::HttpResponse{
            .statusCode = 200,
            .body =
                QByteArray{
                    R"({"methodResponses":[["AddressBook/get",{"accountId":"a1","state":"b2","list":[{"id":"book-1","name":"Personal","description":null,"sortOrder":0,"isDefault":true,"isSubscribed":true,"shareWith":null,"myRights":{"mayRead":true,"mayWrite":true,"mayShare":false,"mayDelete":true}}],"notFound":[]},"contacts-method"]],"sessionState":"s2"})"}},
        javelin::jmap::api::HttpResponse{
            .statusCode = 200,
            .body =
                QByteArray{
                    R"({"methodResponses":[["error",{"type":"cannotCalculateChanges","description":"state expired"},"contacts-method"]],"sessionState":"s2"})"}},
        javelin::jmap::api::HttpResponse{
            .statusCode = 200,
            .body =
                QByteArray{
                    R"({"methodResponses":[["AddressBook/get",{"accountId":"a1","state":"b3","list":[{"id":"book-1","name":"Personal","description":null,"sortOrder":0,"isDefault":true,"isSubscribed":true,"shareWith":null,"myRights":{"mayRead":true,"mayWrite":true,"mayShare":false,"mayDelete":true}}],"notFound":[]},"address-books"],["ContactCard/get",{"accountId":"a1","state":"c9","list":[{"id":"fresh","uid":"uid-fresh","kind":"individual","addressBookIds":{"book-1":true},"name":{"full":"Fresh"},"emails":{"e1":{"address":"fresh@example.test"}}}],"notFound":[]},"contact-cards"]],"sessionState":"s2"})"}},
    };
    javelin::jmap::api::HttpJmapMethodTransport methodTransport{transport};
    javelin::jmap::contacts::ContactService service{connection, contacts, transport,
                                                    methodTransport};
    const auto result =
        QCoro::waitFor(service.refreshAll({.sessionUrl = "https://example.test/.well-known/jmap",
                                           .loginEmail = "alice@example.test",
                                           .apiKey = "secret"},
                                          "a1"));
    REQUIRE(std::holds_alternative<javelin::jmap::contacts::ContactRefreshSummary>(result));
    REQUIRE(transport.requests.size() == 3);
    CHECK(transport.requests[1].body.contains("ContactCard/changes"));
    CHECK(transport.requests[2].body.contains("ContactCard/get"));
    const auto cached = contacts.listContacts("a1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::contacts::ContactSummary>>(cached));
    REQUIRE(std::get<std::vector<javelin::jmap::contacts::ContactSummary>>(cached).size() == 1);
    CHECK(std::get<std::vector<javelin::jmap::contacts::ContactSummary>>(cached).front().id ==
          "fresh");
}

TEST_CASE("contact service downloads contact media through the session template",
          "[jmap][contacts][service][media]")
{
    ensureApplication();
    QTemporaryDir directory;
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("contact-service-media-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    REQUIRE_FALSE(sessions.replace("a1", session()).has_value());
    javelin::jmap::cache::ContactRepository contacts{connection};
    FakeTransport transport;
    transport.results = {javelin::jmap::api::HttpResponse{
        .statusCode = 200, .body = QByteArray::fromHex("89504e470d0a1a0a")}};
    javelin::jmap::api::HttpJmapMethodTransport methodTransport{transport};
    javelin::jmap::contacts::ContactService service{connection, contacts, transport,
                                                    methodTransport};
    const auto result =
        QCoro::waitFor(service.downloadMedia({.sessionUrl = "https://example.test/.well-known/jmap",
                                              .loginEmail = "alice@example.test",
                                              .apiKey = "secret"},
                                             "a1", "a1", "blob with spaces", "image/png"));
    REQUIRE(std::holds_alternative<javelin::jmap::contacts::DownloadedContactMedia>(result));
    CHECK(std::get<javelin::jmap::contacts::DownloadedContactMedia>(result).data ==
          QByteArray::fromHex("89504e470d0a1a0a"));
    REQUIRE(transport.requests.size() == 1);
    CHECK(transport.requests.front().method == javelin::jmap::api::HttpMethod::Get);
    CHECK(transport.requests.front().url.toEncoded().contains("blob%20with%20spaces"));
    REQUIRE(transport.requests.front().headers.size() == 1);
    CHECK(transport.requests.front().headers.front().value == QByteArray{"Bearer secret"});
}
