#include "app/CacheLocationProvider.h"
#include "app/MailApplicationEventsPorts.h"
#include "app/MailMutationApplicationService.h"
#include "app/MailQueryApplicationService.h"
#include "app/MailboxSession.h"
#include "daemon/DaemonServices.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailboxMessageReadRepository.h"
#include "jmap/cache/MailboxRepository.h"
#include "jmap/cache/MailboxWindowRepository.h"
#include "jmap/cache/QueryWindowReadRepository.h"
#include "jmap/sync/MailboxQueryDescriptor.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
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
            static char applicationName[] = "javelin-optimistic-presentation-tests";
            static char* argv[] = {applicationName, nullptr};
            m_application = std::make_unique<QCoreApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };

    template <typename Predicate> void waitFor(Predicate predicate)
    {
        QElapsedTimer timer;
        timer.start();
        while (!predicate() && timer.elapsed() < 2000)
        {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QThread::msleep(1);
        }
        REQUIRE(predicate());
    }

    [[nodiscard]] std::string queryKey(const std::string& mailboxId)
    {
        return javelin::jmap::sync::mailboxQueryKey({
            .mailboxId = mailboxId,
            .sortProperty = "receivedAt",
            .isAscending = false,
            .collapseThreads = true,
        });
    }
} // namespace

