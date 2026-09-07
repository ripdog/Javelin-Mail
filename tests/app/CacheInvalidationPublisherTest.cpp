#include "app/CacheInvalidationPublisher.h"
#include "protocol/SocketWireCodecInternal.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <utility>
#include <vector>

TEST_CASE("cache invalidation publisher coalesces one account after commit",
          "[app][cache][invalidation]")
{
    javelin::app::CacheInvalidationPublisher publisher;
    std::vector<javelin::app::MailCacheInvalidation> invalidations;
    QObject::connect(&publisher, &javelin::app::CacheInvalidationPublisher::invalidated,
                     [&invalidations](javelin::app::MailCacheInvalidation invalidation)
                     { invalidations.push_back(std::move(invalidation)); });

    publisher.publish(javelin::app::MailCacheChange{
        .accountId = QStringLiteral("account-a"),
        .mailboxIds = {QStringLiteral("mailbox-a")},
        .queryWindows = {{
            .mailboxId = QStringLiteral("mailbox-a"),
            .queryKey = QStringLiteral("query-a"),
            .offset = 0,
            .limit = 50,
            .total = 100,
        }},
        .searchWindows = {},
        .mailboxTreeChanged = true,
        .emailObjectsChanged = false,
        .optimisticProjection = false,
        .contactsChanged = true,
    });
    publisher.publish(javelin::app::MailCacheChange{
        .accountId = QStringLiteral("account-a"),
        .mailboxIds = {QStringLiteral("mailbox-b")},
        .queryWindows = {},
        .searchWindows = {},
        .mailboxTreeChanged = false,
        .emailObjectsChanged = true,
        .optimisticProjection = true,
    });

    publisher.flush();

    REQUIRE(invalidations.size() == 1);
    const auto& invalidation = invalidations.front();
    CHECK(invalidation.epoch == 0);
    CHECK(invalidation.change.mailboxTreeChanged);
    CHECK(invalidation.change.emailObjectsChanged);
    CHECK(invalidation.change.optimisticProjection);
    CHECK(invalidation.change.contactsChanged);
    CHECK(invalidation.change.mailboxIds ==
          QStringList{QStringLiteral("mailbox-a"), QStringLiteral("mailbox-b")});
    CHECK(std::ranges::find(invalidation.changedDomains,
                            javelin::protocol::ChangedDomain::MailboxTree) !=
          invalidation.changedDomains.end());
    CHECK(std::ranges::find(invalidation.changedDomains,
                            javelin::protocol::ChangedDomain::MailQueryWindows) !=
          invalidation.changedDomains.end());
    CHECK(std::ranges::find(invalidation.changedDomains,
                            javelin::protocol::ChangedDomain::MessageMetadata) !=
          invalidation.changedDomains.end());
    CHECK(std::ranges::find(invalidation.changedDomains,
                            javelin::protocol::ChangedDomain::Contacts) !=
          invalidation.changedDomains.end());
    CHECK(std::ranges::find(invalidation.affectedKeys, QStringLiteral("account-a")) !=
          invalidation.affectedKeys.end());
    CHECK(std::ranges::find(invalidation.affectedKeys, QStringLiteral("mailbox-b")) !=
          invalidation.affectedKeys.end());
}

TEST_CASE("cache invalidation publisher preserves account queue order and bounds keys",
          "[app][cache][invalidation]")
{
    javelin::app::CacheInvalidationPublisher publisher;
    std::vector<javelin::app::MailCacheInvalidation> invalidations;
    QObject::connect(&publisher, &javelin::app::CacheInvalidationPublisher::invalidated,
                     [&invalidations](javelin::app::MailCacheInvalidation invalidation)
                     { invalidations.push_back(std::move(invalidation)); });

    javelin::app::MailCacheChange first{
        .accountId = QStringLiteral("account-a"),
        .mailboxIds = {},
        .queryWindows = {},
        .searchWindows = {},
        .mailboxTreeChanged = false,
        .emailObjectsChanged = false,
        .optimisticProjection = false,
    };
    for (int index = 0; index < 80; ++index)
        first.mailboxIds.push_back(QStringLiteral("mailbox-%1").arg(index));
    publisher.publish(std::move(first));
    publisher.publish(javelin::app::MailCacheChange{
        .accountId = QStringLiteral("account-b"),
        .mailboxIds = {QStringLiteral("mailbox-b")},
        .queryWindows = {},
        .searchWindows = {},
        .mailboxTreeChanged = false,
        .emailObjectsChanged = false,
        .optimisticProjection = false,
    });

    publisher.flush();

    REQUIRE(invalidations.size() == 2);
    CHECK(invalidations[0].epoch == 0);
    CHECK(invalidations[1].epoch == 0);
    CHECK(invalidations[0].change.accountId == QStringLiteral("account-a"));
    CHECK(invalidations[1].change.accountId == QStringLiteral("account-b"));
    CHECK(invalidations[0].change.mailboxIds.size() == 80);
    CHECK(invalidations[0].affectedKeys.size() <= 64);
}

