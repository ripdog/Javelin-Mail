#include "jmap/contacts/ContactService.h"

#include "jmap/api/Transport.h"
#include "jmap/cache/ContactRepository.h"
#include "jmap/cache/SessionRepository.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

namespace
{
    class FakeTransport final : public javelin::jmap::api::AbstractTransport
    {
      public:
        javelin::jmap::api::HttpRequest lastRequest;
        std::vector<javelin::jmap::api::TransportResult> results;

        QCoro::Task<javelin::jmap::api::TransportResult>
        send(javelin::jmap::api::HttpRequest request) override
        {
            lastRequest = std::move(request);
            REQUIRE_FALSE(results.empty());
            auto result = std::move(results.front());
            results.erase(results.begin());
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
                                                      .mayCreateAddressBook = true}},
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
    javelin::jmap::contacts::ContactService service{connection, transport};
    const auto result =
        QCoro::waitFor(service.refreshAll({.sessionUrl = "https://example.test/.well-known/jmap",
                                           .loginEmail = "alice@example.test",
                                           .apiKey = "secret"},
        "a1"));
    REQUIRE(std::holds_alternative<javelin::jmap::contacts::ContactRefreshSummary>(result));
    CHECK(std::get<javelin::jmap::contacts::ContactRefreshSummary>(result).contactCount == 1);
    CHECK(transport.lastRequest.body.contains("AddressBook/get"));
    CHECK(transport.lastRequest.body.contains("ContactCard/get"));

    javelin::jmap::cache::ContactRepository contacts{connection};
    const auto found = contacts.findByEmail("joe@example.test");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::contacts::ContactSummary>>(found));
    REQUIRE(std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(found).has_value());
}
