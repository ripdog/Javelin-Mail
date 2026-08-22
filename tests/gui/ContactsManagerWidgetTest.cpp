#include "gui/contacts/ContactsManagerWidget.h"

#include "gui/settings/GuiSettings.h"
#include "gui/shell/ContactsTabController.h"
#include "jmap/cache/ContactRepository.h"
#include "jmap/cache/SessionRepository.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateEdit>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QPushButton>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QWidget>

#include <catch2/catch_test_macros.hpp>

namespace
{
    class ScopedDefaultLocale final
    {
      public:
        explicit ScopedDefaultLocale(const QLocale locale) : m_previous{}
        {
            QLocale::setDefault(locale);
        }
        ~ScopedDefaultLocale()
        {
            QLocale::setDefault(m_previous);
        }

      private:
        QLocale m_previous;
    };

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
                                                   .submission = std::nullopt,
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
        int requestCount = 0;

        QCoro::Task<javelin::jmap::contacts::ContactRefreshResult>
        requestContacts(std::string) override
        {
            ++requestCount;
            co_return javelin::jmap::contacts::ContactRefreshSummary{
                .accountCount = 1,
                .addressBookCount = 2,
                .contactCount = 2,
                .reconciledMutations = {},
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

TEST_CASE("Contact groups use locale-aware name ordering", "[gui][contacts][groups][sorting]")
{
    const ScopedDefaultLocale locale{QLocale{QLocale::German, QLocale::Germany}};
    QTemporaryDir directory;
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("contacts-group-locale-sort-test"),
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
                                  {group("Zebra", "a1", "group-z", "uid-group-z"),
                                   group("Äpfel", "a1", "group-a", "uid-group-a")},
                                  "b1", "c1")
                      .has_value());

    RefreshPort refresh;
    CommandPort commands;
    javelin::gui::settings::GuiSettings settings{javelin::protocol::SettingsSnapshot{}};
    javelin::gui::contacts::ContactsManagerWidget widget{settings, repository, refresh, commands};
    auto* groups = widget.findChild<QListWidget*>(QStringLiteral("contactsGroupList"));
    REQUIRE(groups != nullptr);

    int applesRow = -1;
    int zebraRow = -1;
    for (int row = 0; row < groups->count(); ++row)
    {
        if (groups->item(row)->text() == QStringLiteral("Äpfel"))
            applesRow = row;
        else if (groups->item(row)->text() == QStringLiteral("Zebra"))
            zebraRow = row;
    }
    REQUIRE(applesRow >= 0);
    REQUIRE(zebraRow >= 0);
    CHECK(applesRow < zebraRow);
}

TEST_CASE("Contacts workspace state distinguishes availability from writable destinations",
          "[gui][contacts][actions][rights]")
{
    QTemporaryDir directory;
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("contacts-workspace-rights-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    if (const auto error = sessions.replace("a1", session()))
        FAIL(error->message.toStdString());
    auto readOnlyBook = book("book-1", "Shared");
    readOnlyBook.myRights.mayWrite = false;
    javelin::jmap::cache::ContactRepository repository{connection};
    REQUIRE_FALSE(repository.replaceAll("a1", {readOnlyBook}, {}, "b1", "c1").has_value());

    RefreshPort refresh;
    CommandPort commands;
    javelin::gui::settings::GuiSettings settings{javelin::protocol::SettingsSnapshot{}};
    QStackedWidget contentStack;
    std::vector<javelin::gui::shell::TabState> tabs;
    javelin::gui::shell::ContactsTabController controller{settings, repository,   refresh,
                                                          commands, contentStack, tabs};

    const auto state = controller.workspaceState();
    CHECK(state.available);
    CHECK_FALSE(state.canCreateContact);
    CHECK_FALSE(state.createAccountId.has_value());

    controller.invokeWorkspace(javelin::gui::shell::ContactsTabCommand::CreateContact);
    CHECK(tabs.empty());

    int stateChanges = 0;
    QObject::connect(&controller,
                     &javelin::gui::shell::ContactsTabController::workspaceStateChanged,
                     &contentStack, [&stateChanges] { ++stateChanges; });
    auto writableBook = readOnlyBook;
    writableBook.myRights.mayWrite = true;
    REQUIRE_FALSE(repository.replaceAll("a1", {writableBook}, {}, "b2", "c2").has_value());

    CHECK(stateChanges == 1);
    CHECK(controller.workspaceState().canCreateContact);
    CHECK(controller.workspaceState().createAccountId == std::optional<std::string>{"a1"});
}

TEST_CASE("workspace contact creation selects a writable account",
          "[gui][contacts][actions][rights]")
{
    QTemporaryDir directory;
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("contacts-workspace-account-selection-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    if (const auto error = sessions.replace("a1", session("a1", "Shared")))
        FAIL(error->message.toStdString());
    if (const auto error = sessions.replace("a2", session("a2", "Writable")))
        FAIL(error->message.toStdString());
    auto readOnlyBook = book("book-1", "Shared");
    readOnlyBook.myRights.mayWrite = false;
    javelin::jmap::cache::ContactRepository repository{connection};
    REQUIRE_FALSE(repository.replaceAll("a1", {readOnlyBook}, {}, "b1", "c1").has_value());
    REQUIRE_FALSE(
        repository.replaceAll("a2", {book("book-2", "Personal")}, {}, "b2", "c2").has_value());

    RefreshPort refresh;
    CommandPort commands;
    javelin::gui::settings::GuiSettings settings{javelin::protocol::SettingsSnapshot{}};
    QStackedWidget contentStack;
    std::vector<javelin::gui::shell::TabState> tabs;
    javelin::gui::shell::ContactsTabController controller{settings, repository,   refresh,
                                                          commands, contentStack, tabs};

    controller.invokeWorkspace(javelin::gui::shell::ContactsTabCommand::CreateContact);

    REQUIRE(tabs.size() == 1);
    auto* widget =
        qobject_cast<javelin::gui::contacts::ContactsManagerWidget*>(contentStack.widget(0));
    REQUIRE(widget != nullptr);
    CHECK(widget->viewState().accountId == "a2");
    auto* details = widget->findChild<QStackedWidget*>(QStringLiteral("contactsDetailStack"));
    REQUIRE(details != nullptr);
    CHECK(details->currentIndex() == 3);
}

TEST_CASE("workspace Contacts refresh does not duplicate materialization refresh",
          "[gui][contacts][actions]")
{
    QTemporaryDir directory;
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("contacts-workspace-refresh-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    if (const auto error = sessions.replace("a1", session()))
        FAIL(error->message.toStdString());
    javelin::jmap::cache::ContactRepository repository{connection};
    REQUIRE_FALSE(
        repository.replaceAll("a1", {book("book-1", "Personal")}, {}, "b1", "c1").has_value());

    RefreshPort refresh;
    CommandPort commands;
    javelin::gui::settings::GuiSettings settings{javelin::protocol::SettingsSnapshot{}};
    QStackedWidget contentStack;
    std::vector<javelin::gui::shell::TabState> tabs;
    javelin::gui::shell::ContactsTabController controller{settings, repository,   refresh,
                                                          commands, contentStack, tabs};

    controller.invokeWorkspace(javelin::gui::shell::ContactsTabCommand::Refresh);

    REQUIRE(tabs.size() == 1);
    CHECK(refresh.requestCount == 1);

    controller.invokeWorkspace(javelin::gui::shell::ContactsTabCommand::Refresh);
    CHECK(refresh.requestCount == 2);
}

TEST_CASE("workspace contact commands materialize Contacts before executing",
          "[gui][contacts][actions]")
{
    QTemporaryDir directory;
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("contacts-workspace-command-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    if (const auto error = sessions.replace("a1", session()))
        FAIL(error->message.toStdString());
    javelin::jmap::cache::ContactRepository repository{connection};
    REQUIRE_FALSE(
        repository.replaceAll("a1", {book("book-1", "Personal")}, {}, "b1", "c1").has_value());

    RefreshPort refresh;
    CommandPort commands;
    javelin::gui::settings::GuiSettings settings{javelin::protocol::SettingsSnapshot{}};
    QStackedWidget contentStack;
    std::vector<javelin::gui::shell::TabState> tabs;
    javelin::gui::shell::ContactsTabController controller{settings, repository,   refresh,
                                                          commands, contentStack, tabs};
    int activatedIndex = -1;
    QObject::connect(&controller, &javelin::gui::shell::ContactsTabController::tabReady,
                     &contentStack, [&activatedIndex](const int index) { activatedIndex = index; });

    controller.invokeWorkspace(javelin::gui::shell::ContactsTabCommand::CreateContact);

    CHECK(activatedIndex == 0);
    REQUIRE(tabs.size() == 1);
    auto* widget =
        qobject_cast<javelin::gui::contacts::ContactsManagerWidget*>(contentStack.widget(0));
    REQUIRE(widget != nullptr);
    auto* details = widget->findChild<QStackedWidget*>(QStringLiteral("contactsDetailStack"));
    REQUIRE(details != nullptr);
    CHECK(details->currentIndex() == 3);
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
    auto* birthday = widget.findChild<QDateEdit*>(QStringLiteral("contactsBirthdayEdit"));
    auto* birthdayEditor = widget.findChild<QWidget*>(QStringLiteral("contactsBirthdayEditor"));
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
    REQUIRE(birthdayEditor != nullptr);
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
    CHECK(birthdayEditor->isHidden());
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

TEST_CASE("Contact editor uses a birthday picker and keeps address books advanced",
          "[gui][contacts][editor]")
{
    QTemporaryDir directory;
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("contacts-manager-birthday-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    if (const auto error = sessions.replace("a1", session()))
        FAIL(error->message.toStdString());
    auto alice = contact("Alice", "a@x.test");
    alice.document =
        R"({"id":"card-1","uid":"uid-card-1","kind":"individual","addressBookIds":{"book-1":true},"name":{"full":"Alice"},"emails":{"email-1":{"address":"a@x.test"}},"anniversaries":{"birthday":{"kind":"birth","date":{"year":1990,"month":4,"day":5}}}})";
    javelin::jmap::cache::ContactRepository repository{connection};
    REQUIRE_FALSE(
        repository.replaceAll("a1", {book("book-1", "Personal")}, {alice}, "b1", "c1").has_value());

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
    auto* birthday = widget.findChild<QDateEdit*>(QStringLiteral("contactsBirthdayEdit"));
    auto* advancedToggle = widget.findChild<QToolButton*>(QStringLiteral("contactsAdvancedToggle"));
    auto* advancedDetails = widget.findChild<QWidget*>(QStringLiteral("contactsAdvancedDetails"));
    auto* addressBooks = widget.findChild<QListWidget*>(QStringLiteral("contactsAddressBooksEdit"));
    auto* save = widget.findChild<QPushButton*>(QStringLiteral("contactsSaveButton"));
    REQUIRE(contacts != nullptr);
    REQUIRE(birthday != nullptr);
    REQUIRE(advancedToggle != nullptr);
    REQUIRE(advancedDetails != nullptr);
    REQUIRE(addressBooks != nullptr);
    REQUIRE(save != nullptr);

    contacts->setCurrentRow(0);
    widget.beginEditContact();
    CHECK(birthday->calendarPopup());
    CHECK(birthday->date() == QDate(1990, 4, 5));
    const auto* birthdayToggle = birthday->parentWidget()->findChild<QCheckBox*>();
    REQUIRE(birthdayToggle != nullptr);
    CHECK(birthdayToggle->checkState() == Qt::Checked);
    CHECK(advancedDetails->isAncestorOf(addressBooks));
    CHECK(advancedDetails->isHidden());
    advancedToggle->click();
    CHECK_FALSE(advancedDetails->isHidden());

    birthday->setDate(QDate(1991, 6, 7));
    save->click();
    QCoreApplication::processEvents();
    REQUIRE(commands.lastSaveContactCommand.has_value());
    CHECK(commands.lastSaveContactCommand->contact.birthday == "1991-06-07");
}

TEST_CASE("Contact field labels and preference order follow the visible rows",
          "[gui][contacts][editor][fields]")
{
    QTemporaryDir directory;
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("contacts-manager-fields-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::jmap::cache::SessionRepository sessions{connection};
    if (const auto error = sessions.replace("a1", session()))
        FAIL(error->message.toStdString());
    auto alice = contact("Alice", "other@example.test");
    alice.document =
        R"({"id":"card-1","uid":"uid-card-1","kind":"individual","addressBookIds":{"book-1":true},"name":{"full":"Alice"},"emails":{"other":{"address":"other@example.test","label":"Assistant","pref":1},"work":{"address":"work@example.test","label":"Office","pref":2,"contexts":{"work":true}}}})";
    javelin::jmap::cache::ContactRepository repository{connection};
    REQUIRE_FALSE(
        repository.replaceAll("a1", {book("book-1", "Personal")}, {alice}, "b1", "c1").has_value());

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
    auto* emails = widget.findChild<QWidget*>(QStringLiteral("contactsEmailsEdit"));
    auto* save = widget.findChild<QPushButton*>(QStringLiteral("contactsSaveButton"));
    REQUIRE(contacts != nullptr);
    REQUIRE(emails != nullptr);
    REQUIRE(save != nullptr);

    contacts->setCurrentRow(0);
    widget.beginEditContact();
    const auto values = emails->findChildren<QLineEdit*>(QStringLiteral("contactFieldValue"));
    REQUIRE(values.size() == 2);
    QWidget* otherRow = nullptr;
    QWidget* workRow = nullptr;
    for (auto* value : values)
    {
        if (value->text() == QStringLiteral("other@example.test"))
            otherRow = value->parentWidget();
        else if (value->text() == QStringLiteral("work@example.test"))
            workRow = value->parentWidget();
    }
    REQUIRE(otherRow != nullptr);
    REQUIRE(workRow != nullptr);
    auto* otherLabel = otherRow->findChild<QLineEdit*>(QStringLiteral("contactFieldLabel"));
    auto* workLabel = workRow->findChild<QLineEdit*>(QStringLiteral("contactFieldLabel"));
    auto* moveOtherDown = otherRow->findChild<QToolButton*>(QStringLiteral("contactFieldMoveDown"));
    REQUIRE(otherLabel != nullptr);
    REQUIRE(workLabel != nullptr);
    REQUIRE(moveOtherDown != nullptr);
    CHECK_FALSE(otherLabel->isHidden());
    CHECK(workLabel->isHidden());

    auto* addField = emails->findChild<QPushButton*>(QStringLiteral("contactFieldAdd"));
    REQUIRE(addField != nullptr);
    addField->click();
    QLineEdit* newValue = nullptr;
    for (auto* value : emails->findChildren<QLineEdit*>(QStringLiteral("contactFieldValue")))
    {
        if (value->text().isEmpty())
        {
            newValue = value;
            break;
        }
    }
    REQUIRE(newValue != nullptr);
    auto* newType =
        newValue->parentWidget()->findChild<QComboBox*>(QStringLiteral("contactFieldType"));
    auto* newLabel =
        newValue->parentWidget()->findChild<QLineEdit*>(QStringLiteral("contactFieldLabel"));
    REQUIRE(newType != nullptr);
    REQUIRE(newLabel != nullptr);
    CHECK_FALSE(newLabel->isHidden());
    newType->setCurrentIndex(newType->findData(QStringLiteral("work")));
    CHECK(newLabel->isHidden());
    newType->setCurrentIndex(newType->findData(QStringLiteral("")));
    CHECK_FALSE(newLabel->isHidden());

    moveOtherDown->click();
    save->click();
    QCoreApplication::processEvents();
    REQUIRE(commands.lastSaveContactCommand.has_value());
    const auto& savedEmails = commands.lastSaveContactCommand->contact.emails;
    REQUIRE(savedEmails.size() == 2);
    CHECK(savedEmails[0].value == "work@example.test");
    CHECK(savedEmails[0].preference == std::optional<std::uint32_t>{1});
    CHECK(savedEmails[0].label == std::optional<std::string>{"Office"});
    CHECK(savedEmails[1].value == "other@example.test");
    CHECK(savedEmails[1].preference == std::optional<std::uint32_t>{2});
    CHECK(savedEmails[1].label == std::optional<std::string>{"Assistant"});
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
                                  {contact("Alice", "a@x.test"), group()}, "b1", "c1")
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
    auto* groups = widget.findChild<QListWidget*>(QStringLiteral("contactsGroupList"));
    auto* contacts = widget.findChild<QListWidget*>(QStringLiteral("contactsContactList"));
    auto* details = widget.findChild<QStackedWidget*>(QStringLiteral("contactsDetailStack"));
    auto* name = widget.findChild<QLineEdit*>(QStringLiteral("contactsNameEdit"));
    auto* save = widget.findChild<QPushButton*>(QStringLiteral("contactsSaveButton"));
    REQUIRE(groups != nullptr);
    REQUIRE(contacts != nullptr);
    REQUIRE(details != nullptr);
    REQUIRE(name != nullptr);
    REQUIRE(save != nullptr);
    REQUIRE(contacts->count() == 1);

    for (int row = 0; row < groups->count(); ++row)
    {
        if (groups->item(row)->text() == QStringLiteral("Friends"))
        {
            groups->setCurrentRow(row);
            break;
        }
    }
    REQUIRE(groups->currentItem() != nullptr);
    REQUIRE(groups->currentItem()->text() == QStringLiteral("Friends"));
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
    REQUIRE(groups->currentItem() != nullptr);
    CHECK(groups->currentItem()->text() == QStringLiteral("Friends"));

    for (int row = 0; row < groups->count(); ++row)
    {
        if (groups->item(row)->text() == QStringLiteral("Personal") &&
            groups->item(row)->flags().testFlag(Qt::ItemIsSelectable))
        {
            groups->setCurrentRow(row);
            break;
        }
    }
    REQUIRE(groups->currentItem() != nullptr);
    REQUIRE(groups->currentItem()->text() == QStringLiteral("Personal"));

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
    CHECK_FALSE(commands.lastSaveContactCommand->contact.uid.empty());
    CHECK(commands.lastSaveContactCommand->contact.fullName == "Bob");
    CHECK(details->currentIndex() == 1);
    REQUIRE(groups->currentItem() != nullptr);
    CHECK(groups->currentItem()->text() == QStringLiteral("Personal"));
    REQUIRE(contacts->count() == 2);
    QListWidgetItem* bob = nullptr;
    for (int row = 0; row < contacts->count(); ++row)
    {
        if (contacts->item(row)->data(Qt::UserRole).toString() == QStringLiteral("card-2"))
        {
            bob = contacts->item(row);
            break;
        }
    }
    REQUIRE(bob != nullptr);
    CHECK(bob->text() == QStringLiteral("Bob"));
    CHECK(contacts->currentItem() == bob);

    widget.requestRefresh();
    QCoreApplication::processEvents();
    REQUIRE(groups->currentItem() != nullptr);
    CHECK(groups->currentItem()->text() == QStringLiteral("Personal"));
    REQUIRE(contacts->count() == 2);
    QListWidgetItem* refreshedBob = nullptr;
    for (int row = 0; row < contacts->count(); ++row)
    {
        if (contacts->item(row)->data(Qt::UserRole).toString() == QStringLiteral("card-2"))
        {
            refreshedBob = contacts->item(row);
            break;
        }
    }
    REQUIRE(refreshedBob != nullptr);
    contacts->setCurrentRow(0);
    contacts->setCurrentItem(refreshedBob, QItemSelectionModel::ClearAndSelect);
    QCoreApplication::processEvents();
    CHECK(contacts->currentItem() == refreshedBob);
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