TEST_CASE("optimistic archive reaches mailbox sessions through daemon cache invalidation",
          "[app][mailbox-session][optimistic][integration]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    QTemporaryDir temporaryDirectory;
    REQUIRE(temporaryDirectory.isValid());
    const auto locationResult =
        javelin::app::CacheLocationProvider{temporaryDirectory.path()}.loadOrCreate();
    REQUIRE(std::holds_alternative<javelin::app::CacheLocation>(locationResult));
    javelin::app::DaemonServices services{std::get<javelin::app::CacheLocation>(locationResult)};
    auto& databaseConnection = services.databaseConnection();

    QSqlQuery seed{databaseConnection.database()};
    REQUIRE(seed.exec(QStringLiteral(
        "INSERT INTO accounts(account_id,email_address,session_url,is_primary,cap_mail) "
        "VALUES('account-1','user@example.test','http://127.0.0.1:9/jmap',1,1)")));

    const javelin::jmap::domain::MailboxRights rights{
        .mayReadItems = true,
        .mayAddItems = true,
        .mayRemoveItems = true,
        .maySetSeen = true,
        .maySetKeywords = true,
    };
    javelin::jmap::domain::Mailbox inbox;
    inbox.id = "inbox";
    inbox.name = "Inbox";
    inbox.role = "inbox";
    inbox.totalEmails = 1;
    inbox.totalThreads = 1;
    inbox.isSubscribed = true;
    inbox.myRights = rights;
    javelin::jmap::domain::Mailbox archive;
    archive.id = "archive";
    archive.name = "Archive";
    archive.role = "archive";
    archive.totalEmails = 0;
    archive.totalThreads = 0;
    archive.isSubscribed = true;
    archive.myRights = rights;
    javelin::jmap::domain::Mailbox junk;
    junk.id = "junk";
    junk.name = "Junk";
    junk.role = "junk";
    junk.totalEmails = 0;
    junk.totalThreads = 0;
    junk.isSubscribed = true;
    junk.myRights = rights;
    javelin::jmap::cache::MailboxRepository mailboxes{databaseConnection};
    REQUIRE_FALSE(mailboxes.replaceAll("account-1", {inbox, archive, junk}).has_value());

    javelin::jmap::domain::Email email;
    email.id = "email-1";
    email.threadId = "thread-1";
    email.mailboxIds = {"inbox"};
    email.receivedAt = "2026-08-28T01:00:00Z";
    email.subject = "Projected message";
    email.preview = "Projection test";
    javelin::jmap::cache::EmailRepository emails{databaseConnection};
    REQUIRE_FALSE(emails.replaceAll("account-1", {email}).has_value());

    const auto inboxQueryKey = queryKey("inbox");
    const auto archiveQueryKey = queryKey("archive");
    javelin::jmap::cache::MailboxWindowRepository windows{databaseConnection};
    REQUIRE_FALSE(windows
                      .replace({
                          .accountId = "account-1",
                          .mailboxId = "inbox",
                          .queryKey = inboxQueryKey,
                          .requestedOffset = 0,
                          .requestedLimit = 100,
                          .position = 0,
                          .returnedLimit = 1,
                          .total = 1,
                          .queryState = "query-state-1",
                          .emailIds = {"email-1"},
                      })
                      .has_value());
    REQUIRE_FALSE(windows
                      .replace({
                          .accountId = "account-1",
                          .mailboxId = "archive",
                          .queryKey = archiveQueryKey,
                          .requestedOffset = 0,
                          .requestedLimit = 100,
                          .position = 0,
                          .returnedLimit = 0,
                          .total = 0,
                          .queryState = "query-state-1",
                          .emailIds = {},
                      })
                      .has_value());

    javelin::app::MailboxSession inboxSession{"account-1",
                                              "inbox",
                                              QStringLiteral("Inbox"),
                                              std::optional<std::string>{"inbox"},
                                              {},
                                              databaseConnection.database().databaseName(),
                                              services.mailQueryApplicationService(),
                                              100,
                                              services.mailApplicationEvents()};
    javelin::app::MailboxSession archiveSession{"account-1",
                                                "archive",
                                                QStringLiteral("Archive"),
                                                std::optional<std::string>{"archive"},
                                                {},
                                                databaseConnection.database().databaseName(),
                                                services.mailQueryApplicationService(),
                                                100,
                                                services.mailApplicationEvents()};

    inboxSession.loadCachedState();
    archiveSession.loadCachedState();
    waitFor(
        [&]
        {
            return inboxSession.state().cacheLoaded && inboxSession.state().items.size() == 1 &&
                   archiveSession.state().cacheLoaded && archiveSession.state().items.empty();
        });
    CHECK(inboxSession.state().items.front().emailId == "email-1");

    std::vector<javelin::app::MailCacheInvalidation> invalidations;
    bool commandCompleted = false;
    bool optimisticInvalidationBeforeCompletion = false;
    QObject::connect(&services.mailApplicationEvents(),
                     &javelin::app::MailApplicationEventsPort::cacheInvalidated,
                     &services.mailApplicationEvents(),
                     [&](javelin::app::MailCacheInvalidation invalidation)
                     {
                         if (invalidation.change.optimisticProjection)
                             optimisticInvalidationBeforeCompletion = !commandCompleted;
                         invalidations.push_back(std::move(invalidation));
                     });

    const auto queued =
        QCoro::waitFor(services.mailMutationApplicationService().queueMailboxSelectionMutation({
            .accountId = "account-1",
            .selection = {javelin::app::SelectedEmail{.emailId = "email-1"}},
            .operation = javelin::app::MailboxSelectionOperation::Archive,
            .sourceMailboxId = "inbox",
            .destinationMailboxId = std::nullopt,
        }));
    commandCompleted = true;

    const auto* summary = std::get_if<javelin::app::QueuedMailboxSelectionMutation>(&queued);
    REQUIRE(summary != nullptr);
    CHECK(summary->queuedEmailCount == 1);
    CHECK(optimisticInvalidationBeforeCompletion);
    REQUIRE_FALSE(invalidations.empty());
    const auto optimisticInvalidation =
        std::ranges::find_if(invalidations, [](const auto& invalidation)
                             { return invalidation.change.optimisticProjection; });
    REQUIRE(optimisticInvalidation != invalidations.end());
    CHECK(optimisticInvalidation->change.mailboxIds.contains(QStringLiteral("inbox")));
    CHECK(optimisticInvalidation->change.mailboxIds.contains(QStringLiteral("archive")));

    waitFor(
        [&]
        {
            return inboxSession.state().cacheLoaded && inboxSession.state().items.empty() &&
                   archiveSession.state().cacheLoaded && archiveSession.state().items.size() == 1;
        });
    CHECK(archiveSession.state().items.front().emailId == "email-1");

    javelin::jmap::cache::MailboxMessageReadRepository mailboxMessages{databaseConnection};
    javelin::jmap::cache::QueryWindowReadRepository queryWindows{databaseConnection,
                                                                 mailboxMessages};
    const auto inboxPageResult =
        queryWindows.loadMailboxWindow("account-1", inboxQueryKey, 0, 100, {});
    const auto* inboxPage =
        std::get_if<std::optional<javelin::jmap::cache::MailboxWindowPage>>(&inboxPageResult);
    REQUIRE(inboxPage != nullptr);
    REQUIRE(inboxPage->has_value());
    CHECK((*inboxPage)->coverage == javelin::jmap::cache::QueryWindowCoverage::LocallyProjected);
    CHECK((*inboxPage)->items.empty());

    const auto archivePageResult =
        queryWindows.loadMailboxWindow("account-1", archiveQueryKey, 0, 100, {});
    const auto* archivePage =
        std::get_if<std::optional<javelin::jmap::cache::MailboxWindowPage>>(&archivePageResult);
    REQUIRE(archivePage != nullptr);
    REQUIRE(archivePage->has_value());
    CHECK((*archivePage)->coverage == javelin::jmap::cache::QueryWindowCoverage::LocallyProjected);
    REQUIRE((*archivePage)->items.size() == 1);
    CHECK((*archivePage)->items.front().emailId == "email-1");

    const auto markedRead =
        services.mailMutationApplicationService().queueMarkEmailRead("account-1", "email-1");
    REQUIRE(std::holds_alternative<javelin::app::QueuedMessageSelectionMutation>(markedRead));
    CHECK(std::get<javelin::app::QueuedMessageSelectionMutation>(markedRead).queuedEmailCount == 1);
    waitFor(
        [&]
        {
            return archiveSession.state().items.size() == 1 &&
                   !archiveSession.state().items.front().isUnread;
        });

    const auto markedUnread =
        QCoro::waitFor(services.mailMutationApplicationService().queueMarkMessagesUnread(
            "account-1", std::optional<std::string>{"archive"},
            {javelin::app::SelectedEmail{.emailId = "email-1"}}));
    REQUIRE(std::holds_alternative<javelin::app::QueuedMessageSelectionMutation>(markedUnread));
    CHECK(std::get<javelin::app::QueuedMessageSelectionMutation>(markedUnread).queuedEmailCount ==
          1);
    waitFor(
        [&]
        {
            return archiveSession.state().items.size() == 1 &&
                   archiveSession.state().items.front().isUnread;
        });

    // Queue opposite metadata projections back-to-back without waiting for the first session
    // reload. The session must converge on the final committed SQLite projection rather than
    // controller-side command completion ordering.
    const auto flagged =
        QCoro::waitFor(services.mailMutationApplicationService().queueSetMessagesFlagged(
            "account-1", std::optional<std::string>{"archive"},
            {javelin::app::SelectedEmail{.emailId = "email-1"}}, true));
    REQUIRE(std::holds_alternative<javelin::app::QueuedMessageSelectionMutation>(flagged));
    const auto unflagged =
        QCoro::waitFor(services.mailMutationApplicationService().queueSetMessagesFlagged(
            "account-1", std::optional<std::string>{"archive"},
            {javelin::app::SelectedEmail{.emailId = "email-1"}}, false));
    REQUIRE(std::holds_alternative<javelin::app::QueuedMessageSelectionMutation>(unflagged));
    waitFor(
        [&]
        {
            return archiveSession.state().items.size() == 1 &&
                   !archiveSession.state().items.front().isFlagged;
        });

    const auto tagged =
        QCoro::waitFor(services.mailMutationApplicationService().queueSetMessagesTag(
            "account-1", std::optional<std::string>{"archive"},
            {javelin::app::SelectedEmail{.emailId = "email-1"}}, "project-test", true));
    REQUIRE(std::holds_alternative<javelin::app::QueuedMessageSelectionMutation>(tagged));
    const auto untagged =
        QCoro::waitFor(services.mailMutationApplicationService().queueSetMessagesTag(
            "account-1", std::optional<std::string>{"archive"},
            {javelin::app::SelectedEmail{.emailId = "email-1"}}, "project-test", false));
    REQUIRE(std::holds_alternative<javelin::app::QueuedMessageSelectionMutation>(untagged));
    waitFor(
        [&]
        {
            const auto current = emails.find("account-1", "email-1");
            const auto* value = std::get_if<std::optional<javelin::jmap::domain::Email>>(&current);
            return value != nullptr && value->has_value() &&
                   !std::ranges::contains((*value)->keywords, std::string{"project-test"});
        });

    const auto copied =
        QCoro::waitFor(services.mailMutationApplicationService().queueMailboxSelectionMutation({
            .accountId = "account-1",
            .selection = {javelin::app::SelectedEmail{.emailId = "email-1"}},
            .operation = javelin::app::MailboxSelectionOperation::Copy,
            .sourceMailboxId = "archive",
            .destinationMailboxId = "inbox",
        }));
    REQUIRE(std::holds_alternative<javelin::app::QueuedMailboxSelectionMutation>(copied));
    waitFor(
        [&]
        {
            return inboxSession.state().items.size() == 1 &&
                   archiveSession.state().items.size() == 1;
        });

    const auto moved =
        QCoro::waitFor(services.mailMutationApplicationService().queueMailboxSelectionMutation({
            .accountId = "account-1",
            .selection = {javelin::app::SelectedEmail{.emailId = "email-1"}},
            .operation = javelin::app::MailboxSelectionOperation::Move,
            .sourceMailboxId = "inbox",
            .destinationMailboxId = "archive",
        }));
    REQUIRE(std::holds_alternative<javelin::app::QueuedMailboxSelectionMutation>(moved));
    waitFor(
        [&]
        { return inboxSession.state().items.empty() && archiveSession.state().items.size() == 1; });

    const auto junked =
        QCoro::waitFor(services.mailMutationApplicationService().queueMailboxSelectionMutation({
            .accountId = "account-1",
            .selection = {javelin::app::SelectedEmail{.emailId = "email-1"}},
            .operation = javelin::app::MailboxSelectionOperation::Junk,
            .sourceMailboxId = "archive",
            .destinationMailboxId = std::nullopt,
        }));
    REQUIRE(std::holds_alternative<javelin::app::QueuedMailboxSelectionMutation>(junked));
    waitFor(
        [&]
        {
            const auto current = emails.find("account-1", "email-1");
            const auto* value = std::get_if<std::optional<javelin::jmap::domain::Email>>(&current);
            return archiveSession.state().items.empty() && value != nullptr && value->has_value() &&
                   std::ranges::contains((*value)->mailboxIds, std::string{"junk"}) &&
                   std::ranges::contains((*value)->keywords, std::string{"$junk"});
        });

    const auto notJunked =
        QCoro::waitFor(services.mailMutationApplicationService().queueMailboxSelectionMutation({
            .accountId = "account-1",
            .selection = {javelin::app::SelectedEmail{.emailId = "email-1"}},
            .operation = javelin::app::MailboxSelectionOperation::NotJunk,
            .sourceMailboxId = "junk",
            .destinationMailboxId = std::nullopt,
        }));
    REQUIRE(std::holds_alternative<javelin::app::QueuedMailboxSelectionMutation>(notJunked));
    waitFor(
        [&]
        {
            const auto current = emails.find("account-1", "email-1");
            const auto* value = std::get_if<std::optional<javelin::jmap::domain::Email>>(&current);
            return inboxSession.state().items.size() == 1 && value != nullptr &&
                   value->has_value() &&
                   std::ranges::contains((*value)->mailboxIds, std::string{"inbox"}) &&
                   !std::ranges::contains((*value)->mailboxIds, std::string{"junk"}) &&
                   std::ranges::contains((*value)->keywords, std::string{"$notjunk"}) &&
                   !std::ranges::contains((*value)->keywords, std::string{"$junk"});
        });

    const auto invalidationsBeforeDestroy = invalidations.size();
    const auto destroyed =
        QCoro::waitFor(services.mailMutationApplicationService().queueDestroyMessages(
            "account-1", std::optional<std::string>{"inbox"},
            {javelin::app::SelectedEmail{.emailId = "email-1"}}));
    REQUIRE(std::holds_alternative<javelin::app::QueuedMessageSelectionMutation>(destroyed));
    REQUIRE(invalidations.size() > invalidationsBeforeDestroy);
    const auto destroyInvalidation = std::ranges::find_if(
        invalidations.begin() + static_cast<std::ptrdiff_t>(invalidationsBeforeDestroy),
        invalidations.end(),
        [](const auto& invalidation) { return invalidation.change.optimisticProjection; });
    REQUIRE(destroyInvalidation != invalidations.end());
    CHECK(destroyInvalidation->change.mailboxIds.contains(QStringLiteral("inbox")));

    // Permanent destroy has never had a local tombstone projection: the journal retains the Email
    // until server settlement. The important cleanup invariant is that command completion does not
    // invent a second presentation path while the source mailbox still receives the authoritative
    // cache invalidation.
    const auto pendingDestroyEmail = emails.find("account-1", "email-1");
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(pendingDestroyEmail));
    REQUIRE(std::get<std::optional<javelin::jmap::domain::Email>>(pendingDestroyEmail).has_value());
    CHECK(inboxSession.state().items.size() == 1);

    const auto optimisticInvalidationCount =
        std::ranges::count_if(invalidations, [](const auto& invalidation)
                              { return invalidation.change.optimisticProjection; });
    CHECK(optimisticInvalidationCount >= 12);

    QSqlQuery mutationCount{databaseConnection.database()};
    REQUIRE(mutationCount.exec(QStringLiteral("SELECT COUNT(*) FROM mutation_journal")));
    REQUIRE(mutationCount.next());
    CHECK(mutationCount.value(0).toInt() == 12);
}
