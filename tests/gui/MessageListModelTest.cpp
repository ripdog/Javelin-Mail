#include "gui/messages/MessageListModel.h"
#include "jmap/cache/ThreadRepository.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QLocale>
#include <QSqlQuery>
#include <QString>
#include <QTemporaryDir>
#include <QThread>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace
{
    class ApplicationGuard
    {
      public:
        ApplicationGuard()
        {
            if (QCoreApplication::instance() != nullptr)
                return;
            static int argc = 1;
            static char applicationName[] = "javelin-message-list-model-tests";
            static char* argv[] = {applicationName, nullptr};
            m_application = std::make_unique<QCoreApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };

    struct TestDatabase
    {
        QTemporaryDir directory;
        javelin::jmap::cache::DatabaseConnection connection;
        QString queries;

        TestDatabase(QTemporaryDir temporaryDirectory,
                     javelin::jmap::cache::DatabaseConnection databaseConnection)
            : directory(std::move(temporaryDirectory)), connection(std::move(databaseConnection)),
              queries(connection.database().databaseName())
        {
        }
    };

    [[nodiscard]] TestDatabase makeTestDatabase()
    {
        static int connectionCounter = 0;
        QTemporaryDir directory;
        REQUIRE(directory.isValid());
        auto opened = javelin::jmap::cache::DatabaseConnection::open({
            .connectionName = QStringLiteral("message-list-model-%1").arg(++connectionCounter),
            .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
        });
        REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
        return {std::move(directory),
                std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened))};
    }

    void seedThreadContext(javelin::jmap::cache::DatabaseConnection& connection)
    {
        QSqlQuery query{connection.database()};
        REQUIRE(query.exec(QStringLiteral(
            "INSERT INTO accounts(account_id,email_address,session_url,is_primary) VALUES("
            "'account-1','alice@example.com','https://example.com/jmap',1)")));
        REQUIRE(query.exec(QStringLiteral(
            "INSERT INTO mailboxes(account_id,mailbox_id,name,sort_order) VALUES("
            "'account-1','mailbox-1','Inbox',0),('account-1','mailbox-archive','Archive',1)")));
        REQUIRE(query.exec(QStringLiteral(
            "INSERT INTO emails(account_id,email_id,thread_id,subject,received_at) VALUES("
            "'account-1','email-1','thread-1','First','2026-08-10T10:00:00Z')")));
        REQUIRE(query.exec(
            QStringLiteral("INSERT INTO email_mailboxes(account_id,email_id,mailbox_id) VALUES("
                           "'account-1','email-1','mailbox-1')")));
        javelin::jmap::cache::ThreadRepository threads{connection};
        REQUIRE_FALSE(threads
                          .upsertMany("account-1",
                                      {{.id = "thread-1", .emailIds = {"email-1", "email-2"}}},
                                      "thread-state-1")
                          .has_value());
    }

    void seedSecondThreadEmail(javelin::jmap::cache::DatabaseConnection& connection,
                               const bool inMailbox)
    {
        QSqlQuery query{connection.database()};
        REQUIRE(query.exec(QStringLiteral(
            "INSERT INTO emails(account_id,email_id,thread_id,subject,received_at) VALUES("
            "'account-1','email-2','thread-1','Second','2026-08-10T11:00:00Z')")));
        REQUIRE(query.exec(
            QStringLiteral("INSERT INTO email_mailboxes(account_id,email_id,mailbox_id) VALUES("
                           "'account-1','email-2','%1')")
                .arg(inMailbox ? QStringLiteral("mailbox-1") : QStringLiteral("mailbox-archive"))));
    }

    void seedTag(javelin::jmap::cache::DatabaseConnection& connection, const QString& emailId)
    {
        QSqlQuery query{connection.database()};
        REQUIRE(query.exec(QStringLiteral(
            "INSERT OR IGNORE INTO mail_tag_definitions(account_id,keyword,display_name,color,"
            "sort_order) VALUES('account-1','work','Work Items','#123456',0)")));
        query.prepare(QStringLiteral("INSERT INTO email_keywords(account_id,email_id,keyword) "
                                     "VALUES('account-1',:email_id,'work')"));
        query.bindValue(QStringLiteral(":email_id"), emailId);
        REQUIRE(query.exec());
    }

    [[nodiscard]] bool waitUntil(const std::function<bool()>& predicate)
    {
        QElapsedTimer timer;
        timer.start();
        while (!predicate() && timer.elapsed() < 2000)
        {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
        QCoreApplication::processEvents();
        return predicate();
    }

    void processEventsFor(const qint64 milliseconds)
    {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < milliseconds)
        {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
        QCoreApplication::processEvents();
    }

    [[nodiscard]] javelin::jmap::cache::MessageListItem
    item(std::string emailId, std::string threadId, bool unread = false)
    {
        return {
            .emailId = std::move(emailId),
            .threadId = std::move(threadId),
            .subject = std::nullopt,
            .preview = std::nullopt,
            .receivedAt = {},
            .sentAt = std::nullopt,
            .mailboxThreadMessageCount = 1,
            .globalThreadMessageCount = 1,
            .hasAttachment = false,
            .isUnread = unread,
            .isFlagged = false,
            .isJunk = false,
            .from = std::nullopt,
            .mailboxNames = {},
            .tags = {},
        };
    }
} // namespace

