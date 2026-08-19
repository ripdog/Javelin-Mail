#include "gui/shell/MessageListPresentationPolicy.h"

#include <catch2/catch_test_macros.hpp>

using namespace javelin::gui::shell;

TEST_CASE("message list presentation shows no header without a tab")
{
    MessageListPresentationInput input{};
    input.itemCount = 3;
    const auto plan = planMessageListPresentation(input);

    CHECK(plan.emptyState.itemCount == 3);
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

    CHECK(plan.emptyState.kind == javelin::gui::messages::MessageListEmptyStateKind::RefreshFailed);
    CHECK(plan.emptyState.action == javelin::gui::messages::MessageListEmptyAction::Retry);
    CHECK(plan.emptyState.detail == QStringLiteral("offline"));
    const auto& header = std::get<javelin::gui::messages::MessageListHeader>(plan.header);
    CHECK(header.itemCount == 118);
    CHECK(header.total == std::optional<std::size_t>{218});
    CHECK_FALSE(header.search);
    CHECK(header.refreshInFlight);
    CHECK(header.loadMoreInFlight);
}

TEST_CASE("mailbox empty states distinguish filters connectivity authentication and first load")
{
    using javelin::gui::messages::MessageListEmptyAction;
    using javelin::gui::messages::MessageListEmptyStateKind;

    MessageListPresentationInput input{};
    input.tabKind = TabKind::Mailbox;
    input.cacheLoaded = true;

    auto plan = planMessageListPresentation(input);
    CHECK(plan.emptyState.kind == MessageListEmptyStateKind::EmptyMailbox);
    CHECK(plan.emptyState.action == MessageListEmptyAction::None);

    input.quickFilterActive = true;
    plan = planMessageListPresentation(input);
    CHECK(plan.emptyState.kind == MessageListEmptyStateKind::NoFilterMatches);
    CHECK(plan.emptyState.action == MessageListEmptyAction::ClearFilters);

    input.quickFilterActive = false;
    input.accountStatus = javelin::app::MailAccountStatus::Disconnected;
    plan = planMessageListPresentation(input);
    CHECK(plan.emptyState.kind == MessageListEmptyStateKind::EmptyMailbox);

    input.cacheLoaded = false;
    plan = planMessageListPresentation(input);
    CHECK(plan.emptyState.kind == MessageListEmptyStateKind::Disconnected);
    CHECK(plan.emptyState.action == MessageListEmptyAction::Retry);

    input.accountStatus = javelin::app::MailAccountStatus::AuthenticationPaused;
    plan = planMessageListPresentation(input);
    CHECK(plan.emptyState.kind == MessageListEmptyStateKind::AuthenticationRequired);
    CHECK(plan.emptyState.action == MessageListEmptyAction::SignInAgain);

    input.accountStatus = javelin::app::MailAccountStatus::Connected;
    plan = planMessageListPresentation(input);
    CHECK(plan.emptyState.kind == MessageListEmptyStateKind::NotYetLoaded);
    CHECK(plan.emptyState.action == MessageListEmptyAction::Retry);

    input.refreshInFlight = true;
    plan = planMessageListPresentation(input);
    CHECK(plan.emptyState.kind == MessageListEmptyStateKind::Loading);
    CHECK(plan.emptyState.action == MessageListEmptyAction::None);
}

TEST_CASE("query failure outranks ordinary empty results but authentication is more specific")
{
    using javelin::gui::messages::MessageListEmptyAction;
    using javelin::gui::messages::MessageListEmptyStateKind;

    MessageListPresentationInput input{};
    input.tabKind = TabKind::Search;
    input.cacheLoaded = true;
    input.refreshError = QStringLiteral("server rejected query");
    input.accountStatus = javelin::app::MailAccountStatus::Connected;

    auto plan = planMessageListPresentation(input);
    CHECK(plan.emptyState.kind == MessageListEmptyStateKind::RefreshFailed);
    CHECK(plan.emptyState.action == MessageListEmptyAction::Retry);
    CHECK(plan.emptyState.detail == QStringLiteral("server rejected query"));

    input.accountStatus = javelin::app::MailAccountStatus::AuthenticationPaused;
    plan = planMessageListPresentation(input);
    CHECK(plan.emptyState.kind == MessageListEmptyStateKind::AuthenticationRequired);
    CHECK(plan.emptyState.action == MessageListEmptyAction::SignInAgain);
}

TEST_CASE("online search empty state offers editing the search")
{
    MessageListPresentationInput input{};
    input.tabKind = TabKind::Search;
    input.cacheLoaded = true;
    input.localSearch = false;

    const auto plan = planMessageListPresentation(input);
    CHECK(plan.emptyState.kind ==
          javelin::gui::messages::MessageListEmptyStateKind::NoSearchResults);
    CHECK(plan.emptyState.action == javelin::gui::messages::MessageListEmptyAction::EditSearch);
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
    input.canSearchServer = true;
    input.cacheLoaded = true;
    input.list = list;
    const auto plan = planMessageListPresentation(input);

    CHECK(plan.emptyState.kind ==
          javelin::gui::messages::MessageListEmptyStateKind::NoLocalSearchResults);
    CHECK(plan.emptyState.action == javelin::gui::messages::MessageListEmptyAction::SearchServer);
    const auto& header = std::get<javelin::gui::messages::MessageListHeader>(plan.header);
    CHECK(header.indexedSearch);
    CHECK(header.canSearchServer);
}
