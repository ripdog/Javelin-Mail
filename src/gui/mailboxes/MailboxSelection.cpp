#include "gui/mailboxes/MailboxSelection.h"

#include "gui/mailboxes/MailboxTreeModel.h"

#include <QAbstractItemModel>

namespace javelin::gui::mailboxes
{
    QModelIndex findMailboxIndexForSelection(const QAbstractItemModel& model,
                                             const QString& accountId,
                                             const std::optional<QString>& mailboxId,
                                             const QModelIndex& parent)
    {
        const int rowCount = model.rowCount(parent);
        for (int row = 0; row < rowCount; ++row)
        {
            const QModelIndex index = model.index(row, 0, parent);
            if (!index.isValid())
                continue;

            const QString indexAccountId = index.data(MailboxTreeModel::AccountIdRole).toString();
            const QString indexMailboxId = index.data(MailboxTreeModel::MailboxIdRole).toString();
            const bool accountMatches = indexAccountId == accountId;
            const bool mailboxMatches =
                mailboxId.has_value() ? indexMailboxId == *mailboxId : indexMailboxId.isEmpty();
            if (accountMatches && mailboxMatches)
                return index;

            const QModelIndex childMatch =
                findMailboxIndexForSelection(model, accountId, mailboxId, index);
            if (childMatch.isValid())
                return childMatch;
        }

        return {};
    }

    QModelIndex findMailboxIndexForRole(const QAbstractItemModel& model, const QString& accountId,
                                        const QString& role, const QModelIndex& parent)
    {
        const int rowCount = model.rowCount(parent);
        for (int row = 0; row < rowCount; ++row)
        {
            const QModelIndex index = model.index(row, 0, parent);
            if (!index.isValid())
                continue;

            const QString indexAccountId = index.data(MailboxTreeModel::AccountIdRole).toString();
            const QString indexMailboxId = index.data(MailboxTreeModel::MailboxIdRole).toString();
            const QString indexRole = index.data(MailboxTreeModel::MailboxRoleRole).toString();
            if (indexAccountId == accountId && !indexMailboxId.isEmpty() && indexRole == role)
                return index;

            const QModelIndex childMatch = findMailboxIndexForRole(model, accountId, role, index);
            if (childMatch.isValid())
                return childMatch;
        }

        return {};
    }
} // namespace javelin::gui::mailboxes