TEST_CASE("message list expansion waits for complete Thread cache coverage",
          "[gui][messages][model][thread-coverage]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeTestDatabase();
    seedThreadContext(database.connection);
    javelin::gui::messages::MessageListModel model{database.queries};
    auto summary = item("email-1", "thread-1");
    summary.mailboxThreadMessageCount.reset();
    summary.globalThreadMessageCount = 2;
    model.setItems("account-1", "mailbox-1", {summary});

    int materializationRequests = 0;
    QObject::connect(
        &model, &javelin::gui::messages::MessageListModel::threadMaterializationRequired, &model,
        [&](const QString& threadId)
        {
            CHECK(threadId == QStringLiteral("thread-1"));
            ++materializationRequests;
        });

    REQUIRE(model.setThreadExpanded("thread-1", true));
    REQUIRE(waitUntil([&] { return materializationRequests == 1; }));
    CHECK(model.rowCount() == 1);
    CHECK(model.isThreadExpanded("thread-1"));

    seedSecondThreadEmail(database.connection, true);
    model.setItems("account-1", "mailbox-1", {summary});
    REQUIRE(waitUntil([&] { return model.rowCount() == 2; }));
    const auto currentMailboxMemberIndex = model.index(1);
    CHECK(
        model.data(currentMailboxMemberIndex, javelin::gui::messages::MessageListModel::EmailIdRole)
            .toString() == QStringLiteral("email-2"));
    CHECK(model
              .data(currentMailboxMemberIndex,
                    javelin::gui::messages::MessageListModel::MailboxNamesRole)
              .toStringList()
              .isEmpty());

    QSqlQuery moveMember{database.connection.database()};
    REQUIRE(moveMember.exec(QStringLiteral(
        "DELETE FROM email_mailboxes WHERE account_id='account-1' AND email_id='email-2'")));
    REQUIRE(moveMember.exec(
        QStringLiteral("INSERT INTO email_mailboxes(account_id,email_id,mailbox_id) VALUES("
                       "'account-1','email-2','mailbox-archive')")));
    model.refreshExpandedThreadMembers();
    REQUIRE(waitUntil(
        [&]
        {
            return model
                       .data(model.index(1),
                             javelin::gui::messages::MessageListModel::MailboxNamesRole)
                       .toStringList() == QStringList{QStringLiteral("Archive")};
        }));
    CHECK(model.rowCount() == 2);
    CHECK(materializationRequests == 1);
}

