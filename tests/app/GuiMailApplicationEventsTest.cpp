#include "client/GuiMailApplicationEvents.h"
#include "client/GuiDaemonSession.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <utility>
#include <vector>

TEST_CASE("GUI mail events retain daemon status published before construction", "[app][gui]")
{
    javelin::app::GuiDaemonSession session{
        {.runtimeDirectory = QStringLiteral("/tmp"),
         .socketPath = QStringLiteral("/tmp/unused-javelin-test.sock"),
         .daemonExecutable = {},
         .protocol = {.major = 3, .minor = 0},
         .build = {.application = QStringLiteral("Javelin-Mail"),
                   .revision = QStringLiteral("test")},
         .startTimeoutMilliseconds = 10,
         .startDaemonIfMissing = false}};

    session.onBoundaryEvent(javelin::protocol::DaemonStatusChanged{
        .status = {.lifecycle = javelin::protocol::DaemonLifecycle::Ready,
                   .accounts = {{.accountId = QStringLiteral("account-1"),
                                 .state = javelin::protocol::AccountState::Ready,
                                 .detail = {}}}}});

    javelin::app::GuiMailApplicationEvents events{session};
    const auto statuses = events.accountStatuses();
    REQUIRE(statuses.contains("account-1"));
    CHECK(statuses.at("account-1") == javelin::app::MailAccountStatus::Connected);

    std::optional<javelin::app::MailAccountStatus> recoveryStatus;
    QObject::connect(
        &events, &javelin::app::MailApplicationEventsPort::accountStatusChanged, &events,
        [&recoveryStatus](const QString& accountId, const javelin::app::MailAccountStatus status)
        {
            if (accountId == QStringLiteral("account-1"))
                recoveryStatus = status;
        });
    Q_EMIT session.recoveryStarted(QStringLiteral("daemon disconnected"));
    REQUIRE(recoveryStatus.has_value());
    CHECK(*recoveryStatus == javelin::app::MailAccountStatus::Disconnected);
    CHECK(events.accountStatuses().empty());
}

TEST_CASE("GUI mail events publish contact cache invalidations", "[app][gui][cache]")
{
    javelin::app::GuiDaemonSession session{
        {.runtimeDirectory = QStringLiteral("/tmp"),
         .socketPath = QStringLiteral("/tmp/unused-javelin-test.sock"),
         .daemonExecutable = {},
         .protocol = {.major = 3, .minor = 0},
         .build = {.application = QStringLiteral("Javelin-Mail"),
                   .revision = QStringLiteral("test")},
         .startTimeoutMilliseconds = 10,
         .startDaemonIfMissing = false}};

    javelin::app::GuiMailApplicationEvents events{session};
    std::optional<javelin::app::MailCacheInvalidation> received;
    QObject::connect(&events, &javelin::app::MailApplicationEventsPort::cacheInvalidated,
                     [&received](javelin::app::MailCacheInvalidation invalidation)
                     { received = std::move(invalidation); });

    session.onBoundaryEvent(javelin::protocol::CacheInvalidation{
        .epoch = {.value = 7},
        .changedDomains = {javelin::protocol::ChangedDomain::Contacts},
        .affectedKeys = {QStringLiteral("contacts-account")},
        .accountId = QStringLiteral("contacts-account"),
    });

    REQUIRE(received.has_value());
    CHECK(received->change.accountId == QStringLiteral("contacts-account"));
    CHECK(received->change.contactsChanged);
    CHECK(received->changedDomains == std::vector{javelin::protocol::ChangedDomain::Contacts});
}

