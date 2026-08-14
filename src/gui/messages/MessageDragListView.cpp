#include "gui/messages/MessageDragListView.h"

#include "gui/messages/MessageListModel.h"

#include <KLocalizedString>

#include <QDrag>
#include <QItemSelectionModel>
#include <QPainter>

#include <algorithm>

namespace javelin::gui::messages
{
    qsizetype representedMessageCountForDrag(const QModelIndexList& indexes)
    {
        qsizetype count = 0;
        for (const auto& index : indexes)
        {
            qsizetype represented = 1;
            const auto rowKind = static_cast<MessageListModel::RowKind>(
                index.data(MessageListModel::RowKindRole).toInt());
            const bool collapsedSummary = rowKind == MessageListModel::RowKind::ThreadSummary &&
                                          !index.data(MessageListModel::IsExpandedRole).toBool();
            if (collapsedSummary)
            {
                const auto mailboxCount = index.data(MessageListModel::ThreadMessageCountRole);
                if (mailboxCount.isValid())
                    represented = std::max<qsizetype>(1, mailboxCount.toLongLong());
            }
            count += represented;
        }
        return count;
    }

    void MessageDragListView::startDrag(const Qt::DropActions supportedActions)
    {
        auto indexes = selectionModel()->selectedRows();
        if (indexes.isEmpty() && currentIndex().isValid())
        {
            indexes.push_back(currentIndex());
        }

        auto* dragMimeData = model()->mimeData(indexes);
        if (dragMimeData == nullptr)
        {
            return;
        }

        const auto representedCount = representedMessageCountForDrag(indexes);
        const QString label = i18np("%1 message", "%1 messages", representedCount);
        const QFontMetrics metrics{font()};
        const QSize badgeSize{metrics.horizontalAdvance(label) + 48,
                              std::max(34, metrics.height() + 14)};
        const qreal scale = devicePixelRatioF();
        QPixmap badge{badgeSize * scale};
        badge.setDevicePixelRatio(scale);
        badge.fill(Qt::transparent);

        QPainter painter{&badge};
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(palette().highlight());
        painter.drawRoundedRect(QRect{QPoint{}, badgeSize}.adjusted(1, 1, -1, -1), 8, 8);

        const QRect envelopeRect{12, (badgeSize.height() - 14) / 2, 20, 14};
        QPen envelopePen{palette().highlightedText(), 1.5};
        painter.setPen(envelopePen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(envelopeRect, 2, 2);
        painter.drawLine(envelopeRect.topLeft(), envelopeRect.center());
        painter.drawLine(envelopeRect.topRight(), envelopeRect.center());

        painter.setPen(palette().highlightedText().color());
        painter.setFont(font());
        painter.drawText(QRect{40, 0, badgeSize.width() - 48, badgeSize.height()},
                         Qt::AlignVCenter | Qt::AlignLeft, label);

        auto* drag = new QDrag{this};
        drag->setMimeData(dragMimeData);
        drag->setPixmap(badge);
        drag->setHotSpot(QPoint{-10, -10});
        static_cast<void>(drag->exec(supportedActions, defaultDropAction()));
    }

} // namespace javelin::gui::messages
