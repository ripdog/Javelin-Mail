#include "app/AccountCredentialStore.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <variant>

TEST_CASE("memory account credential store round-trips and removes secrets", "[app][credentials]")
{
    javelin::app::MemoryAccountCredentialStore store;
    const auto connectionId = QStringLiteral("connection-1");
    const javelin::app::AccountCredentialSecrets expected{
        .accessToken = QStringLiteral("access-token"),
        .refreshToken = QStringLiteral("refresh-token"),
        .registrationAccessToken = QStringLiteral("registration-token"),
    };

    REQUIRE_FALSE(store.store(connectionId, expected).has_value());
    const auto loaded = store.load(connectionId);
    const auto* credentials =
        std::get_if<std::optional<javelin::app::AccountCredentialSecrets>>(&loaded);
    REQUIRE(credentials != nullptr);
    REQUIRE(credentials->has_value());
    CHECK(**credentials == expected);

    REQUIRE_FALSE(store.remove(connectionId).has_value());
    const auto removed = store.load(connectionId);
    const auto* missing =
        std::get_if<std::optional<javelin::app::AccountCredentialSecrets>>(&removed);
    REQUIRE(missing != nullptr);
    CHECK_FALSE(missing->has_value());
}

TEST_CASE("storing empty credentials clears an existing account entry", "[app][credentials]")
{
    javelin::app::MemoryAccountCredentialStore store;
    const auto connectionId = QStringLiteral("connection-1");
    REQUIRE_FALSE(store
                      .store(connectionId, {.accessToken = QStringLiteral("access-token"),
                                            .refreshToken = {},
                                            .registrationAccessToken = {}})
                      .has_value());
    REQUIRE_FALSE(store.store(connectionId, {}).has_value());

    const auto loaded = store.load(connectionId);
    const auto* credentials =
        std::get_if<std::optional<javelin::app::AccountCredentialSecrets>>(&loaded);
    REQUIRE(credentials != nullptr);
    CHECK_FALSE(credentials->has_value());
}
