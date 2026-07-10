#include "jmap/cache/ContactRepository.h"

#include <QCoreApplication>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <memory>

namespace
{
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

    [[nodiscard]] javelin::jmap::contacts::ContactSummary contact(std::string id, std::string name,
                                                                  std::string email)
    {
        return {
            .accountId = "a1",
            .id = id,
            .uid = "uid-" + id,
            .kind = "individual",
            .displayName = name,
            .organization = "Example Ltd",
            .emails = {{.key = "email-1",
                        .address = email,
                        .label = "work",
                        .preference = 1,
                        .contexts = {{"work", true}}}},
            .addressBookIds = {"book-1"},
            .document =
                R"({"id":")" + id + R"(","uid":"uid-)" + id +
                R"(","kind":"individual","addressBookIds":{"book-1":true},"name":{"full":")" +
                name + R"("},"emails":{"email-1":{"address":")" + email +
                R"(","label":"work","pref":1,"contexts":{"work":true}}},"organizations":{"org-1":{"name":"Example Ltd"}}})"};
    }
} // namespace

TEST_CASE("contact repository greedily caches, filters, and resolves email addresses",
          "[jmap][contacts][cache]")
{
    ensureApplication();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("contacts-repository-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    QSqlQuery account{connection.database()};
    REQUIRE(account.exec(
        QStringLiteral("INSERT INTO accounts(account_id,email_address,session_url,is_primary) "
                       "VALUES('a1','alice@example.test','https://example.test',1)")));

    javelin::jmap::cache::ContactRepository repository{connection};
    const javelin::jmap::api::AddressBook book{
        .id = "book-1",
        .name = "Personal",
        .description = std::nullopt,
        .sortOrder = 0,
        .isDefault = true,
        .isSubscribed = true,
        .shareWith = std::nullopt,
        .myRights = {.mayRead = true, .mayWrite = true, .mayShare = false, .mayDelete = true},
    };
    const std::vector contacts{contact("c1", "Joe Bloggs", "Joe@Example.test"),
                               contact("c2", "Alex Smith", "alex@example.test")};
    REQUIRE_FALSE(repository.replaceAll("a1", {book}, contacts, "b1", "c1").has_value());

    const auto books = repository.listAddressBooks("a1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::api::AddressBook>>(books));
    CHECK(std::get<std::vector<javelin::jmap::api::AddressBook>>(books).front().isDefault);

    const auto filtered = repository.listContacts("a1", "book-1", "Bloggs");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::contacts::ContactSummary>>(filtered));
    REQUIRE(std::get<std::vector<javelin::jmap::contacts::ContactSummary>>(filtered).size() == 1);

    const auto found = repository.findByEmail("joe@example.test");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::contacts::ContactSummary>>(found));
    REQUIRE(std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(found).has_value());
    CHECK(std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(found)->displayName ==
          "Joe Bloggs");
}
