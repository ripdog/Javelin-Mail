#include "gui/mailboxes/MailboxTreeDelegate.h"

#include "gui/mailboxes/MailboxTreeModel.h"

#include <QPainter>

namespace javelin::gui::mailboxes
{
    void MailboxTreeDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                    const QModelIndex& index) const
    {
        const auto statusValue = index.data(MailboxTreeModel::ConnectionStatusRole);
        if (!statusValue.isValid())
        {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        constexpr int statusSpace = 25;
        auto contentOption = option;
        contentOption.rect.adjust(0, 0, -statusSpace, 0);
        QStyledItemDelegate::paint(painter, contentOption, index);

        const auto status = static_cast<MailboxTreeModel::ConnectionStatus>(statusValue.toInt());
        QColor color;
        switch (status)
        {
        case MailboxTreeModel::ConnectionStatus::Disconnected:
            color = QColor{QStringLiteral("#d96c6c")};
            break;
        case MailboxTreeModel::ConnectionStatus::Connecting:
            color = QColor{QStringLiteral("#d6a14b")};
            break;
        case MailboxTreeModel::ConnectionStatus::Connected:
            color = QColor{QStringLiteral("#69b36f")};
            break;
        }

        constexpr int diameter = 9;
        constexpr int rightMargin = 8;
        const QRect dotRect{option.rect.right() - rightMargin - diameter,
                            option.rect.center().y() - diameter / 2, diameter, diameter};
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        painter->setBrush(color);
        painter->drawEllipse(dotRect);
        painter->restore();
    }
} // namespace javelin::gui::mailboxes
