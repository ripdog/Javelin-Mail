#include "app/RefreshGeneration.h"

#include <catch2/catch_test_macros.hpp>

#include <QUuid>

TEST_CASE("refresh generation rejects a late read after a newer request",
          "[gui][refresh][generation]")
{
    javelin::app::RefreshGeneration refresh{QUuid::createUuid()};
    const auto older = refresh.begin(10);
    const auto newer = refresh.begin(11);

    CHECK_FALSE(refresh.accepts(older, 10));
    CHECK(refresh.accepts(newer, 11));
    CHECK(refresh.install(newer, 11));
    CHECK_FALSE(refresh.install(older, 12));
}

TEST_CASE("refresh generation keeps an older epoch from replacing installed state",
          "[gui][refresh][generation]")
{
    javelin::app::RefreshGeneration refresh{QUuid::createUuid()};
    const auto ticket = refresh.begin(4);

    REQUIRE(refresh.install(ticket, 8));
    CHECK_FALSE(refresh.accepts(ticket, 7));
    CHECK(refresh.accepts(ticket, 8));
    CHECK(refresh.installedEpoch() == 8);

    const auto fenced = refresh.begin(12);
    CHECK_FALSE(refresh.accepts(fenced, 11));
    CHECK(refresh.accepts(fenced, 12));
}

TEST_CASE("refresh generation closes and replaces scopes explicitly", "[gui][refresh][generation]")
{
    javelin::app::RefreshGeneration refresh{QUuid::createUuid()};
    const auto oldScope = refresh.scope();
    const auto ticket = refresh.begin(1);

    refresh.close();
    CHECK_FALSE(refresh.accepts(ticket, 1));

    refresh.replaceScope(QUuid::createUuid());
    CHECK(refresh.scope() != oldScope);
    const auto replacement = refresh.begin(2);
    CHECK(refresh.accepts(replacement, 2));
}
