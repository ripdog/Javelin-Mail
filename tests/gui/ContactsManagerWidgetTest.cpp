#include "gui/contacts/ContactsManagerWidget.h"

#include "gui/settings/GuiSettings.h"
#include "jmap/cache/ContactRepository.h"
#include "jmap/cache/SessionRepository.h"

#include <QComboBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

namespace
{
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
            .maxObjectsInGet = 100,
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

    [[nodiscard]] javelin::jmap::api::AddressBook book(std::string id, std::string name)
    {
        return {
            .id = std::move(id),
            .name = std::move(name),
            .description = std::nullopt,
            .sortOrder = 0,
            .isDefault = true,
            .isSubscribed = true,
            .shareWith = std::nullopt,
            .myRights = {.mayRead = true, .mayWrite = true, .mayShare = false, .mayDelete = true}};
    }

    [[nodiscard]] javelin::jmap::contacts::ContactSummary contact(std::string name,
                                                                  std::string email)
    {
        return {
            .accountId = "a1",
            .id = "card-1",
            .uid = "uid-card-1",
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
                R"({"id":"card-1","uid":"uid-card-1","kind":"individual","addressBookIds":{"book-1":true},"name":{"full":")" +
                name + R"("},"emails":{"email-1":{"address":")" + email + R"("}}})"};
    }

    [[nodiscard]] javelin::jmap::contacts::ContactSummary group()
    {
        return {
            .accountId = "a1",
            .id = "group-1",
            .uid = "uid-group-1",
            .kind = "group",
            .displayName = "Friends",
            .organization = std::nullopt,
            .emails = {},
            .addressBookIds = {"book-1"},
            .isImportant = false,
            .document =
                R"({"id":"group-1","uid":"uid-group-1","kind":"group","addressBookIds":{"book-1":true},"name":{"full":"Friends"},"members":{"uid-card-1":true}})"};
    }

    class RefreshPort final : public javelin::app::ContactRefreshPort
    {
      public:
        QCoro::Task<javelin::jmap::contacts::ContactRefreshResult>
        requestContacts(std::string) override
        {
            co_return javelin::jmap::contacts::ContactRefreshSummary{
                .accountCount = 1,
                .addressBookCount = 2,
                .contactCount = 2,
            };
        }
    };

    class CommandPort final : public javelin::app::ContactCommandPort
    {
        [[nodiscard]] static javelin::jmap::contacts::ContactMutationResult unused()
        {
            return javelin::jmap::OperationError{.message = QStringLiteral("Unused")};
        }

      public:
        QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        mutateAddressBook(std::string, javelin::app::AddressBookCommand) override
        {
            co_return unused();
        }
        QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        saveContact(std::string, javelin::app::SaveContactCommand) override
        {
            co_return unused();
        }
        QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        setContactsStarred(std::string, javelin::app::SetContactsStarredCommand) override
        {
            co_return unused();
        }
        QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        deleteContacts(std::string, javelin::app::DeleteContactsCommand) override
        {
            co_return unused();
        }
        QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        createContactGroup(std::string, javelin::app::CreateContactGroupCommand) override
        {
            co_return unused();
        }
        QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        deleteContactGroup(std::string, javelin::app::DeleteContactGroupCommand) override
        {
            co_return unused();
        }
        QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        setContactGroupMembership(std::string,
                                  javelin::app::SetContactGroupMembershipCommand) override
        {
            co_return unused();
        }
        QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        copyContact(std::string, javelin::app::CopyContactCommand) override
        {
            co_return unused();
        }
        QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        importContacts(std::string, javelin::app::ImportContactsCommand) override
        {
            co_return unused();
        }
        QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        mergeContacts(std::string, javelin::app::MergeContactsCommand) override
        {
            co_return unused();
        }
        QCoro::Task<javelin::jmap::contacts::ContactUploadResult>
        uploadContactMedia(std::string, std::string, QByteArray, std::string) override
        {
            co_return javelin::jmap::OperationError{.message = QStringLiteral("Unused")};
        }
        QCoro::Task<javelin::jmap::contacts::ContactDownloadResult>
        downloadContactMedia(std::string, std::string, std::string, std::string) override
        {
            co_return javelin::jmap::OperationError{.message = QStringLiteral("Unused")};
        }
    };
} // namespace

