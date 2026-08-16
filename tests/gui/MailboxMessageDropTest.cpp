#include "gui/mailboxes/MailboxTreeModel.h"
#include "gui/messages/MessageDragPayload.h"

#include <catch2/catch_test_macros.hpp>

#include <QMimeData>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace
{
    class AccountReader final : public javelin::jmap::cache::AccountReader
    {
      public:
        [[nodiscard]] std::variant<std::vector<javelin::jmap::cache::CachedAccount>,
                                   javelin::jmap::cache::DatabaseError>
        listAll() const override
        {
            return std::vector{
                account("source-account", "connection-a", "u1", true),
                account("destination-account", "connection-b", "u1", false),
            };
        }

        [[nodiscard]] std::variant<std::vector<javelin::jmap::cache::CachedAccount>,
                                   javelin::jmap::cache::DatabaseError>
        listOwnedBy(std::string_view) const override
        {
            return std::get<std::vector<javelin::jmap::cache::CachedAccount>>(listAll());
        }

        [[nodiscard]] std::variant<std::optional<javelin::jmap::cache::CachedAccount>,
                                   javelin::jmap::cache::DatabaseError>
        findById(std::string_view accountId) const override
        {
            auto accounts = std::get<std::vector<javelin::jmap::cache::CachedAccount>>(listAll());
            const auto found = std::ranges::find(accounts, accountId,
                                                 &javelin::jmap::cache::CachedAccount::accountId);
            return found == accounts.end() ? std::optional<javelin::jmap::cache::CachedAccount>{}
                                           : std::optional{*found};
        }

      private:
        [[nodiscard]] static javelin::jmap::cache::CachedAccount
        account(std::string id, std::string connectionId, std::string remoteId, bool primary)
        {
            return {
                .accountId = std::move(id),
                .connectionId = std::move(connectionId),
                .remoteAccountId = std::move(remoteId),
                .name = primary ? "Source" : "Destination",
                .isPersonal = true,
                .isReadOnly = false,
                .isPrimary = primary,
                .hasMailCapability = true,
                .mayCreateTopLevelMailbox = true,
                .ownerAccountId = {},
                .hasSubmissionCapability = false,
                .maxDelayedSendSeconds = 0,
            };
        }
    };

    class MailboxReader final : public javelin::jmap::cache::MailboxReader
    {
      public:
        [[nodiscard]] std::variant<std::vector<javelin::jmap::cache::MailboxTreeItem>,
                                   javelin::jmap::cache::DatabaseError>
        listMailboxTree(std::string_view accountId) const override
        {
            if (accountId == "source-account")
                return std::vector{mailbox("inbox", "Inbox", true)};
            if (accountId == "destination-account")
            {
                return std::vector{
                    mailbox("archive", "Archive", true),
                    mailbox("locked", "Locked", false),
                };
            }
            return std::vector<javelin::jmap::cache::MailboxTreeItem>{};
        }

      private:
        [[nodiscard]] static javelin::jmap::cache::MailboxTreeItem
        mailbox(std::string id, std::string name, bool writable)
        {
            return {
                .id = std::move(id),
                .name = std::move(name),
                .parentId = std::nullopt,
                .role = std::nullopt,
                .sortOrder = 0,
                .totalEmails = 0,
                .unreadEmails = 0,
                .totalThreads = 0,
                .unreadThreads = 0,
                .isSubscribed = true,
                .myRights =
                    {
                        .mayReadItems = true,
                        .mayAddItems = writable,
                        .mayRemoveItems = true,
                        .maySetSeen = true,
                        .maySetKeywords = true,
                        .mayCreateChild = true,
                        .mayRename = true,
                        .mayDelete = true,
                        .maySubmit = true,
                    },
                .hasChildren = false,
                .pendingCreate = false,
            };
        }
    };

    [[nodiscard]] QModelIndex accountIndex(const javelin::gui::mailboxes::MailboxTreeModel& model,
                                           const QString& accountId)
    {
        for (int row = 0; row < model.rowCount(); ++row)
        {
            const auto index = model.index(row, 0);
            if (index.data(javelin::gui::mailboxes::MailboxTreeModel::AccountIdRole).toString() ==
                    accountId &&
                index.data(javelin::gui::mailboxes::MailboxTreeModel::MailboxIdRole)
                    .toString()
                    .isEmpty())
                return index;
        }
        return {};
    }

    [[nodiscard]] QModelIndex mailboxIndex(const javelin::gui::mailboxes::MailboxTreeModel& model,
                                           const QModelIndex& account, const QString& mailboxId)
    {
        for (int row = 0; row < model.rowCount(account); ++row)
        {
            const auto index = model.index(row, 0, account);
            if (index.data(javelin::gui::mailboxes::MailboxTreeModel::MailboxIdRole).toString() ==
                mailboxId)
                return index;
        }
        return {};
    }

    [[nodiscard]] std::unique_ptr<QMimeData> mimeData()
    {
        auto mime = std::make_unique<QMimeData>();
        mime->setData(
            QString::fromLatin1(javelin::gui::messages::messageDragMimeType),
            javelin::gui::messages::encodeMessageDragPayload({
                .sourceAccountId = "source-account",
                .sourceMailboxId = std::optional<std::string>{"inbox"},
                .selection = {javelin::app::SelectedCollapsedThread{.threadId = "thread-1"},
                              javelin::app::SelectedEmail{.emailId = "email-2"}},
            }));
        return mime;
    }
} // namespace

