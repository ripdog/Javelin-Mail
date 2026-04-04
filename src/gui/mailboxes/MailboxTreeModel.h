#pragma once

#include "jmap/cache/QueryService.h"

#include <QAbstractItemModel>

#include <memory>
#include <optional>
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
        };

        explicit MailboxTreeModel(javelin::jmap::cache::QueryService& queryService,
                                  QObject* parent = nullptr);
        ~MailboxTreeModel() override;

        [[nodiscard]] QModelIndex index(int row, int column,
                                        const QModelIndex& parent = QModelIndex{}) const override;
        [[nodiscard]] QModelIndex parent(const QModelIndex& child) const override;
        [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex{}) const override;
        [[nodiscard]] int columnCount(const QModelIndex& parent = QModelIndex{}) const override;
        [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;

        void setAccountId(std::optional<std::string> accountId);

      private:
        struct Node
        {
            javelin::jmap::cache::MailboxTreeItem item;
            Node* parent = nullptr;
            std::vector<std::unique_ptr<Node>> children;
        };

        [[nodiscard]] const Node* nodeForIndex(const QModelIndex& index) const;
        void rebuild();

        javelin::jmap::cache::QueryService& m_queryService;
        std::optional<std::string> m_accountId;
        std::vector<std::unique_ptr<Node>> m_rootNodes;
    };

} // namespace javelin::gui::mailboxes