TEST_CASE("Contacts refresh merges rows without disturbing selection or editor drafts",
          "[gui][contacts][refresh]")
{
    QTemporaryDir directory;
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("contacts-manager-refresh-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    if (const auto error = sessions.replace("a1", session()))
        FAIL(error->message.toStdString());
    javelin::jmap::cache::ContactRepository repository{connection};
    REQUIRE_FALSE(repository
                      .replaceAll("a1", {book("book-1", "Personal")},
                                  {contact("Alice", "a@x.test"), group()}, "b1", "c1")
                      .has_value());

    RefreshPort refresh;
    CommandPort commands;
    javelin::gui::settings::GuiSettings settings{javelin::protocol::SettingsSnapshot{}};
    javelin::gui::contacts::ContactsManagerWidget widget{settings, repository, refresh, commands,
                                                         "a1"};
    auto* books = widget.findChild<QComboBox*>(QStringLiteral("contactsAddressBookCombo"));
    auto* groups = widget.findChild<QListWidget*>(QStringLiteral("contactsGroupList"));
    auto* contacts = widget.findChild<QListWidget*>(QStringLiteral("contactsContactList"));
    auto* details = widget.findChild<QStackedWidget*>(QStringLiteral("contactsDetailStack"));
    auto* name = widget.findChild<QLineEdit*>(QStringLiteral("contactsNameEdit"));
    auto* save = widget.findChild<QPushButton*>(QStringLiteral("contactsSaveButton"));
    REQUIRE(books != nullptr);
    REQUIRE(groups != nullptr);
    REQUIRE(contacts != nullptr);
    REQUIRE(details != nullptr);
    REQUIRE(name != nullptr);
    REQUIRE(save != nullptr);

    books->setCurrentIndex(books->findData(QStringLiteral("book-1")));
    for (int row = 0; row < groups->count(); ++row)
        if (groups->item(row)->text() == QStringLiteral("Friends"))
            groups->setCurrentRow(row);
    REQUIRE(contacts->count() == 1);
    contacts->setCurrentRow(0);
    const auto* selectedGroupItem = groups->currentItem();
    const auto* selectedContactItem = contacts->currentItem();
    widget.beginEditContact();
    REQUIRE(details->currentIndex() == 3);
    name->setText(QStringLiteral("Unsaved draft"));

    REQUIRE_FALSE(repository
                      .replaceAll("a1",
                                  {book("book-2", "Collected"), book("book-1", "Personal renamed")},
                                  {contact("Alice on server", "new@x.test"), group()}, "b2", "c2")
                      .has_value());

    CHECK(books->currentData().toString() == QStringLiteral("book-1"));
    CHECK(groups->currentItem() == selectedGroupItem);
    CHECK(groups->currentItem()->text() == QStringLiteral("Friends"));
    CHECK(contacts->currentItem() == selectedContactItem);
    CHECK(contacts->currentItem()->text() == QStringLiteral("Alice on server"));
    CHECK(details->currentIndex() == 3);
    CHECK(name->text() == QStringLiteral("Unsaved draft"));
    CHECK(books->isEnabled());
    CHECK(groups->isEnabled());
    CHECK(contacts->isEnabled());
    CHECK(save->isEnabled());

    widget.requestRefresh();
    CHECK(details->currentIndex() == 3);
    CHECK(name->text() == QStringLiteral("Unsaved draft"));
    CHECK(books->isEnabled());
    CHECK(groups->isEnabled());
    CHECK(contacts->isEnabled());
    CHECK(save->isEnabled());
}
