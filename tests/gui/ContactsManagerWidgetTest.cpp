#include "gui/contacts/ContactsManagerWidget.h"

#include "gui/settings/GuiSettings.h"
#include "jmap/cache/ContactRepository.h"
#include "jmap/cache/SessionRepository.h"

#include <QAction>
#include <QComboBox>
#include <QCoreApplication>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QToolButton>

#include <catch2/catch_test_macros.hpp>

namespace
{
    [[nodiscard]] javelin::jmap::api::Session session(std::string accountId = "a1",
                                                      std::string accountName = "Personal",
                                                      std::string username = "alice@example.test")
    {
        javelin::jmap::api::Session value;
        value.username = std::move(username);
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
        value.primaryAccounts.contactsAccountId = accountId;
        value.accounts.emplace(
            accountId, javelin::jmap::api::Account{
                           .id = accountId,
                           .name = std::move(accountName),
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

    [[nodiscard]] javelin::jmap::api::AddressBook book(std::string id, std::string name,
                                                       const bool subscribed = true,
                                                       const bool mayDelete = true)
    {
        return {.id = std::move(id),
                .name = std::move(name),
                .description = std::nullopt,
                .sortOrder = 0,
                .isDefault = true,
                .isSubscribed = subscribed,
                .shareWith = std::nullopt,
                .myRights = {
                    .mayRead = true, .mayWrite = true, .mayShare = false, .mayDelete = mayDelete}};
    }

    [[nodiscard]] javelin::jmap::contacts::ContactSummary
    contact(std::string name, std::string email, std::string accountId = "a1",
            std::string id = "card-1", std::string uid = "uid-card-1",
            std::string addressBookId = "book-1")
    {
        return {
            .accountId = std::move(accountId),
            .id = id,
            .uid = uid,
            .kind = "individual",
            .displayName = name,
            .organization = std::nullopt,
            .emails = {{.key = "email-1",
                        .address = email,
                        .label = std::nullopt,
                        .preference = std::nullopt,
                        .contexts = {}}},
            .addressBookIds = {addressBookId},
            .isImportant = false,
            .document =
                QStringLiteral(
                    R"({"id":"%1","uid":"%2","kind":"individual","addressBookIds":{"%3":true},"name":{"full":"%4"},"emails":{"email-1":{"address":"%5"}}})")
                    .arg(QString::fromStdString(id), QString::fromStdString(uid),
                         QString::fromStdString(addressBookId), QString::fromStdString(name),
                         QString::fromStdString(email))
                    .toStdString()};
    }

    [[nodiscard]] javelin::jmap::contacts::ContactSummary
    group(std::string name = "Friends", std::string accountId = "a1", std::string id = "group-1",
          std::string uid = "uid-group-1", std::string addressBookId = "book-1")
    {
        return {
            .accountId = std::move(accountId),
            .id = id,
            .uid = uid,
            .kind = "group",
            .displayName = name,
            .organization = std::nullopt,
            .emails = {},
            .addressBookIds = {addressBookId},
            .isImportant = false,
            .document =
                QStringLiteral(
                    R"({"id":"%1","uid":"%2","kind":"group","addressBookIds":{"%3":true},"name":{"full":"%4"},"members":{"uid-card-1":true}})")
                    .arg(QString::fromStdString(id), QString::fromStdString(uid),
                         QString::fromStdString(addressBookId), QString::fromStdString(name))
                    .toStdString()};
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
        std::string lastOwnerAccountId;
        std::optional<javelin::app::AddressBookCommand> lastAddressBookCommand;
        std::optional<javelin::app::SaveContactCommand> lastSaveContactCommand;
        javelin::jmap::contacts::ContactMutationResult saveContactResult = unused();

        QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        mutateAddressBook(std::string ownerAccountId,
                          javelin::app::AddressBookCommand command) override
        {
            const auto accountId =
                std::visit([](const auto& value) { return value.accountId; }, command);
            lastOwnerAccountId = std::move(ownerAccountId);
            lastAddressBookCommand = std::move(command);
            co_return javelin::jmap::contacts::ContactMutationSummary{
                .accountId = accountId,
                .newState = {},
                .createdId = std::nullopt,
                .createdIds = {},
                .receipt = {.domains = {},
                            .acceptedObjectIds = {},
                            .rejectedObjectIds = {},
                            .affectedCacheViews = {},
                            .incompleteMaterialization = false},
            };
        }
        QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        saveContact(std::string ownerAccountId, javelin::app::SaveContactCommand command) override
        {
            lastOwnerAccountId = std::move(ownerAccountId);
            lastSaveContactCommand = std::move(command);
            co_return saveContactResult;
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
    javelin::gui::contacts::ContactsManagerWidget widget{settings, repository, refresh, commands};
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

TEST_CASE("Fastmail-style rights allow editing and deleting group cards",
          "[gui][contacts][groups][rights]")
{
    QTemporaryDir directory;
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("contacts-manager-fastmail-group-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    if (const auto error = sessions.replace("a1", session("a1", "Stalwart", "user@example.test")))
        FAIL(error->message.toStdString());
    if (const auto error =
            sessions.replace("u74ee43a3", session("u74ee43a3", "Fastmail", "user@fastmail.test")))
        FAIL(error->message.toStdString());
    javelin::jmap::cache::ContactRepository repository{connection};
    REQUIRE_FALSE(
        repository
            .replaceAll("a1", {book("c", "Personal")},
                        {contact("Alice", "alice@example.test", "a1", "card-1", "uid-card-1", "c")},
                        "b1", "c1")
            .has_value());
    REQUIRE_FALSE(repository
                      .replaceAll("u74ee43a3", {book("RBk", "Personal", true, false)},
                                  {group("Friends", "u74ee43a3", "group-1", "uid-group-1", "RBk")},
                                  "b2", "c2")
                      .has_value());

    RefreshPort refresh;
    CommandPort commands;
    commands.saveContactResult = javelin::jmap::contacts::ContactMutationSummary{
        .accountId = "u74ee43a3",
        .newState = "c2",
        .createdId = std::nullopt,
        .createdIds = {},
        .receipt = {},
    };
    javelin::gui::settings::GuiSettings settings{javelin::protocol::SettingsSnapshot{}};
    javelin::gui::contacts::ContactsManagerWidget widget{settings, repository, refresh, commands};
    auto* groups = widget.findChild<QListWidget*>(QStringLiteral("contactsGroupList"));
    auto* contacts = widget.findChild<QListWidget*>(QStringLiteral("contactsContactList"));
    auto* details = widget.findChild<QStackedWidget*>(QStringLiteral("contactsDetailStack"));
    auto* kind = widget.findChild<QComboBox*>(QStringLiteral("contactsKindEdit"));
    auto* organization = widget.findChild<QLineEdit*>(QStringLiteral("contactsOrganizationEdit"));
    auto* title = widget.findChild<QLineEdit*>(QStringLiteral("contactsTitleEdit"));
    auto* birthday = widget.findChild<QLineEdit*>(QStringLiteral("contactsBirthdayEdit"));
    auto* members = widget.findChild<QListWidget*>(QStringLiteral("contactsMembersEdit"));
    auto* groupDetails =
        widget.findChild<QToolButton*>(QStringLiteral("contactsGroupContactDetailsToggle"));
    auto* emails = widget.findChild<QWidget*>(QStringLiteral("contactsEmailsEdit"));
    auto* name = widget.findChild<QLineEdit*>(QStringLiteral("contactsNameEdit"));
    auto* save = widget.findChild<QPushButton*>(QStringLiteral("contactsSaveButton"));
    REQUIRE(groups != nullptr);
    REQUIRE(contacts != nullptr);
    REQUIRE(details != nullptr);
    REQUIRE(kind != nullptr);
    REQUIRE(organization != nullptr);
    REQUIRE(title != nullptr);
    REQUIRE(birthday != nullptr);
    REQUIRE(members != nullptr);
    REQUIRE(groupDetails != nullptr);
    REQUIRE(emails != nullptr);
    REQUIRE(name != nullptr);
    REQUIRE(save != nullptr);

    for (int row = 0; row < groups->count(); ++row)
        if (groups->item(row)->text() == QStringLiteral("Friends"))
            groups->setCurrentRow(row);
    REQUIRE(contacts->count() == 1);
    CHECK(contacts->currentItem()->text() == QStringLiteral("Alice"));
    CHECK(contacts->currentItem()->data(Qt::UserRole + 12).toString() == QStringLiteral("a1"));
    CHECK(widget.canEditGroup());
    CHECK(widget.canDeleteGroup());

    QMenu addressBookMenu;
    widget.populateAddressBookMenu(addressBookMenu);
    QAction* fastmailBookAction = nullptr;
    bool inFastmailSection = false;
    for (auto* action : addressBookMenu.actions())
    {
        if (action->isSeparator())
        {
            inFastmailSection = action->text() == QStringLiteral("Fastmail");
            continue;
        }
        if (inFastmailSection && action->text() == QStringLiteral("Personal"))
        {
            fastmailBookAction = action;
            break;
        }
    }
    REQUIRE(fastmailBookAction != nullptr);
    CHECK(fastmailBookAction->isChecked());
    CHECK_FALSE(fastmailBookAction->isEnabled());
    CHECK_FALSE(fastmailBookAction->toolTip().isEmpty());
    fastmailBookAction->setChecked(false);
    QCoreApplication::processEvents();
    CHECK_FALSE(commands.lastAddressBookCommand.has_value());

    widget.beginEditGroup();
    CHECK(details->currentIndex() == 3);
    CHECK(kind->currentData().toString() == QStringLiteral("group"));
    CHECK(organization->isHidden());
    CHECK(title->isHidden());
    CHECK(birthday->isHidden());
    CHECK_FALSE(members->isHidden());
    CHECK_FALSE(groupDetails->isHidden());
    CHECK(emails->isHidden());
    groupDetails->setChecked(true);
    CHECK_FALSE(emails->isHidden());

    name->setText(QStringLiteral("Friends edited"));
    save->click();
    QCoreApplication::processEvents();
    REQUIRE(commands.lastSaveContactCommand.has_value());
    CHECK(commands.lastOwnerAccountId == "u74ee43a3");
    CHECK(commands.lastSaveContactCommand->accountId == "u74ee43a3");
    CHECK(commands.lastSaveContactCommand->contactId == std::optional<std::string>{"group-1"});
    CHECK(commands.lastSaveContactCommand->contact.kind == "group");
    CHECK(commands.lastSaveContactCommand->contact.fullName == "Friends edited");
    CHECK(details->currentIndex() == 1);
}

TEST_CASE("Creating a group uses the full editor and exits it after saving",
          "[gui][contacts][groups][create]")
{
    QTemporaryDir directory;
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("contacts-manager-create-group-test"),
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
                                  {contact("Alice", "a@x.test")}, "b1", "c1")
                      .has_value());

    RefreshPort refresh;
    CommandPort commands;
    commands.saveContactResult = javelin::jmap::contacts::ContactMutationSummary{
        .accountId = "a1",
        .newState = "c2",
        .createdId = std::string{"group-2"},
        .createdIds = {{.creationId = "new-contact", .serverId = "group-2"}},
        .receipt = {},
    };
    javelin::gui::settings::GuiSettings settings{javelin::protocol::SettingsSnapshot{}};
    javelin::gui::contacts::ContactsManagerWidget widget{settings, repository, refresh, commands};
    auto* details = widget.findChild<QStackedWidget*>(QStringLiteral("contactsDetailStack"));
    auto* kind = widget.findChild<QComboBox*>(QStringLiteral("contactsKindEdit"));
    auto* name = widget.findChild<QLineEdit*>(QStringLiteral("contactsNameEdit"));
    auto* save = widget.findChild<QPushButton*>(QStringLiteral("contactsSaveButton"));
    REQUIRE(details != nullptr);
    REQUIRE(kind != nullptr);
    REQUIRE(name != nullptr);
    REQUIRE(save != nullptr);

    widget.beginCreateGroup();
    CHECK(details->currentIndex() == 3);
    CHECK(kind->currentData().toString() == QStringLiteral("group"));
    name->setText(QStringLiteral("Project Team"));
    save->click();
    QCoreApplication::processEvents();

    REQUIRE(commands.lastSaveContactCommand.has_value());
    CHECK_FALSE(commands.lastSaveContactCommand->contactId.has_value());
    CHECK(commands.lastSaveContactCommand->contact.kind == "group");
    CHECK(commands.lastSaveContactCommand->contact.fullName == "Project Team");
    CHECK(details->currentIndex() == 1);
}

TEST_CASE("Saving a contact exits the editor after both edits and creates", "[gui][contacts][save]")
{
    QTemporaryDir directory;
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("contacts-manager-save-view-test"),
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
                                  {contact("Alice", "a@x.test")}, "b1", "c1")
                      .has_value());

    RefreshPort refresh;
    CommandPort commands;
    commands.saveContactResult = javelin::jmap::contacts::ContactMutationSummary{
        .accountId = "a1",
        .newState = "c2",
        .createdId = std::nullopt,
        .createdIds = {},
        .receipt = {},
    };
    javelin::gui::settings::GuiSettings settings{javelin::protocol::SettingsSnapshot{}};
    javelin::gui::contacts::ContactsManagerWidget widget{settings, repository, refresh, commands};
    auto* contacts = widget.findChild<QListWidget*>(QStringLiteral("contactsContactList"));
    auto* details = widget.findChild<QStackedWidget*>(QStringLiteral("contactsDetailStack"));
    auto* name = widget.findChild<QLineEdit*>(QStringLiteral("contactsNameEdit"));
    auto* save = widget.findChild<QPushButton*>(QStringLiteral("contactsSaveButton"));
    REQUIRE(contacts != nullptr);
    REQUIRE(details != nullptr);
    REQUIRE(name != nullptr);
    REQUIRE(save != nullptr);
    REQUIRE(contacts->count() == 1);

    contacts->setCurrentRow(0);
    widget.beginEditContact();
    REQUIRE(details->currentIndex() == 3);
    name->setText(QStringLiteral("Alice edited"));
    save->click();
    QCoreApplication::processEvents();
    REQUIRE(commands.lastSaveContactCommand.has_value());
    CHECK(commands.lastSaveContactCommand->contactId == std::optional<std::string>{"card-1"});
    CHECK(commands.lastSaveContactCommand->contact.fullName == "Alice edited");
    CHECK(details->currentIndex() == 1);

    commands.lastSaveContactCommand.reset();
    commands.saveContactResult = javelin::jmap::contacts::ContactMutationSummary{
        .accountId = "a1",
        .newState = "c3",
        .createdId = std::string{"card-2"},
        .createdIds = {{.creationId = "new-contact", .serverId = "card-2"}},
        .receipt = {},
    };
    widget.beginCreateContact();
    REQUIRE(details->currentIndex() == 3);
    name->setText(QStringLiteral("Bob"));
    save->click();
    QCoreApplication::processEvents();
    REQUIRE(commands.lastSaveContactCommand.has_value());
    CHECK_FALSE(commands.lastSaveContactCommand->contactId.has_value());
    CHECK(commands.lastSaveContactCommand->contact.fullName == "Bob");
    CHECK(details->currentIndex() == 1);
}

TEST_CASE("Contacts address book subscriptions drive the group list across servers",
          "[gui][contacts][subscriptions]")
{
    QTemporaryDir directory;
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("contacts-manager-subscription-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    if (const auto error = sessions.replace("a1", session("a1", "Personal", "alice@example.test")))
        FAIL(error->message.toStdString());
    if (const auto error = sessions.replace("a2", session("a2", "Work", "alice@work.test")))
        FAIL(error->message.toStdString());

    javelin::jmap::cache::ContactRepository repository{connection};
    REQUIRE_FALSE(
        repository
            .replaceAll("a1",
                        {book("personal-book", "Personal Book"),
                         book("hidden-personal-book", "Hidden Personal Book", false)},
                        {contact("Alice", "alice@example.test", "a1", "alice", "uid-alice",
                                 "personal-book"),
                         contact("Hidden Person", "hidden@example.test", "a1", "hidden-person",
                                 "uid-hidden", "hidden-personal-book"),
                         group("Friends", "a1", "friends", "uid-friends", "personal-book"),
                         group("Hidden Friends", "a1", "hidden-friends", "uid-hidden-friends",
                               "hidden-personal-book")},
                        "b1", "c1")
            .has_value());
    REQUIRE_FALSE(repository
                      .replaceAll("a2", {book("work-book", "Work Book", false)},
                                  {contact("Work Person", "person@work.test", "a2", "work-person",
                                           "uid-work-person", "work-book"),
                                   group("Work Friends", "a2", "work-friends", "uid-work-friends",
                                         "work-book")},
                                  "b2", "c2")
                      .has_value());

    RefreshPort refresh;
    CommandPort commands;
    javelin::gui::settings::GuiSettings settings{javelin::protocol::SettingsSnapshot{}};
    javelin::gui::contacts::ContactsManagerWidget widget{settings, repository, refresh, commands};
    auto* books = widget.findChild<QComboBox*>(QStringLiteral("contactsAddressBookCombo"));
    auto* groups = widget.findChild<QListWidget*>(QStringLiteral("contactsGroupList"));
    auto* contacts = widget.findChild<QListWidget*>(QStringLiteral("contactsContactList"));
    REQUIRE(books != nullptr);
    REQUIRE(groups != nullptr);
    REQUIRE(contacts != nullptr);

    const auto findGroupRow = [groups](const QString& text)
    {
        for (int row = 0; row < groups->count(); ++row)
            if (groups->item(row)->text() == text)
                return row;
        return -1;
    };
    CHECK(books->findText(QStringLiteral("Personal Book (default)")) >= 0);
    CHECK(books->findText(QStringLiteral("Hidden Personal Book")) == -1);
    CHECK(findGroupRow(QStringLiteral("Personal Book")) >= 0);
    CHECK(findGroupRow(QStringLiteral("Friends")) >= 0);
    CHECK(findGroupRow(QStringLiteral("Hidden Personal Book")) == -1);
    CHECK(findGroupRow(QStringLiteral("Hidden Friends")) == -1);
    CHECK(findGroupRow(QStringLiteral("Work Book")) == -1);
    CHECK(findGroupRow(QStringLiteral("Work Friends")) == -1);

    const int workHeader = findGroupRow(QStringLiteral("Work"));
    REQUIRE(workHeader >= 0);
    REQUIRE(workHeader + 1 < groups->count());
    CHECK(groups->item(workHeader + 1)->text() == QStringLiteral("No subscribed address books"));
    CHECK(groups->item(workHeader + 1)->flags() == Qt::NoItemFlags);
    CHECK(workHeader + 2 == groups->count());
    REQUIRE(contacts->count() == 1);
    CHECK(contacts->item(0)->text() == QStringLiteral("Alice"));

    QMenu menu;
    widget.populateAddressBookMenu(menu);
    const auto findAction = [&menu](const QString& text) -> QAction*
    {
        for (auto* action : menu.actions())
            if (action->text() == text)
                return action;
        return nullptr;
    };
    auto* personalAction = findAction(QStringLiteral("Personal Book"));
    auto* hiddenPersonalAction = findAction(QStringLiteral("Hidden Personal Book"));
    auto* workAction = findAction(QStringLiteral("Work Book"));
    REQUIRE(personalAction != nullptr);
    REQUIRE(hiddenPersonalAction != nullptr);
    REQUIRE(workAction != nullptr);
    CHECK(personalAction->isChecked());
    CHECK_FALSE(hiddenPersonalAction->isChecked());
    CHECK_FALSE(workAction->isChecked());

    workAction->trigger();
    REQUIRE(commands.lastAddressBookCommand.has_value());
    CHECK(commands.lastOwnerAccountId == "a2");
    const auto* update =
        std::get_if<javelin::app::UpdateAddressBookCommand>(&*commands.lastAddressBookCommand);
    REQUIRE(update != nullptr);
    CHECK(update->accountId == "a2");
    CHECK(update->addressBook.id == "work-book");
    CHECK(update->addressBook.isSubscribed);
}
