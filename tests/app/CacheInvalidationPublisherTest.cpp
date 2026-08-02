#include "app/CacheInvalidationPublisher.h"

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
            .offset = 0,
            .limit = 50,
            .total = 100,
        }},
        .searchWindows = {},
        .mailboxTreeChanged = true,
        .hasNewMail = false,
        .optimisticProjection = false,
        .contactsChanged = true,
    });
    publisher.publish(javelin::app::MailCacheChange{
        .accountId = QStringLiteral("account-a"),
        .mailboxIds = {QStringLiteral("mailbox-b")},
        .queryWindows = {},
        .searchWindows = {},
        .mailboxTreeChanged = false,
        .hasNewMail = true,
        .optimisticProjection = true,
    });

    publisher.flush();

    REQUIRE(invalidations.size() == 1);
    const auto& invalidation = invalidations.front();
    CHECK(invalidation.epoch == 1);
    CHECK(publisher.currentEpoch() == 1);
    CHECK(invalidation.change.mailboxTreeChanged);
    CHECK(invalidation.change.hasNewMail);
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
        .hasNewMail = false,
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
        .hasNewMail = false,
        .optimisticProjection = false,
    });

    publisher.flush();

    REQUIRE(invalidations.size() == 2);
    CHECK(invalidations[0].epoch == 1);
    CHECK(invalidations[1].epoch == 2);
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
        .hasNewMail = false,
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
