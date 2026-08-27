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
    javelin::jmap::cache::MailboxRepository mailboxes{databaseConnection};
    REQUIRE_FALSE(mailboxes.replaceAll("account-1", {inbox, archive}).has_value());

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

    javelin::app::MailboxSession inboxSession{
        "account-1", "inbox", QStringLiteral("Inbox"), std::optional<std::string>{"inbox"}, {},
        databaseConnection.database().databaseName(), services.mailQueryApplicationService(), 100,
        services.mailApplicationEvents()};
    javelin::app::MailboxSession archiveSession{
        "account-1", "archive", QStringLiteral("Archive"), std::optional<std::string>{"archive"}, {},
        databaseConnection.database().databaseName(), services.mailQueryApplicationService(), 100,
        services.mailApplicationEvents()};

    inboxSession.loadCachedState();
    archiveSession.loadCachedState();
    waitFor([&]
            {
                return inboxSession.state().cacheLoaded && inboxSession.state().items.size() == 1 &&
                       archiveSession.state().cacheLoaded && archiveSession.state().items.empty();
            });
    CHECK(inboxSession.state().items.front().emailId == "email-1");

    std::vector<javelin::app::MailCacheInvalidation> invalidations;
    bool commandCompleted = false;
    bool optimisticInvalidationBeforeCompletion = false;
    QObject::connect(
        &services.mailApplicationEvents(),
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

    waitFor([&]
            {
                return inboxSession.state().cacheLoaded && inboxSession.state().items.empty() &&
                       archiveSession.state().cacheLoaded && archiveSession.state().items.size() == 1;
            });
    CHECK(archiveSession.state().items.front().emailId == "email-1");

    javelin::jmap::cache::MailboxMessageReadRepository mailboxMessages{databaseConnection};
    javelin::jmap::cache::QueryWindowReadRepository queryWindows{databaseConnection, mailboxMessages};
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

    QSqlQuery mutationCount{databaseConnection.database()};
    REQUIRE(mutationCount.exec(QStringLiteral("SELECT COUNT(*) FROM mutation_journal")));
    REQUIRE(mutationCount.next());
    CHECK(mutationCount.value(0).toInt() == 1);
}
