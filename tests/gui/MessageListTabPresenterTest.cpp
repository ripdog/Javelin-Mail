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

TEST_CASE("message list presentation preserves mailbox page metadata")
{
    javelin::gui::messages::MessageListPageHeader page{};
    page.title = QStringLiteral("Inbox — Personal");
    page.offset = 100;
    page.position = 100;
    page.itemCount = 18;
    page.returnedLimit = 100;
    page.total = 218;

    MessageListPresentationInput input{};
    input.tabKind = TabKind::Mailbox;
    input.title = page.title;
    input.itemCount = 18;
    input.refreshError = QStringLiteral("offline");
    input.refreshInFlight = true;
    input.page = page;
    const auto plan = planMessageListPresentation(input);

    CHECK(plan.emptyState.refreshError == QStringLiteral("offline"));
    CHECK(plan.emptyState.refreshInFlight);
    const auto& header = std::get<javelin::gui::messages::MessageListPageHeader>(plan.header);
    CHECK(header.offset == 100);
    CHECK(header.total == std::optional<std::size_t>{218});
    CHECK_FALSE(header.search);
    CHECK(header.refreshInFlight);
}

TEST_CASE("local search presentation selects the indexed empty state")
{
    javelin::gui::messages::MessageListPageHeader page{};
    page.title = QStringLiteral("Search: needle");
    page.search = true;
    page.indexedSearch = true;
    page.canSearchServer = true;

    MessageListPresentationInput input{};
    input.tabKind = TabKind::Search;
    input.title = page.title;
    input.localSearch = true;
    input.page = page;
    const auto plan = planMessageListPresentation(input);

    CHECK(plan.emptyState.collection == javelin::gui::messages::MessageCollectionKind::LocalSearch);
    const auto& header = std::get<javelin::gui::messages::MessageListPageHeader>(plan.header);
    CHECK(header.indexedSearch);
    CHECK(header.canSearchServer);
}