TEST_CASE("collapsing a pending Thread clears only the presentation intent",
          "[gui][messages][model][thread-coverage]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeTestDatabase();
    seedThreadContext(database.connection);
    javelin::gui::messages::MessageListModel model{database.queries};
    auto summary = item("email-1", "thread-1");
    summary.globalThreadMessageCount = 2;
    model.setItems("account-1", "mailbox-1", {summary});

    int materializationRequests = 0;
    QObject::connect(&model,
                     &javelin::gui::messages::MessageListModel::threadMaterializationRequired,
                     &model, [&](const QString&) { ++materializationRequests; });
    REQUIRE(model.setThreadExpanded("thread-1", true));
    REQUIRE(model.setThreadExpanded("thread-1", false));
    processEventsFor(100);
    REQUIRE(model.rowCount() == 1);
    CHECK_FALSE(model.isThreadExpanded("thread-1"));
    CHECK(materializationRequests == 0);
}

TEST_CASE("Thread expansion includes children outside the represented mailbox",
          "[gui][messages][model][thread-coverage][search]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeTestDatabase();
    seedThreadContext(database.connection);
    seedSecondThreadEmail(database.connection, false);
    auto summary = item("email-1", "thread-1");
    summary.globalThreadMessageCount = 2;

    javelin::gui::messages::MessageListModel mailboxModel{database.queries};
    mailboxModel.setItems("account-1", "mailbox-1", {summary});
    REQUIRE(mailboxModel.setThreadExpanded("thread-1", true));
    REQUIRE(waitUntil([&] { return mailboxModel.rowCount() == 2; }));
    const auto mailboxMemberIndex = mailboxModel.index(1);
    CHECK(
        mailboxModel.data(mailboxMemberIndex, javelin::gui::messages::MessageListModel::EmailIdRole)
            .toString() == QStringLiteral("email-2"));
    CHECK(mailboxModel
              .data(mailboxMemberIndex, javelin::gui::messages::MessageListModel::MailboxNamesRole)
              .toStringList() == QStringList{QStringLiteral("Archive")});
    CHECK(mailboxModel.data(mailboxMemberIndex, Qt::AccessibleTextRole)
              .toString()
              .contains(QStringLiteral("Mailboxes: Archive")));

    javelin::gui::messages::MessageListModel searchModel{database.queries};
    searchModel.setItems("account-1", std::nullopt, {summary});
    REQUIRE(searchModel.setThreadExpanded("thread-1", true));
    REQUIRE(waitUntil([&] { return searchModel.rowCount() == 2; }));
    const auto searchMemberIndex = searchModel.index(1);
    CHECK(searchModel.data(searchMemberIndex, javelin::gui::messages::MessageListModel::EmailIdRole)
              .toString() == QStringLiteral("email-2"));
    CHECK(searchModel
              .data(searchMemberIndex, javelin::gui::messages::MessageListModel::MailboxNamesRole)
              .toStringList() == QStringList{QStringLiteral("Archive")});
}

TEST_CASE("expanded Thread members expose tags and refresh after metadata changes",
          "[gui][messages][model][tags][thread]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeTestDatabase();
    seedThreadContext(database.connection);
    seedSecondThreadEmail(database.connection, false);
    seedTag(database.connection, QStringLiteral("email-2"));

    auto summary = item("email-1", "thread-1");
    summary.globalThreadMessageCount = 2;
    javelin::gui::messages::MessageListModel model{database.queries};
    model.setItems("account-1", "mailbox-1", {summary});
    REQUIRE(model.setThreadExpanded("thread-1", true));
    REQUIRE(waitUntil([&] { return model.rowCount() == 2; }));
    CHECK(model.data(model.index(1), javelin::gui::messages::MessageListModel::TagNamesRole)
              .toStringList() == QStringList{QStringLiteral("Work Items")});
    CHECK(model.data(model.index(1), javelin::gui::messages::MessageListModel::TagColorsRole)
              .toStringList() == QStringList{QStringLiteral("#123456")});

    QSqlQuery removeTag{database.connection.database()};
    REQUIRE(removeTag.exec(QStringLiteral(
        "DELETE FROM email_keywords WHERE account_id='account-1' AND email_id='email-2' "
        "AND keyword='work'")));
    model.refreshExpandedThreadMembers();
    REQUIRE(waitUntil(
        [&]
        {
            return model
                .data(model.index(1), javelin::gui::messages::MessageListModel::TagNamesRole)
                .toStringList()
                .isEmpty();
        }));
}

