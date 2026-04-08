#include "jmap/sync/LongPollMailboxObserver.h"

#include <QCoroTask>

#include <QCoreApplication>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <variant>

namespace
{

    void ensureApplication()
    {
        if (QCoreApplication::instance() != nullptr)
        {
            return;
        }

        static int argc = 1;
        static char appName[] = "javelin-tests";
        static char* argv[] = {appName, nullptr};
        static QCoreApplication application(argc, argv);
        Q_UNUSED(application);
    }

    class FakeMailboxRefresher final : public javelin::jmap::sync::AbstractLongPollMailboxRefresher
    {
      public:
        struct Request
        {
            std::string accountId;
            std::string mailboxId;
        };

        std::vector<Request> requests;
        std::vector<javelin::jmap::sync::MailboxRefreshResult> queuedResults;

        [[nodiscard]] QCoro::Task<javelin::jmap::sync::MailboxRefreshResult>
        refreshMailbox(const std::string_view accountId, const std::string_view mailboxId) override
        {
            requests.push_back(Request{
                .accountId = std::string{accountId},
                .mailboxId = std::string{mailboxId},
            });
            REQUIRE_FALSE(queuedResults.empty());

            auto result = std::move(queuedResults.front());
            queuedResults.erase(queuedResults.begin());
            co_return result;
        }
    };

    class FakeNotificationSink final : public javelin::jmap::sync::AbstractRefreshNotificationSink
    {
      public:
        struct Publication
        {
            std::string accountId;
            std::string mailboxId;
            std::vector<javelin::jmap::sync::RefreshNotificationCandidate> candidates;
        };

        std::vector<Publication> publications;

        void publish(const std::string_view accountId, const std::string_view mailboxId,
                     const std::vector<javelin::jmap::sync::RefreshNotificationCandidate>& candidates) override
        {
            publications.push_back(Publication{
                .accountId = std::string{accountId},
                .mailboxId = std::string{mailboxId},
                .candidates = candidates,
            });
        }
    };

} // namespace

TEST_CASE("long poll mailbox observer refreshes mailbox and publishes notification candidates",
          "[jmap][sync][longpoll]")
{
    ensureApplication();

    FakeMailboxRefresher refresher;
    refresher.queuedResults.push_back(javelin::jmap::sync::MailboxRefreshSummary{
        .representativeCount = 1,
        .usedIncrementalRefresh = true,
        .changedEmailIds = {"eml-1"},
        .insertedEmailIds = {"eml-1"},
        .removedEmailIds = {},
        .requiresNotificationScan = true,
        .notificationCandidates =
            {
                javelin::jmap::sync::RefreshNotificationCandidate{
                    .emailId = "eml-1",
                    .threadId = "thr-1",
                    .subject = "Subject",
                    .receivedAt = "2026-04-06T12:22:33Z",
                },
            },
    });

    FakeNotificationSink sink;
    javelin::jmap::sync::LongPollMailboxObserver observer{refresher, sink, "account-1",
                                                          "mbx-inbox"};
    QCoro::waitFor(observer.onUpdate(javelin::jmap::sync::LongPollResponse{
        .newState = "state-2",
        .changedTypes = {"Email"},
    }));

    REQUIRE(refresher.requests.size() == 1);
    CHECK(refresher.requests.front().accountId == "account-1");
    CHECK(refresher.requests.front().mailboxId == "mbx-inbox");
    REQUIRE(sink.publications.size() == 1);
    CHECK(sink.publications.front().candidates.size() == 1);
    CHECK(sink.publications.front().candidates.front().emailId == "eml-1");
}

TEST_CASE("long poll mailbox observer ignores unrelated changed types",
          "[jmap][sync][longpoll]")
{
    ensureApplication();

    FakeMailboxRefresher refresher;
    FakeNotificationSink sink;
    javelin::jmap::sync::LongPollMailboxObserver observer{refresher, sink, "account-1",
                                                          "mbx-inbox"};
    QCoro::waitFor(observer.onUpdate(javelin::jmap::sync::LongPollResponse{
        .newState = "state-2",
        .changedTypes = {"Identity"},
    }));

    CHECK(refresher.requests.empty());
    CHECK(sink.publications.empty());
}

TEST_CASE("long poll mailbox observer suppresses publication when refresh returns no candidates",
          "[jmap][sync][longpoll]")
{
    ensureApplication();

    FakeMailboxRefresher refresher;
    refresher.queuedResults.push_back(javelin::jmap::sync::MailboxRefreshSummary{
        .representativeCount = 1,
        .usedIncrementalRefresh = false,
        .changedEmailIds = {},
        .insertedEmailIds = {},
        .removedEmailIds = {},
        .requiresNotificationScan = false,
        .notificationCandidates = {},
    });

    FakeNotificationSink sink;
    javelin::jmap::sync::LongPollMailboxObserver observer{refresher, sink, "account-1",
                                                          "mbx-inbox"};
    QCoro::waitFor(observer.onUpdate(javelin::jmap::sync::LongPollResponse{
        .newState = "state-2",
        .changedTypes = {"Mailbox"},
    }));

    REQUIRE(refresher.requests.size() == 1);
    CHECK(sink.publications.empty());
}
