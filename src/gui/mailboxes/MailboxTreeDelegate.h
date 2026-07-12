#pragma once

#include <QStyledItemDelegate>

namespace javelin::gui::mailboxes
{
    class MailboxTreeDelegate final : public QStyledItemDelegate
    {
      public:
        using QStyledItemDelegate::QStyledItemDelegate;

        void paint(QPainter* painter, const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;
    };
} // namespace javelin::gui::mailboxes
