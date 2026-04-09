#pragma once

#include "jmap/cache/AccountRepository.h"
#include "jmap/cache/QueryService.h"

#include <QAbstractItemModel>

#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace javelin::gui::mailboxes
{

    class MailboxTreeModel : public QAbstractItemModel
    {
        Q_OBJECT

      public:
        enum Roles
        {
            MailboxIdRole = Qt::UserRole + 1,
            AccountIdRole = Qt::UserRole + 2,
            TotalThreadsRole = Qt::UserRole + 3,
        };

        explicit MailboxTreeModel(javelin::jmap::cache::AccountRepository& accountRepository,
                                  javelin::jmap::cache::QueryService& queryService,
                                  QObject* parent = nullptr);
        ~MailboxTreeModel() override;

        [[nodiscard]] QModelIndex index(int row, int column,
                                        const QModelIndex& parent = QModelIndex{}) const override;
        [[nodiscard]] QModelIndex parent(const QModelIndex& child) const override;
        [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex{}) const override;
        [[nodiscard]] int columnCount(const QModelIndex& parent = QModelIndex{}) const override;
        [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;

        void refresh();

      private:
        // Each node represents either an account (mailboxId empty, children = mailboxes)
        // or a mailbox. accountId is always set on every node.
        struct Node
        {
            std::string accountId;
            std::string displayName;
            std::string mailboxId; // empty for account-level nodes
            std::uint64_t unreadEmails = 0;
            std::uint64_t totalThreads = 0;
            Node* parent = nullptr;
            std::vector<std::unique_ptr<Node>> children;
        };

        [[nodiscard]] const Node* nodeForIndex(const QModelIndex& index) const;
        void rebuild();

        javelin::jmap::cache::AccountRepository& m_accountRepository;
        javelin::jmap::cache::QueryService& m_queryService;
        std::vector<std::unique_ptr<Node>> m_rootNodes;
    };

} // namespace javelin::gui::mailboxes