TEST_CASE("message list model displays a placeholder for missing subjects",
          "[gui][messages][model]")
{
    javelin::gui::messages::MessageListModel model{QString{}};

    auto first = item("email-1", "thread-1");
    auto second = item("email-2", "thread-2");
    second.subject = std::string{};
    model.setItems(std::optional<std::string>{"account-1"}, std::optional<std::string>{"mailbox-1"},
                   {std::move(first), std::move(second)});

    REQUIRE(model.rowCount() == 2);
    CHECK(model.data(model.index(0), javelin::gui::messages::MessageListModel::SubjectRole)
              .toString() == QStringLiteral("<No Subject>"));
    CHECK(model.data(model.index(1), javelin::gui::messages::MessageListModel::SubjectRole)
              .toString() == QStringLiteral("<No Subject>"));
}

TEST_CASE("message list model exposes tags already carried by message rows",
          "[gui][messages][model][tags]")
{
    javelin::gui::messages::MessageListModel model{QString{}};

    auto tagged = item("email-1", "thread-1");
    tagged.tags.push_back(javelin::jmap::cache::MessageListTag{
        .keyword = "work",
        .displayName = QStringLiteral("Work Items"),
        .color = QStringLiteral("#123456"),
    });
    model.setItems(std::optional<std::string>{"account-1"}, std::optional<std::string>{"mailbox-1"},
                   {std::move(tagged)});

    REQUIRE(model.rowCount() == 1);
    CHECK(model.data(model.index(0), javelin::gui::messages::MessageListModel::TagNamesRole)
              .toStringList() == QStringList{QStringLiteral("Work Items")});
    CHECK(model.data(model.index(0), javelin::gui::messages::MessageListModel::TagColorsRole)
              .toStringList() == QStringList{QStringLiteral("#123456")});
}

TEST_CASE("message list model normalizes tooltips and prefers cached body previews",
          "[gui][messages][model][tooltip]")
{
    javelin::gui::messages::MessageListModel model{QString{}};

    auto serverPreview = item("email-1", "thread-1");
    serverPreview.preview = "\n\n      Server   preview\n   text   ";
    auto bodyPreview = item("email-2", "thread-2");
    bodyPreview.preview = "Server preview should not win";
    bodyPreview.bodyPreview = "\n\t  Plaintext    body\npreview  ";
    model.setItems(std::optional<std::string>{"account-1"}, std::optional<std::string>{"mailbox-1"},
                   {std::move(serverPreview), std::move(bodyPreview)});

    REQUIRE(model.rowCount() == 2);
    CHECK(model.data(model.index(0), Qt::ToolTipRole).toString() ==
          QStringLiteral("Server preview text"));
    CHECK(model.data(model.index(1), Qt::ToolTipRole).toString() ==
          QStringLiteral("Plaintext body preview"));
}

TEST_CASE("message list model exposes painted message state to accessibility",
          "[gui][messages][model][accessibility]")
{
    javelin::gui::messages::MessageListModel model{QString{}};

    auto accessible = item("email-1", "thread-1", true);
    accessible.subject = "Quarterly update";
    accessible.preview = "The preview text";
    accessible.receivedAt = "2026-08-10T08:15:00+12:00";
    accessible.mailboxThreadMessageCount = 2;
    accessible.globalThreadMessageCount = 2;
    accessible.hasAttachment = true;
    accessible.isFlagged = true;
    accessible.from =
        javelin::jmap::domain::EmailAddress{.name = "Alice", .email = "alice@example.com"};
    accessible.mailboxNames = {"Inbox", "Projects"};
    accessible.tags.push_back(javelin::jmap::cache::MessageListTag{
        .keyword = "work",
        .displayName = QStringLiteral("Work"),
        .color = QStringLiteral("#123456"),
    });
    model.setItems(std::optional<std::string>{"account-1"}, std::nullopt, {std::move(accessible)});

    const auto index = model.index(0);
    const auto text = model.data(index, Qt::AccessibleTextRole).toString();
    CHECK(text.contains(QStringLiteral("Alice")));
    CHECK(text.contains(QStringLiteral("Quarterly update")));
    const auto received =
        QDateTime::fromString(QStringLiteral("2026-08-10T08:15:00+12:00"), Qt::ISODate);
    REQUIRE(received.isValid());
    CHECK(text.contains(QLocale{}.toString(received.toLocalTime(), QLocale::LongFormat)));
    CHECK(text.contains(QStringLiteral("Unread")));
    CHECK(text.contains(QStringLiteral("Starred")));
    CHECK(text.contains(QStringLiteral("Has attachment")));
    CHECK(text.contains(QStringLiteral("2 messages in this mailbox")));
    CHECK(text.contains(QStringLiteral("Work")));
    CHECK(text.contains(QStringLiteral("Inbox, Projects")));
    CHECK(model.data(index, Qt::AccessibleDescriptionRole).toString().isEmpty());
}