TEST_CASE("GUI mail events preserve equal account and mailbox identifiers", "[app][gui][cache]")
{
    javelin::app::GuiDaemonSession session{
        {.runtimeDirectory = QStringLiteral("/tmp"),
         .socketPath = QStringLiteral("/tmp/unused-javelin-test.sock"),
         .daemonExecutable = {},
         .protocol = {.major = 3, .minor = 0},
         .build = {.application = QStringLiteral("Javelin-Mail"),
                   .revision = QStringLiteral("test")},
         .startTimeoutMilliseconds = 10,
         .startDaemonIfMissing = false}};

    javelin::app::GuiMailApplicationEvents events{session};
    std::optional<javelin::app::MailCacheInvalidation> received;
    QObject::connect(&events, &javelin::app::MailApplicationEventsPort::cacheInvalidated,
                     [&received](javelin::app::MailCacheInvalidation invalidation)
                     { received = std::move(invalidation); });

    session.onBoundaryEvent(javelin::protocol::CacheInvalidation{
        .epoch = {.value = 8},
        .changedDomains = {javelin::protocol::ChangedDomain::MailQueryWindows},
        .affectedKeys = {QStringLiteral("c")},
        .accountId = QStringLiteral("c"),
        .mailboxIds = {QStringLiteral("c")},
        .mailboxWindows =
            {{.mailboxId = QStringLiteral("c"), .offset = 0, .limit = 100, .total = 113}},
    });

    REQUIRE(received.has_value());
    CHECK(received->change.accountId == QStringLiteral("c"));
    CHECK(received->change.mailboxIds == QStringList{QStringLiteral("c")});
    REQUIRE(received->change.queryWindows.size() == 1);
    CHECK(received->change.queryWindows.front().mailboxId == QStringLiteral("c"));
    CHECK(received->change.queryWindows.front().total == std::optional<std::size_t>{113});
}

TEST_CASE("GUI mail events preserve optimistic projection independently of metadata",
          "[app][gui][cache]")
{
    javelin::app::GuiDaemonSession session{
        {.runtimeDirectory = QStringLiteral("/tmp"),
         .socketPath = QStringLiteral("/tmp/unused-javelin-test.sock"),
         .daemonExecutable = {},
         .protocol = {.major = 3, .minor = 0},
         .build = {.application = QStringLiteral("Javelin-Mail"),
                   .revision = QStringLiteral("test")},
         .startTimeoutMilliseconds = 10,
         .startDaemonIfMissing = false}};

    javelin::app::GuiMailApplicationEvents events{session};
    std::optional<javelin::app::MailCacheInvalidation> received;
    QObject::connect(&events, &javelin::app::MailApplicationEventsPort::cacheInvalidated,
                     [&received](javelin::app::MailCacheInvalidation invalidation)
                     { received = std::move(invalidation); });

    session.onBoundaryEvent(javelin::protocol::CacheInvalidation{
        .epoch = {.value = 9},
        .changedDomains = {javelin::protocol::ChangedDomain::MessageMetadata},
        .affectedKeys = {},
        .accountId = QStringLiteral("account-1"),
        .optimisticProjection = false,
    });
    REQUIRE(received.has_value());
    CHECK_FALSE(received->change.emailObjectsChanged);
    CHECK_FALSE(received->change.optimisticProjection);

    session.onBoundaryEvent(javelin::protocol::CacheInvalidation{
        .epoch = {.value = 10},
        .changedDomains = {},
        .affectedKeys = {},
        .accountId = QStringLiteral("account-1"),
        .optimisticProjection = true,
    });
    REQUIRE(received.has_value());
    CHECK_FALSE(received->change.emailObjectsChanged);
    CHECK(received->change.optimisticProjection);
}

TEST_CASE("GUI mail events publish the dedicated mail-tag cache domain", "[app][gui][cache]")
{
    javelin::app::GuiDaemonSession session{
        {.runtimeDirectory = QStringLiteral("/tmp"),
         .socketPath = QStringLiteral("/tmp/unused-javelin-test.sock"),
         .daemonExecutable = {},
         .protocol = {.major = 3, .minor = 0},
         .build = {.application = QStringLiteral("Javelin-Mail"),
                   .revision = QStringLiteral("test")},
         .startTimeoutMilliseconds = 10,
         .startDaemonIfMissing = false}};
    javelin::app::GuiMailApplicationEvents events{session};
    std::optional<javelin::app::MailCacheInvalidation> received;
    QObject::connect(&events, &javelin::app::MailApplicationEventsPort::cacheInvalidated,
                     [&received](javelin::app::MailCacheInvalidation invalidation)
                     { received = std::move(invalidation); });

    session.onBoundaryEvent(javelin::protocol::CacheInvalidation{
        .epoch = {.value = 13},
        .changedDomains = {javelin::protocol::ChangedDomain::MailTags},
        .affectedKeys = {QStringLiteral("account-1")},
        .accountId = QStringLiteral("account-1"),
    });

    REQUIRE(received.has_value());
    CHECK(received->change.mailTagsChanged);
    CHECK_FALSE(received->changedDomains.empty());
    CHECK(received->changedDomains.front() == javelin::protocol::ChangedDomain::MailTags);
}