TEST_CASE("cache invalidation publisher emits contacts for contact-only changes",
          "[app][cache][invalidation]")
{
    javelin::app::CacheInvalidationPublisher publisher;
    std::vector<javelin::app::MailCacheInvalidation> invalidations;
    QObject::connect(&publisher, &javelin::app::CacheInvalidationPublisher::invalidated,
                     [&invalidations](javelin::app::MailCacheInvalidation invalidation)
                     { invalidations.push_back(std::move(invalidation)); });

    publisher.publish(javelin::app::MailCacheChange{
        .accountId = QStringLiteral("contacts-account"),
        .mailboxIds = {},
        .queryWindows = {},
        .searchWindows = {},
        .mailboxTreeChanged = false,
        .emailObjectsChanged = false,
        .optimisticProjection = false,
        .contactsChanged = true,
    });
    publisher.flush();

    REQUIRE(invalidations.size() == 1);
    CHECK(invalidations.front().changedDomains ==
          std::vector{javelin::protocol::ChangedDomain::Contacts});
    CHECK(invalidations.front().affectedKeys ==
          std::vector<QString>{QStringLiteral("contacts-account")});
}

TEST_CASE("cache invalidation publisher emits mail tags without a fake query domain",
          "[app][cache][invalidation][tags]")
{
    javelin::app::CacheInvalidationPublisher publisher;
    std::optional<javelin::app::MailCacheInvalidation> invalidation;
    QObject::connect(&publisher, &javelin::app::CacheInvalidationPublisher::invalidated,
                     [&invalidation](javelin::app::MailCacheInvalidation value)
                     { invalidation = std::move(value); });

    publisher.publish(javelin::app::MailCacheChange{
        .accountId = QStringLiteral("account-a"),
        .mailboxIds = {},
        .queryWindows = {},
        .searchWindows = {},
        .mailTagsChanged = true,
    });
    publisher.flush();

    REQUIRE(invalidation.has_value());
    CHECK(invalidation->changedDomains == std::vector{javelin::protocol::ChangedDomain::MailTags});
}

TEST_CASE("cache invalidation publisher targets hydrated message content",
          "[app][cache][invalidation][message-content]")
{
    javelin::app::CacheInvalidationPublisher publisher;
    std::optional<javelin::app::MailCacheInvalidation> invalidation;
    QObject::connect(&publisher, &javelin::app::CacheInvalidationPublisher::invalidated,
                     [&invalidation](javelin::app::MailCacheInvalidation value)
                     { invalidation = std::move(value); });

    publisher.publish(javelin::app::MailCacheChange{
        .accountId = QStringLiteral("account-a"),
        .mailboxIds = {},
        .queryWindows = {},
        .searchWindows = {},
        .messageContentEmailIds = {QStringLiteral("email-a")},
    });
    publisher.flush();

    REQUIRE(invalidation.has_value());
    CHECK(invalidation->changedDomains ==
          std::vector{javelin::protocol::ChangedDomain::MessageContent});
    CHECK(invalidation->affectedKeys ==
          std::vector<QString>{QStringLiteral("account-a"), QStringLiteral("email-a")});
    CHECK(invalidation->change.messageContentEmailIds == QStringList{QStringLiteral("email-a")});
}

TEST_CASE("cache invalidation publisher does not invent a domain for semantic-empty changes",
          "[app][cache][invalidation]")
{
    javelin::app::CacheInvalidationPublisher publisher;
    std::optional<javelin::app::MailCacheInvalidation> invalidation;
    QObject::connect(&publisher, &javelin::app::CacheInvalidationPublisher::invalidated,
                     [&invalidation](javelin::app::MailCacheInvalidation value)
                     { invalidation = std::move(value); });

    publisher.publish(javelin::app::MailCacheChange{
        .accountId = QStringLiteral("account-a"),
        .mailboxIds = {},
        .queryWindows = {},
        .searchWindows = {},
    });
    publisher.flush();

    REQUIRE(invalidation.has_value());
    CHECK(invalidation->changedDomains.empty());
    CHECK(invalidation->affectedKeys == std::vector<QString>{QStringLiteral("account-a")});
}

TEST_CASE("cache invalidation publisher can publish a committed mutation synchronously",
          "[app][cache][invalidation][ordering]")
{
    javelin::app::CacheInvalidationPublisher publisher;
    std::optional<javelin::app::MailCacheInvalidation> invalidation;
    QObject::connect(&publisher, &javelin::app::CacheInvalidationPublisher::invalidated,
                     [&invalidation](javelin::app::MailCacheInvalidation value)
                     { invalidation = std::move(value); });

    publisher.publishImmediately(javelin::app::MailCacheChange{
        .accountId = QStringLiteral("account-a"),
        .mailboxIds = {QStringLiteral("inbox")},
        .queryWindows = {},
        .searchWindows = {},
        .optimisticProjection = true,
    });

    REQUIRE(invalidation.has_value());
    CHECK(invalidation->epoch == 0);
    CHECK(invalidation->change.optimisticProjection);
}

