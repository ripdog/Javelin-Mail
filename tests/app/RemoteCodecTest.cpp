#include "app/RemoteCodec.h"
#include "app/RemoteActionTypes.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace
{
    struct CodecFixture
    {
        QString title;
        QByteArray bytes;
        std::vector<std::string> identifiers;
        std::map<std::string, std::optional<std::size_t>> totals;
        std::variant<int, QString> status;
        std::chrono::milliseconds delay{};
    };
} // namespace

TEST_CASE("remote codec round-trips nested typed values", "[app][remote-codec]")
{
    const CodecFixture fixture{
        .title = QStringLiteral("Mailbox window"),
        .bytes = QByteArray{"binary\0payload", 14},
        .identifiers = {"account-1", "mailbox-1"},
        .totals = {{"known", 42}, {"unknown", std::nullopt}},
        .status = QStringLiteral("ready"),
        .delay = std::chrono::milliseconds{275},
    };

    const auto encoded = javelin::app::remote::encode(fixture);
    const auto* payload = std::get_if<QByteArray>(&encoded);
    REQUIRE(payload != nullptr);

    const auto decoded = javelin::app::remote::decodeValue<CodecFixture>(*payload);
    const auto* value = std::get_if<CodecFixture>(&decoded);
    REQUIRE(value != nullptr);
    CHECK(value->title == fixture.title);
    CHECK(value->bytes == fixture.bytes);
    CHECK(value->identifiers == fixture.identifiers);
    CHECK(value->totals == fixture.totals);
    CHECK(value->status == fixture.status);
    CHECK(value->delay == fixture.delay);
}

TEST_CASE("remote codec preserves structured undo failures", "[app][remote-codec][undo]")
{
    const javelin::app::RemoteUndoExecutionResult result{
        .succeeded = false,
        .completedEntryId = std::nullopt,
        .refreshScope = std::nullopt,
        .failure =
            javelin::app::undo::HistoryFailure{
                .entryId = QStringLiteral("history-1"),
                .actionLabel = QStringLiteral("Move message"),
                .summary = QStringLiteral("The message is no longer in Archive."),
                .objectFailures = {{.objectId = QStringLiteral("email-1"),
                                    .summary = QStringLiteral("Object not found")}},
                .mayRemoveFromHistory = true,
                .acknowledgeAndRemove = true,
            },
    };

    const auto encoded = javelin::app::remote::encode(result);
    const auto* payload = std::get_if<QByteArray>(&encoded);
    REQUIRE(payload != nullptr);

    const auto decoded =
        javelin::app::remote::decodeValue<javelin::app::RemoteUndoExecutionResult>(*payload);
    const auto* value = std::get_if<javelin::app::RemoteUndoExecutionResult>(&decoded);
    REQUIRE(value != nullptr);
    CHECK_FALSE(value->succeeded);
    REQUIRE(value->failure.has_value());
    CHECK(value->failure->entryId == QStringLiteral("history-1"));
    CHECK(value->failure->summary == QStringLiteral("The message is no longer in Archive."));
    REQUIRE(value->failure->objectFailures.size() == 1);
    CHECK(value->failure->objectFailures.front().objectId == QStringLiteral("email-1"));
    CHECK(value->failure->acknowledgeAndRemove);
}

TEST_CASE("remote codec rejects trailing data", "[app][remote-codec]")
{
    auto encoded = javelin::app::remote::encode(QStringLiteral("value"));
    auto* payload = std::get_if<QByteArray>(&encoded);
    REQUIRE(payload != nullptr);
    payload->append('x');

    const auto decoded = javelin::app::remote::decodeValue<QString>(*payload);
    CHECK(std::holds_alternative<javelin::app::remote::CodecError>(decoded));
}
