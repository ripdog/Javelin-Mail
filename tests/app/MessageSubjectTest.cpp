#include "app/MessageSubject.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

TEST_CASE("subjectForDisplay uses the no-subject label for missing and empty subjects",
          "[app][mail][presentation]")
{
    CHECK(javelin::app::subjectForDisplay(std::nullopt) == QStringLiteral("<No Subject>"));
    CHECK(javelin::app::subjectForDisplay(std::optional<std::string>{std::string{}}) ==
          QStringLiteral("<No Subject>"));
}

TEST_CASE("subjectForDisplay preserves a non-empty subject", "[app][mail][presentation]")
{
    CHECK(javelin::app::subjectForDisplay(std::optional<std::string>{"Quarterly update"}) ==
          QStringLiteral("Quarterly update"));
}