TEST_CASE("GUI mail events preserve hydrated message content identifiers", "[app][gui][cache]")
{
    javelin::app::GuiDaemonSession session{
        {.runtimeDirectory = QStringLiteral("/tmp"),
         .socketPath = QStringLiteral("/tmp/unused-javelin-test.sock"),
         .daemonExecutable = {},
         .protocol = {.major = 3, .minor = 0},
         .build = {.application = QStringLiteral("Javelin-Mail"),
                   .revision = QStringLiteral("test")},
         .startTimeoutMilliseconds = 10,
         .startDaemonIfMissing = false}};

    javelin::app::GuiMailApplicationEvents events{session};
    std::optional<javelin::app::MailCacheInvalidation> received;
    QObject::connect(&events, &javelin::app::MailApplicationEventsPort::cacheInvalidated,
                     [&received](javelin::app::MailCacheInvalidation invalidation)
                     { received = std::move(invalidation); });

    session.onBoundaryEvent(javelin::protocol::CacheInvalidation{
        .epoch = {.value = 9},
        .changedDomains = {javelin::protocol::ChangedDomain::MessageContent},
        .affectedKeys = {QStringLiteral("account-a"), QStringLiteral("email-a")},
        .accountId = QStringLiteral("account-a"),
        .messageContentEmailIds = {QStringLiteral("email-a")},
    });

    REQUIRE(received.has_value());
    CHECK(received->change.messageContentEmailIds == QStringList{QStringLiteral("email-a")});
    CHECK(received->changedDomains ==
          std::vector{javelin::protocol::ChangedDomain::MessageContent});
}

TEST_CASE("GUI mail events preserve Thread materialization progress", "[app][gui][thread]")
{
    javelin::app::GuiDaemonSession session{
        {.runtimeDirectory = QStringLiteral("/tmp"),
         .socketPath = QStringLiteral("/tmp/unused-javelin-test.sock"),
         .daemonExecutable = {},
         .protocol = {.major = 3, .minor = 0},
         .build = {.application = QStringLiteral("Javelin-Mail"),
                   .revision = QStringLiteral("test")},
         .startTimeoutMilliseconds = 10,
         .startDaemonIfMissing = false}};

    javelin::app::GuiMailApplicationEvents events{session};
    std::optional<javelin::app::ThreadMaterializationProgress> received;
    QObject::connect(&events,
                     &javelin::app::MailApplicationEventsPort::threadMaterializationProgress,
                     [&received](javelin::app::ThreadMaterializationProgress progress)
                     { received = std::move(progress); });

    session.onBoundaryEvent(javelin::protocol::ThreadMaterializationProgress{
        .accountId = QStringLiteral("account-a"),
        .threadIds = {QStringLiteral("thread-a"), QStringLiteral("thread-b")},
        .inFlight = false,
        .success = false,
        .error = QStringLiteral("temporary failure"),
    });

    REQUIRE(received.has_value());
    CHECK(received->accountId == QStringLiteral("account-a"));
    CHECK(received->threadIds ==
          QStringList{QStringLiteral("thread-a"), QStringLiteral("thread-b")});
    CHECK_FALSE(received->inFlight);
    CHECK_FALSE(received->success);
    CHECK(received->error == QStringLiteral("temporary failure"));
}

TEST_CASE("GUI bootstrap does not offer the packaged service for a source build", "[app][gui]")
{
    javelin::app::GuiDaemonSession session{
        {.runtimeDirectory = QStringLiteral("/tmp"),
         .socketPath = QStringLiteral("/tmp/unused-javelin-test.sock"),
         .daemonExecutable = {},
         .protocol = {.major = 3, .minor = 0},
         .build = {.application = QStringLiteral("Javelin-Mail"),
                   .revision = QStringLiteral("test")},
         .startTimeoutMilliseconds = 10,
         .startDaemonIfMissing = false}};

    CHECK_FALSE(session.canUseSystemdUserService());
}
