#pragma once

#include <QModelIndex>
#include <QString>

#include <optional>

class QAbstractItemModel;

namespace javelin::gui::mailboxes
{
    [[nodiscard]] QModelIndex findMailboxIndexForSelection(const QAbstractItemModel& model,
                                                           const QString& accountId,
                                                           const std::optional<QString>& mailboxId,
                                                           const QModelIndex& parent = {});
    [[nodiscard]] QModelIndex findMailboxIndexForRole(const QAbstractItemModel& model,
                                                      const QString& accountId, const QString& role,
                                                      const QModelIndex& parent = {});
} // namespace javelin::gui::mailboxes
