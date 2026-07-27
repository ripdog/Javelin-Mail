#include "app/undo/ContactHistoryExecutor.h"

#include <QCoroTask>

#include <catch2/catch_test_macros.hpp>

#include <unordered_map>

namespace
{
    using namespace javelin::app::undo;
    using namespace javelin::jmap::contacts;

    [[nodiscard]] std::string document(std::string id, std::string uid, std::string name)
    {
        return R"({"id":")" + std::move(id) + R"(","uid":")" + std::move(uid) +
               R"(","kind":"individual","addressBookIds":{"book-1":true},"name":{"full":")" +
               std::move(name) + R"("}})";
    }

    [[nodiscard]] ContactSummary contact(std::string id, std::string uid, std::string name)
    {
        auto json = document(id, uid, name);
        return {
            .accountId = "account-1",
            .id = std::move(id),
            .uid = std::move(uid),
            .kind = "individual",
            .displayName = std::move(name),
            .organization = std::nullopt,
            .emails = {},
            .addressBookIds = {"book-1"},
            .isImportant = false,
            .document = std::move(json),
        };
    }

    class FakeContactHistoryPort final : public ContactHistoryPort
    {
      public:
        std::vector<ContactSummary> contacts;
        std::string state = "state-1";
        int mutations = 0;
        int nextId = 1;

        QCoro::Task<AuthoritativeContactsResult> getAuthoritativeContacts(std::string,
                                                                          std::string) override
        {
            co_return AuthoritativeContacts{.state = state, .contacts = contacts};
        }

        QCoro::Task<ContactMutationResult>
        applyContactCardsFromHistory(std::string, javelin::jmap::api::ContactCardSetRequest request,
                                     CommandOrigin) override
        {
            ++mutations;
            ContactMutationSummary summary{
                .accountId = request.accountId,
                .newState = "state-next",
                .createdId = std::nullopt,
                .createdIds = {},
            };
            for (const auto& id : request.destroy)
                std::erase_if(contacts, [&](const auto& value) { return value.id == id; });
            for (const auto& [id, value] : request.update)
            {
                const auto found = std::ranges::find(contacts, id, &ContactSummary::id);
                REQUIRE(found != contacts.end());
                found->document = value.json;
            }
            for (const auto& [creationId, value] : request.create)
            {
                const auto id = "created-" + std::to_string(nextId++);
                auto parsed = summarizeContact(request.accountId, javelin::jmap::api::ContactCard{
                                                                      .id = id,
                                                                      .uid = {},
                                                                      .kind = {},
                                                                      .document = value.json,
                                                                  });
                REQUIRE(parsed.has_value());
                contacts.push_back(std::move(*parsed));
                summary.createdIds.push_back({.creationId = creationId, .serverId = id});
            }
            co_return summary;
        }
    };

    [[nodiscard]] HistoryEntry entry(std::vector<ContactCardItemHistory> items)
    {
        HistoryEntry value;
        value.entryId = QStringLiteral("contacts-history");
        value.commandKind = QStringLiteral("contact_card");
        value.payload = ContactCardHistory{
            .connectionId = "owner-1",
            .accountId = "account-1",
            .items = std::move(items),
        };
        value.status = HistoryEntryStatus::Ready;
        return value;
    }
} // namespace

TEST_CASE("contact batch undo preflights every card before mutation",
          "[app][undo][contact-executor]")
{
    FakeContactHistoryPort port;
    const auto firstBefore = document("card-1", "uid-1", "First");
    const auto firstAfter = document("card-1", "uid-1", "First changed");
    const auto secondBefore = document("card-2", "uid-2", "Second");
    const auto secondAfter = document("card-2", "uid-2", "Second changed");
    port.contacts = {contact("card-1", "uid-1", "First changed"),
                     contact("card-2", "uid-2", "External")};
    ContactHistoryExecutor executor{port};

    const auto result = QCoro::waitFor(executor.execute(entry({
                                                            {.addressBookId = "book-1",
                                                             .currentCardId = "card-1",
                                                             .uid = "uid-1",
                                                             .beforeDocumentJson = firstBefore,
                                                             .afterDocumentJson = firstAfter},
                                                            {.addressBookId = "book-1",
                                                             .currentCardId = "card-2",
                                                             .uid = "uid-2",
                                                             .beforeDocumentJson = secondBefore,
                                                             .afterDocumentJson = secondAfter},
                                                        }),
                                                        HistoryExecutionDirection::Undo));

    CHECK(result.outcome == HistoryExecutionOutcome::Conflict);
    CHECK(port.mutations == 0);
    CHECK(port.contacts.front().displayName == "First changed");
}

