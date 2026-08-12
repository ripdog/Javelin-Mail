#include "jmap/cache/ContactRepository.h"
#include "jmap/contacts/ContactIdentityLookup.h"
#include "jmap/contacts/ContactMutationJournal.h"

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
    auto secondaryBook = book;
    secondaryBook.id = "book-2";
    secondaryBook.name = "Shared";
    secondaryBook.isDefault = false;
    const std::vector contacts{contact("c1", "Joe Bloggs", "Joe@Example.test"),
                               contact("c2", "Alex Smith", "alex@example.test")};
    REQUIRE_FALSE(
        repository.replaceAll("a1", {book, secondaryBook}, contacts, "b1", "c1").has_value());

    const auto books = repository.listAddressBooks("a1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::api::AddressBook>>(books));
    CHECK(std::get<std::vector<javelin::jmap::api::AddressBook>>(books).front().isDefault);

    const auto filtered = repository.listContacts("a1", "book-1", "Bloggs");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::contacts::ContactSummary>>(filtered));
    REQUIRE(std::get<std::vector<javelin::jmap::contacts::ContactSummary>>(filtered).size() == 1);

    const auto addresses = repository.listEmailAddresses();
    REQUIRE(std::holds_alternative<std::vector<std::string>>(addresses));
    CHECK(std::get<std::vector<std::string>>(addresses) ==
          std::vector<std::string>{"alex@example.test", "Joe@Example.test"});

    const auto found = repository.findByEmail("joe@example.test");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::contacts::ContactSummary>>(found));
    REQUIRE(std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(found).has_value());
    CHECK(std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(found)->displayName ==
          "Joe Bloggs");
    const auto foundById = repository.findContact("a1", "c1");
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::contacts::ContactSummary>>(foundById));
    REQUIRE(
        std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(foundById).has_value());
    CHECK(
        std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(foundById)->displayName ==
        "Joe Bloggs");

    javelin::jmap::contacts::ContactIdentityLookup identities{repository};
    QString changedAccount;
    QObject::connect(&identities,
                     &javelin::jmap::contacts::ContactIdentityLookup::contactDataChanged,
                     [&changedAccount](const QString& accountId) { changedAccount = accountId; });
    REQUIRE_FALSE(
        repository
            .upsertContacts("a1", {contact("c1", "Joseph Bloggs", "Joe@Example.test")}, {}, "c2")
            .has_value());
    CHECK(changedAccount == QStringLiteral("a1"));
    const auto resolved = identities.resolve("a1", "JOE@example.test");
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::contacts::ContactIdentity>>(resolved));
    REQUIRE(
        std::get<std::optional<javelin::jmap::contacts::ContactIdentity>>(resolved).has_value());
    CHECK(
        std::get<std::optional<javelin::jmap::contacts::ContactIdentity>>(resolved)->displayName ==
        "Joseph Bloggs");
    const auto suggestions = identities.suggestions("a1");
    REQUIRE(
        std::holds_alternative<std::vector<javelin::jmap::contacts::ContactIdentity>>(suggestions));
    CHECK(std::get<std::vector<javelin::jmap::contacts::ContactIdentity>>(suggestions).size() == 2);

    auto renamedBook = book;
    renamedBook.name = "Private";
    REQUIRE_FALSE(repository.replaceAddressBooks("a1", {renamedBook}, "b2").has_value());
    const auto reconciledBooks = repository.listAddressBooks("a1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::api::AddressBook>>(reconciledBooks));
    REQUIRE(std::get<std::vector<javelin::jmap::api::AddressBook>>(reconciledBooks).size() == 1);
    CHECK(std::get<std::vector<javelin::jmap::api::AddressBook>>(reconciledBooks).front().name ==
          "Private");
    const auto retainedMembership = repository.listContacts("a1", "book-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::contacts::ContactSummary>>(
        retainedMembership));
    CHECK(
        std::get<std::vector<javelin::jmap::contacts::ContactSummary>>(retainedMembership).size() ==
        2);

    changedAccount.clear();
    auto rollbackResult = javelin::jmap::cache::DatabaseTransaction::begin(
        connection, QStringLiteral("Test Contact rollback"));
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseTransaction>(rollbackResult));
    auto rollback = std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(rollbackResult));
    REQUIRE_FALSE(
        repository
            .upsertContacts(rollback, "a1", {contact("c3", "Rollback", "r@example.test")}, {}, "c3")
            .has_value());
    rollback.rollback();
    CHECK(changedAccount.isEmpty());
    const auto afterRollback = repository.listContacts("a1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::contacts::ContactSummary>>(
        afterRollback));
    CHECK(std::get<std::vector<javelin::jmap::contacts::ContactSummary>>(afterRollback).size() ==
          2);

    auto commitResult = javelin::jmap::cache::DatabaseTransaction::begin(
        connection, QStringLiteral("Test Contact commit"));
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseTransaction>(commitResult));
    auto commit = std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(commitResult));
    REQUIRE_FALSE(
        repository.projectContacts(commit, "a1", {contact("c3", "Committed", "c@example.test")}, {})
            .has_value());
    REQUIRE_FALSE(commit.commit().has_value());
    CHECK(changedAccount.isEmpty());
    repository.notifyChanged("a1");
    CHECK(changedAccount == QStringLiteral("a1"));
    const auto projected = repository.findContact("a1", "c3");
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::contacts::ContactSummary>>(projected));
    REQUIRE(
        std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(projected).has_value());
    CHECK(
        std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(projected)->displayName ==
        "Committed");

    const auto baseResult = repository.findContact("a1", "c1");
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::contacts::ContactSummary>>(baseResult));
    const auto& base = std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(baseResult);
    REQUIRE(base.has_value());
    const auto optimistic = contact("c1", "Optimistic", "optimistic@example.test");
    const javelin::jmap::contacts::ContactMutationRecord updateMutation{
        .mutationId = "contact-update-1",
        .operationGroupId = std::nullopt,
        .accountId = "a1",
        .objectId = "c1",
        .creationId = std::nullopt,
        .kind = javelin::jmap::contacts::ContactMutationKind::Update,
        .status = javelin::jmap::sync::MutationStatus::Pending,
        .requestedDocument = R"({"name/full":"Optimistic"})",
        .baseDocument = base->document,
        .projectedDocument = optimistic.document,
        .baseState = "c2",
        .acceptedState = std::nullopt,
        .errorJson = std::nullopt,
    };
    javelin::jmap::contacts::ContactMutationJournal contactJournal{connection, repository};
    REQUIRE_FALSE(contactJournal.queue({updateMutation}, {optimistic}, {}).has_value());
    const auto pendingMutations = contactJournal.listForContact("a1", "c1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::contacts::ContactMutationRecord>>(
        pendingMutations));
    REQUIRE(std::get<std::vector<javelin::jmap::contacts::ContactMutationRecord>>(pendingMutations)
                .size() == 1);
    CHECK(std::get<std::vector<javelin::jmap::contacts::ContactMutationRecord>>(pendingMutations)
              .front()
              .status == javelin::jmap::sync::MutationStatus::Pending);
    const auto projectedUpdate = repository.findContact("a1", "c1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::contacts::ContactSummary>>(
        projectedUpdate));
    REQUIRE(std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(projectedUpdate)
                .has_value());
    CHECK(std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(projectedUpdate)
              ->displayName == "Optimistic");
    REQUIRE_FALSE(
        contactJournal.restoreRejected({updateMutation}, R"({"type":"forbidden"})").has_value());
    const auto rejectedMutations = contactJournal.listForContact("a1", "c1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::contacts::ContactMutationRecord>>(
        rejectedMutations));
    REQUIRE(std::get<std::vector<javelin::jmap::contacts::ContactMutationRecord>>(rejectedMutations)
                .size() == 1);
    CHECK(std::get<std::vector<javelin::jmap::contacts::ContactMutationRecord>>(rejectedMutations)
              .front()
              .status == javelin::jmap::sync::MutationStatus::Rejected);
    const auto restoredUpdate = repository.findContact("a1", "c1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::contacts::ContactSummary>>(
        restoredUpdate));
    REQUIRE(std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(restoredUpdate)
                .has_value());
    CHECK(std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(restoredUpdate)
              ->displayName == "Joseph Bloggs");

    const auto created = contact("create-1", "New Contact", "new@example.test");
    const javelin::jmap::contacts::ContactMutationRecord createMutation{
        .mutationId = "contact-create-1",
        .operationGroupId = std::nullopt,
        .accountId = "a1",
        .objectId = "create-1",
        .creationId = "create-1",
        .kind = javelin::jmap::contacts::ContactMutationKind::Create,
        .status = javelin::jmap::sync::MutationStatus::Pending,
        .requestedDocument = created.document,
        .baseDocument = std::nullopt,
        .projectedDocument = created.document,
        .baseState = "c2",
        .acceptedState = std::nullopt,
        .errorJson = std::nullopt,
    };
    REQUIRE_FALSE(contactJournal.queue({createMutation}, {created}, {}).has_value());
    REQUIRE_FALSE(contactJournal.restoreRejected({createMutation}).has_value());
    const auto removedCreate = repository.findContact("a1", "create-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::contacts::ContactSummary>>(
        removedCreate));
    CHECK_FALSE(std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(removedCreate)
                    .has_value());

    QSqlQuery rejectProjection{connection.database()};
    REQUIRE(rejectProjection.exec(QStringLiteral(
        "CREATE TRIGGER reject_contact_projection BEFORE INSERT ON contact_cards "
        "WHEN NEW.contact_id='fail-1' BEGIN SELECT RAISE(ABORT,'projection rejected'); END")));
    const auto failedContact = contact("fail-1", "Failed", "failed@example.test");
    auto failedMutation = createMutation;
    failedMutation.mutationId = "contact-create-failed";
    failedMutation.objectId = "fail-1";
    failedMutation.requestedDocument = failedContact.document;
    failedMutation.projectedDocument = failedContact.document;
    REQUIRE(contactJournal.queue({failedMutation}, {failedContact}, {}).has_value());
    const auto rolledBack = contactJournal.listForContact("a1", "fail-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::contacts::ContactMutationRecord>>(
        rolledBack));
    CHECK(
        std::get<std::vector<javelin::jmap::contacts::ContactMutationRecord>>(rolledBack).empty());
}
