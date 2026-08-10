#include "gui/mailboxes/MailboxTreeModel.h"

#include <catch2/catch_test_macros.hpp>

namespace
{
    class AccountReader final : public javelin::jmap::cache::AccountReader
    {
      public:
        [[nodiscard]] std::variant<std::vector<javelin::jmap::cache::CachedAccount>,
                                   javelin::jmap::cache::DatabaseError>
        listAll() const override
        {
            return std::vector{javelin::jmap::cache::CachedAccount{
                .accountId = "account-1",
                .name = "Personal",
                .isPersonal = true,
                .isReadOnly = false,
                .isPrimary = true,
                .hasMailCapability = true,
                .mayCreateTopLevelMailbox = true,
                .ownerAccountId = "account-1",
            }};
        }

        [[nodiscard]] std::variant<std::vector<javelin::jmap::cache::CachedAccount>,
                                   javelin::jmap::cache::DatabaseError>
        listOwnedBy(std::string_view ownerAccountId) const override
        {
            if (ownerAccountId != "account-1")
                return std::vector<javelin::jmap::cache::CachedAccount>{};
            return std::get<std::vector<javelin::jmap::cache::CachedAccount>>(listAll());
        }

        [[nodiscard]] std::variant<std::optional<javelin::jmap::cache::CachedAccount>,
                                   javelin::jmap::cache::DatabaseError>
        findById(std::string_view accountId) const override
        {
            if (accountId != "account-1")
                return std::optional<javelin::jmap::cache::CachedAccount>{};
            auto accounts = std::get<std::vector<javelin::jmap::cache::CachedAccount>>(listAll());
            return std::optional{accounts.front()};
        }
    };

    class MailboxReader final : public javelin::jmap::cache::MailboxReader
    {
      public:
        [[nodiscard]] std::variant<std::vector<javelin::jmap::cache::MailboxTreeItem>,
                                   javelin::jmap::cache::DatabaseError>
        listMailboxTree(std::string_view accountId) const override
        {
            if (accountId != "account-1")
                return std::vector<javelin::jmap::cache::MailboxTreeItem>{};
            return std::vector{javelin::jmap::cache::MailboxTreeItem{
                .id = "pending-mailbox:creation-1",
                .name = "Projects",
                .parentId = std::nullopt,
                .role = std::nullopt,
                .sortOrder = 0,
                .totalEmails = 0,
                .unreadEmails = 0,
                .totalThreads = 0,
                .unreadThreads = 0,
                .isSubscribed = true,
                .myRights = {},
                .hasChildren = false,
                .pendingCreate = true,
            }};
        }
    };
} // namespace

TEST_CASE("pending mailbox creation is visible but noninteractive",
          "[gui][mailbox][create][optimistic]")
{
    AccountReader accounts;
    MailboxReader mailboxes;
    javelin::gui::mailboxes::MailboxTreeModel model{accounts, mailboxes};

    REQUIRE(model.rowCount() == 1);
    const auto account = model.index(0, 0);
    REQUIRE(account.isValid());
    REQUIRE(model.rowCount(account) == 1);
    const auto pending = model.index(0, 0, account);
    REQUIRE(pending.isValid());
    CHECK(pending.data(Qt::DisplayRole).toString() == QStringLiteral("Projects (creating…)"));
    CHECK(
        pending.data(javelin::gui::mailboxes::MailboxTreeModel::MailboxPendingCreateRole).toBool());
    const auto flags = model.flags(pending);
    CHECK_FALSE(flags.testFlag(Qt::ItemIsEnabled));
    CHECK_FALSE(flags.testFlag(Qt::ItemIsSelectable));
    CHECK_FALSE(flags.testFlag(Qt::ItemIsDropEnabled));
}