TEST_CASE("mailbox tree accepts Move and Copy only on writable mailboxes",
          "[gui][mailbox][drag][mail-transfer]")
{
    AccountReader accounts;
    MailboxReader mailboxes;
    javelin::gui::mailboxes::MailboxTreeModel model{accounts, mailboxes};
    const auto destinationAccount = accountIndex(model, QStringLiteral("destination-account"));
    REQUIRE(destinationAccount.isValid());
    const auto archive = mailboxIndex(model, destinationAccount, QStringLiteral("archive"));
    const auto locked = mailboxIndex(model, destinationAccount, QStringLiteral("locked"));
    REQUIRE(archive.isValid());
    REQUIRE(locked.isValid());

    const auto mime = mimeData();
    CHECK(model.flags(archive).testFlag(Qt::ItemIsDropEnabled));
    CHECK_FALSE(model.flags(locked).testFlag(Qt::ItemIsDropEnabled));
    CHECK(model.supportedDropActions().testFlag(Qt::MoveAction));
    CHECK(model.supportedDropActions().testFlag(Qt::CopyAction));
    CHECK(model.canDropMimeData(mime.get(), Qt::MoveAction, -1, 0, archive));
    CHECK(model.canDropMimeData(mime.get(), Qt::CopyAction, -1, 0, archive));
    CHECK_FALSE(model.canDropMimeData(mime.get(), Qt::MoveAction, -1, 0, locked));
    CHECK_FALSE(model.canDropMimeData(mime.get(), Qt::CopyAction, -1, 0, locked));
}

TEST_CASE("mailbox drop emits stable selection and Qt copy action across accounts",
          "[gui][mailbox][drag][mail-transfer]")
{
    AccountReader accounts;
    MailboxReader mailboxes;
    javelin::gui::mailboxes::MailboxTreeModel model{accounts, mailboxes};
    const auto destinationAccount = accountIndex(model, QStringLiteral("destination-account"));
    REQUIRE(destinationAccount.isValid());
    const auto archive = mailboxIndex(model, destinationAccount, QStringLiteral("archive"));
    REQUIRE(archive.isValid());

    std::optional<javelin::gui::messages::MessageDragPayload> emittedPayload;
    QString emittedAccount;
    QString emittedMailbox;
    Qt::DropAction emittedAction = Qt::IgnoreAction;
    QObject::connect(&model, &javelin::gui::mailboxes::MailboxTreeModel::emailsDropped, &model,
                     [&](const javelin::gui::messages::MessageDragPayload& payload,
                         const QString& destinationAccountId, const QString& destinationMailboxId,
                         const Qt::DropAction action)
                     {
                         emittedPayload = payload;
                         emittedAccount = destinationAccountId;
                         emittedMailbox = destinationMailboxId;
                         emittedAction = action;
                     });

    const auto mime = mimeData();
    REQUIRE(model.dropMimeData(mime.get(), Qt::CopyAction, -1, 0, archive));
    REQUIRE(emittedPayload.has_value());
    CHECK(emittedPayload->sourceAccountId == "source-account");
    CHECK(emittedPayload->sourceMailboxId == std::optional<std::string>{"inbox"});
    REQUIRE(emittedPayload->selection.size() == 2);
    CHECK(
        std::get<javelin::app::SelectedCollapsedThread>(emittedPayload->selection.at(0)).threadId ==
        "thread-1");
    CHECK(std::get<javelin::app::SelectedEmail>(emittedPayload->selection.at(1)).emailId ==
          "email-2");
    CHECK(emittedAccount == QStringLiteral("destination-account"));
    CHECK(emittedMailbox == QStringLiteral("archive"));
    CHECK(emittedAction == Qt::CopyAction);
}

TEST_CASE("mailbox drop rejects malformed transfer payload", "[gui][mailbox][drag][mail-transfer]")
{
    AccountReader accounts;
    MailboxReader mailboxes;
    javelin::gui::mailboxes::MailboxTreeModel model{accounts, mailboxes};
    const auto destinationAccount = accountIndex(model, QStringLiteral("destination-account"));
    REQUIRE(destinationAccount.isValid());
    const auto archive = mailboxIndex(model, destinationAccount, QStringLiteral("archive"));
    REQUIRE(archive.isValid());

    QMimeData malformed;
    malformed.setData(QString::fromLatin1(javelin::gui::messages::messageDragMimeType),
                      QByteArrayLiteral("not-a-valid-payload"));
    CHECK_FALSE(model.canDropMimeData(&malformed, Qt::MoveAction, -1, 0, archive));
    CHECK_FALSE(model.dropMimeData(&malformed, Qt::MoveAction, -1, 0, archive));
}