TEST_CASE("message list model expands a known conversation without an exact mailbox count",
          "[gui][messages][model][accessibility][thread-coverage]")
{
    javelin::gui::messages::MessageListModel model{QString{}};

    auto conversation = item("email-1", "thread-1");
    conversation.mailboxThreadMessageCount.reset();
    conversation.globalThreadMessageCount = 3;
    model.setItems(std::optional<std::string>{"account-1"}, std::optional<std::string>{"mailbox-1"},
                   {std::move(conversation)});

    const auto index = model.index(0);
    CHECK(model.data(index, javelin::gui::messages::MessageListModel::CanExpandRole).toBool());
    CHECK_FALSE(model.data(index, javelin::gui::messages::MessageListModel::ThreadMessageCountRole)
                    .isValid());
    CHECK(model.data(index, javelin::gui::messages::MessageListModel::GlobalThreadMessageCountRole)
              .toULongLong() == 3);
    const auto accessibleText = model.data(index, Qt::AccessibleTextRole).toString();
    CHECK(accessibleText.contains(QStringLiteral("Conversation")));
    CHECK_FALSE(accessibleText.contains(QStringLiteral("3 messages")));
}

TEST_CASE("message list model marks one cached row read without resetting its list",
          "[gui][messages][model]")
{
    javelin::gui::messages::MessageListModel model{QString{}};

    model.setItems(std::optional<std::string>{"account-1"}, std::optional<std::string>{"mailbox-1"},
                   {item("email-1", "thread-1", true), item("email-2", "thread-2", true)});

    REQUIRE(model.rowCount() == 2);
    CHECK(model.setEmailRead("email-1"));
    CHECK_FALSE(model.data(model.index(0), javelin::gui::messages::MessageListModel::IsUnreadRole)
                    .toBool());
    CHECK(model.data(model.index(1), javelin::gui::messages::MessageListModel::IsUnreadRole)
              .toBool());
    CHECK_FALSE(model.setEmailRead("email-1"));
}

TEST_CASE("message list model appends an infinite-scroll tail without resetting existing rows",
          "[gui][messages][model][infinite-scroll]")
{
    javelin::gui::messages::MessageListModel model{QString{}};

    model.setItems(std::optional<std::string>{"account-1"}, std::optional<std::string>{"mailbox-1"},
                   {item("email-1", "thread-1"), item("email-2", "thread-2")});

    int resetCount = 0;
    int insertedCount = 0;
    QObject::connect(&model, &QAbstractItemModel::modelReset, &model, [&] { ++resetCount; });
    QObject::connect(&model, &QAbstractItemModel::rowsInserted, &model,
                     [&](const QModelIndex&, const int first, const int last)
                     {
                         CHECK(first == 2);
                         CHECK(last == 3);
                         ++insertedCount;
                     });

    model.setItems(std::optional<std::string>{"account-1"}, std::optional<std::string>{"mailbox-1"},
                   {item("email-1", "thread-1"), item("email-2", "thread-2"),
                    item("email-3", "thread-3"), item("email-4", "thread-4")});

    CHECK(resetCount == 0);
    CHECK(insertedCount == 1);
    CHECK(model.rowCount() == 4);
    CHECK(model.data(model.index(3), javelin::gui::messages::MessageListModel::EmailIdRole)
              .toString() == QStringLiteral("email-4"));
}
