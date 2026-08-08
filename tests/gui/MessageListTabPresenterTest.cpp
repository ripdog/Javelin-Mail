#include "gui/shell/MessageListPresentationPolicy.h"

#include <catch2/catch_test_macros.hpp>

using namespace javelin::gui::shell;

TEST_CASE("message list presentation shows no header without a tab")
{
    MessageListPresentationInput input{};
    input.itemCount = 3;
    const auto plan = planMessageListPresentation(input);

    CHECK(plan.emptyState.itemCount == 3);
    CHECK(plan.emptyState.collection == javelin::gui::messages::MessageCollectionKind::Mailbox);
    CHECK(std::holds_alternative<std::monostate>(plan.header));
}

TEST_CASE("message list presentation labels non-message tabs")
{
    MessageListPresentationInput input{};
    input.tabKind = TabKind::Compose;
    input.title = QStringLiteral("Reply to Ada");
    const auto plan = planMessageListPresentation(input);

    const auto& header = std::get<javelin::gui::messages::MessageListContextHeader>(plan.header);
    CHECK(header.title == QStringLiteral("Reply to Ada"));
    CHECK(header.context == QStringLiteral("Compose"));
}

TEST_CASE("message list presentation preserves aggregate mailbox metadata")
{
    javelin::gui::messages::MessageListHeader list{};
    list.title = QStringLiteral("Inbox — Personal");
    list.itemCount = 118;
    list.total = 218;
    list.loadMoreInFlight = true;

    MessageListPresentationInput input{};
    input.tabKind = TabKind::Mailbox;
    input.title = list.title;
    input.itemCount = 118;
    input.refreshError = QStringLiteral("offline");
    input.refreshInFlight = true;
    input.list = list;
    const auto plan = planMessageListPresentation(input);

    CHECK(plan.emptyState.refreshError == QStringLiteral("offline"));
    CHECK(plan.emptyState.refreshInFlight);
    const auto& header = std::get<javelin::gui::messages::MessageListHeader>(plan.header);
    CHECK(header.itemCount == 118);
    CHECK(header.total == std::optional<std::size_t>{218});
    CHECK_FALSE(header.search);
    CHECK(header.refreshInFlight);
    CHECK(header.loadMoreInFlight);
}

TEST_CASE("local search presentation selects the indexed empty state")
{
    javelin::gui::messages::MessageListHeader list{};
    list.title = QStringLiteral("Search: needle");
    list.search = true;
    list.indexedSearch = true;
    list.canSearchServer = true;

    MessageListPresentationInput input{};
    input.tabKind = TabKind::Search;
    input.title = list.title;
    input.localSearch = true;
    input.list = list;
    const auto plan = planMessageListPresentation(input);

    CHECK(plan.emptyState.collection == javelin::gui::messages::MessageCollectionKind::LocalSearch);
    const auto& header = std::get<javelin::gui::messages::MessageListHeader>(plan.header);
    CHECK(header.indexedSearch);
    CHECK(header.canSearchServer);
}
