#include "jmap/JmapCore.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("JmapCore exposes a non-empty status summary", "[jmap]")
{
    const javelin::jmap::JmapCore core;

    CHECK_FALSE(core.statusSummary().isEmpty());
    CHECK(core.statusSummary().contains(QStringLiteral("JMAP")));
}