TEST_CASE("cache invalidation publisher preserves every window across bounded batches",
          "[app][cache][invalidation]")
{
    javelin::app::CacheInvalidationPublisher publisher;
    std::vector<javelin::app::MailCacheInvalidation> invalidations;
    QObject::connect(&publisher, &javelin::app::CacheInvalidationPublisher::invalidated,
                     [&invalidations](auto invalidation)
                     { invalidations.push_back(std::move(invalidation)); });
    for (std::size_t index = 0; index < 600; ++index)
    {
        javelin::app::MailCacheChange change;
        change.accountId = QStringLiteral("account");
        change.queryWindows.push_back({.mailboxId = QStringLiteral("inbox"),
                                       .queryKey = QStringLiteral("query-%1").arg(index),
                                       .offset = index,
                                       .limit = 50,
                                       .total = 1000});
        change.searchWindows.push_back({.queryKey = QStringLiteral("search-%1").arg(index),
                                        .offset = index,
                                        .limit = 25,
                                        .total = 2000});
        publisher.publish(std::move(change));
    }
    publisher.flush();
    REQUIRE(invalidations.size() == 3);
    std::size_t index = 0;
    for (const auto& invalidation : invalidations)
    {
        CHECK(invalidation.change.queryWindows.size() <= 256);
        REQUIRE(invalidation.change.searchWindows.size() ==
                invalidation.change.queryWindows.size());
        for (std::size_t offset = 0; offset < invalidation.change.queryWindows.size();
             ++offset, ++index)
        {
            const auto& mailbox = invalidation.change.queryWindows[offset];
            const auto& search = invalidation.change.searchWindows[offset];
            CHECK(mailbox.mailboxId == QStringLiteral("inbox"));
            CHECK(mailbox.queryKey == QStringLiteral("query-%1").arg(index));
            CHECK(mailbox.offset == index);
            CHECK(mailbox.limit == 50);
            CHECK(mailbox.total == 1000);
            CHECK(search.queryKey == QStringLiteral("search-%1").arg(index));
            CHECK(search.offset == index);
            CHECK(search.limit == 25);
            CHECK(search.total == 2000);
        }
    }
    CHECK(index == 600);
}

TEST_CASE("cache invalidation publisher bounds frames containing long query identities",
          "[app][cache][invalidation]")
{
    javelin::app::CacheInvalidationPublisher publisher;
    std::size_t mailboxCount = 0;
    std::size_t searchCount = 0;
    QObject::connect(
        &publisher, &javelin::app::CacheInvalidationPublisher::invalidated,
        [&](const auto& invalidation)
        {
            javelin::protocol::CacheInvalidation wire;
            wire.accountId = invalidation.change.accountId;
            wire.affectedKeys = invalidation.affectedKeys;
            wire.changedDomains = invalidation.changedDomains;
            for (const auto& window : invalidation.change.queryWindows)
                wire.mailboxWindows.push_back({.mailboxId = window.mailboxId,
                                               .queryKey = window.queryKey,
                                               .offset = window.offset,
                                               .limit = window.limit,
                                               .total = window.total});
            for (const auto& window : invalidation.change.searchWindows)
                wire.searchWindows.push_back({.queryKey = window.queryKey,
                                              .offset = window.offset,
                                              .limit = window.limit,
                                              .total = window.total});
            const auto encoded = javelin::protocol::detail::encodeBoundaryEvent(wire, {});
            REQUIRE(std::holds_alternative<javelin::protocol::detail::EncodedPayload>(encoded));
            CHECK(std::get<javelin::protocol::detail::EncodedPayload>(encoded).payload.size() <=
                  1024 * 1024);
            mailboxCount += wire.mailboxWindows.size();
            searchCount += wire.searchWindows.size();
        });
    javelin::app::MailCacheChange change;
    change.accountId = QStringLiteral("account");
    for (std::size_t index = 0; index < 300; ++index)
    {
        const auto key = QString(4000, QLatin1Char('q')) + QString::number(index);
        change.queryWindows.push_back({.mailboxId = QStringLiteral("inbox"),
                                       .queryKey = key,
                                       .offset = index,
                                       .limit = 100,
                                       .total = 1000});
        change.searchWindows.push_back(
            {.queryKey = key, .offset = index, .limit = 100, .total = 1000});
    }
    publisher.publishImmediately(std::move(change));
    CHECK(mailboxCount == 300);
    CHECK(searchCount == 300);
}
