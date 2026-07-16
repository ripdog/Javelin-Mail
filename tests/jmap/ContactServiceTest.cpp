#include "jmap/contacts/ContactService.h"

#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/Transport.h"
#include "jmap/cache/ContactRepository.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/cache/SyncStateRepository.h"
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
    cachedContact(std::string id, std::string name, std::string email)
    {
        return {.accountId = "a1",
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
                R"({"methodResponses":[["ContactCard/set",{"accountId":"a1","oldState":"c1","newState":"c2","created":{},"updated":{"card-1":{"updated":"2026-07-17T00:00:00Z"}},"destroyed":[],"notCreated":{},"notUpdated":{},"notDestroyed":{}},"contacts-set"]],"sessionState":"s2"})"},
    });
    javelin::jmap::api::HttpJmapMethodTransport methodTransport{transport};
    javelin::jmap::cache::ContactRepository contacts{connection};
    javelin::jmap::contacts::ContactService service{connection, contacts, transport,
                                                    methodTransport};
    javelin::jmap::api::ContactCardSetRequest request{
        .accountId = "a1",
        .ifInState = "c1",
        .create = {},
        .update = {{"card-1", {.json = R"({"name":{"full":"Updated"}})"}}},
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