TEST_CASE("contact import undo deletes and redo recreates every card with remapped ids",
          "[app][undo][contact-executor]")
{
    FakeContactHistoryPort port;
    const auto first = document("old-1", "uid-1", "First");
    const auto second = document("old-2", "uid-2", "Second");
    port.contacts = {contact("old-1", "uid-1", "First"), contact("old-2", "uid-2", "Second")};
    ContactHistoryExecutor executor{port};
    auto historyEntry = entry({
        {.addressBookId = "book-1",
         .currentCardId = "old-1",
         .uid = "uid-1",
         .beforeDocumentJson = std::nullopt,
         .afterDocumentJson = first},
        {.addressBookId = "book-1",
         .currentCardId = "old-2",
         .uid = "uid-2",
         .beforeDocumentJson = std::nullopt,
         .afterDocumentJson = second},
    });

    auto undone = QCoro::waitFor(executor.execute(historyEntry, HistoryExecutionDirection::Undo));
    CHECK(undone.outcome == HistoryExecutionOutcome::Success);
    CHECK(port.contacts.empty());

    historyEntry.payload = *undone.updatedPayload;
    auto redone = QCoro::waitFor(executor.execute(historyEntry, HistoryExecutionDirection::Redo));
    CHECK(redone.outcome == HistoryExecutionOutcome::Success);
    REQUIRE(port.contacts.size() == 2);
    const auto& updated = std::get<ContactCardHistory>(*redone.updatedPayload);
    REQUIRE(updated.items.size() == 2);
    CHECK(updated.items[0].currentCardId.has_value());
    CHECK(updated.items[1].currentCardId.has_value());
    CHECK(updated.items[0].currentCardId != updated.items[1].currentCardId);
}

TEST_CASE("contact create undo accepts server-added JSContact metadata",
          "[app][undo][contact-executor]")
{
    FakeContactHistoryPort port;
    const std::string submitted =
        R"({"uid":"uid-1","kind":"individual","addressBookIds":{"book-1":true},"name":{"full":"Created"}})";
    auto current = contact("server-1", "uid-1", "Created");
    current.document =
        R"({"@type":"Card","name":{"full":"Created"},"uid":"uid-1","version":"1.0","kind":"individual","id":"server-1","addressBookIds":{"book-1":true}})";
    port.contacts = {std::move(current)};
    ContactHistoryExecutor executor{port};

    const auto result = QCoro::waitFor(executor.execute(entry({{
                                                            .addressBookId = "book-1",
                                                            .currentCardId = "server-1",
                                                            .uid = "uid-1",
                                                            .beforeDocumentJson = std::nullopt,
                                                            .afterDocumentJson = submitted,
                                                        }}),
                                                        HistoryExecutionDirection::Undo));

    CHECK(result.outcome == HistoryExecutionOutcome::Success);
    CHECK(port.contacts.empty());
    CHECK(port.mutations == 1);
}

TEST_CASE("contact delete undo recreates the complete document", "[app][undo][contact-executor]")
{
    FakeContactHistoryPort port;
    const auto before = document("old-1", "uid-1", "Deleted");
    ContactHistoryExecutor executor{port};

    const auto result = QCoro::waitFor(executor.execute(entry({{
                                                            .addressBookId = "book-1",
                                                            .currentCardId = "old-1",
                                                            .uid = "uid-1",
                                                            .beforeDocumentJson = before,
                                                            .afterDocumentJson = std::nullopt,
                                                        }}),
                                                        HistoryExecutionDirection::Undo));

    CHECK(result.outcome == HistoryExecutionOutcome::Success);
    REQUIRE(port.contacts.size() == 1);
    CHECK(port.contacts.front().uid == "uid-1");
}

TEST_CASE("contact delete undo recreates a server document with default kind",
          "[app][undo][contact-executor]")
{
    FakeContactHistoryPort port;
    const std::string before =
        R"({"@type":"Card","id":"old-1","version":"1.0","uid":"uid-1","updated":"2026-07-27T03:40:33Z","addressBookIds":{"book-1":true},"name":{"full":"Deleted"}})";
    ContactHistoryExecutor executor{port};

    const auto result = QCoro::waitFor(executor.execute(entry({{
                                                            .addressBookId = "book-1",
                                                            .currentCardId = "old-1",
                                                            .uid = "uid-1",
                                                            .beforeDocumentJson = before,
                                                            .afterDocumentJson = std::nullopt,
                                                        }}),
                                                        HistoryExecutionDirection::Undo));

    CHECK(result.outcome == HistoryExecutionOutcome::Success);
    REQUIRE(port.contacts.size() == 1);
    CHECK(port.contacts.front().uid == "uid-1");
    CHECK(port.contacts.front().kind == "individual");
    CHECK(port.contacts.front().document.find(R"("version":"1.0")") != std::string::npos);
    CHECK(port.contacts.front().document.find(R"("updated":"2026-07-27T03:40:33Z")") !=
          std::string::npos);
}
