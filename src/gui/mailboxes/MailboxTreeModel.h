#pragma once

#include "jmap/cache/AccountRepository.h"
#include "jmap/cache/QueryService.h"

#include <QAbstractItemModel>
#include <QStringList>
#include <QStringView>

#include <memory>
#include <optional>
#include <unordered_map>
#include <variant>
#include <vector>

namespace javelin::gui::mailboxes
{

    class MailboxTreeModel : public QAbstractItemModel
    {
        Q_OBJECT

      public:
        struct Options
        {
            std::optional<std::string> accountId;
            bool showAccount = true;
            bool checkable = false;
            QStringList checkedMailboxIds;
        };

        enum Roles
        {
            MailboxIdRole = Qt::UserRole + 1,
            AccountIdRole = Qt::UserRole + 2,
            TotalThreadsRole = Qt::UserRole + 3,
            MailboxRoleRole = Qt::UserRole + 4,
            ConnectionStatusRole = Qt::UserRole + 5,
        };

        enum class ConnectionStatus
        {
            Disconnected,
            Connecting,
            Connected,
            AuthenticationPaused,
        };

        explicit MailboxTreeModel(javelin::jmap::cache::AccountRepository& accountRepository,
                                  javelin::jmap::cache::QueryService& queryService,
                                  QObject* parent = nullptr);
        MailboxTreeModel(javelin::jmap::cache::AccountRepository& accountRepository,
                         javelin::jmap::cache::QueryService& queryService, Options options,
                         QObject* parent = nullptr);
        ~MailboxTreeModel() override;

        [[nodiscard]] QModelIndex index(int row, int column,
                                        const QModelIndex& parent = QModelIndex{}) const override;
        [[nodiscard]] QModelIndex parent(const QModelIndex& child) const override;
        [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex{}) const override;
        [[nodiscard]] int columnCount(const QModelIndex& parent = QModelIndex{}) const override;
        [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
        [[nodiscard]] bool setData(const QModelIndex& index, const QVariant& value,
                                   int role) override;
        [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
        [[nodiscard]] QStringList mimeTypes() const override;
        [[nodiscard]] bool canDropMimeData(const QMimeData* data, Qt::DropAction action, int row,
                                           int column, const QModelIndex& parent) const override;
        [[nodiscard]] bool dropMimeData(const QMimeData* data, Qt::DropAction action, int row,
                                        int column, const QModelIndex& parent) override;
        [[nodiscard]] Qt::DropActions supportedDropActions() const override;

        void refresh();
        void setAccountId(std::optional<std::string> accountId);
        void setCheckedMailboxIds(QStringList mailboxIds);
        [[nodiscard]] QStringList checkedMailboxIds() const;
        void setConnectionStatus(QStringView accountId, ConnectionStatus status);

      Q_SIGNALS:
        void emailsDropped(const QString& sourceAccountId, const QString& destinationAccountId,
                           const QString& destinationMailboxId, const QStringList& emailIds);

      private:
        // Each node represents either an account (mailboxId empty, children = mailboxes)
        // or a mailbox. accountId is always set on every node.
        struct Node
        {
            enum class Kind
            {
                Account,
                Mailbox,
                Separator,
            };

            Kind kind = Kind::Mailbox;
            std::string accountId;
            std::string displayName;
            std::string mailboxId; // empty for account-level nodes
            std::optional<std::string> role;
            std::uint64_t unreadEmails = 0;
            std::uint64_t totalThreads = 0;
            bool checked = false;
            Node* parent = nullptr;
            std::vector<std::unique_ptr<Node>> children;
        };

        [[nodiscard]] const Node* nodeForIndex(const QModelIndex& index) const;
        [[nodiscard]] static bool mailboxNameLess(const std::unique_ptr<Node>& left,
                                                  const std::unique_ptr<Node>& right);
        void rebuild();

        javelin::jmap::cache::AccountRepository& m_accountRepository;
        javelin::jmap::cache::QueryService& m_queryService;
        Options m_options;
        std::vector<std::unique_ptr<Node>> m_rootNodes;
        std::unordered_map<std::string, ConnectionStatus> m_connectionStatuses;
    };

} // namespace javelin::gui::mailboxes
