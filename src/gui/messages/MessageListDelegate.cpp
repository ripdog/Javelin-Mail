#include "gui/messages/MessageListDelegate.h"

#include "gui/messages/MessageListModel.h"

#include <QApplication>
#include <QDateTime>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>

namespace javelin::gui::messages
{
    namespace
    {

        [[nodiscard]] QString formattedTimestamp(const QString& isoTimestamp)
        {
            const auto dateTime = QDateTime::fromString(isoTimestamp, Qt::ISODate);
            if (!dateTime.isValid())
            {
                return isoTimestamp;
            }

            return dateTime.toLocalTime().toString(QStringLiteral("dd/MM/yyyy, HH:mm"));
        }

        [[nodiscard]] QRect insetRect(const QRect& rect, const int amount)
        {
            return rect.adjusted(amount, amount, -amount, -amount);
        }

        [[nodiscard]] QRect disclosureRect(const QRect& contentRect, const bool isMemberRow)
        {
            const int leftInset = isMemberRow ? 24 : 0;
            return QRect{contentRect.left() + leftInset, contentRect.top() + 2, 18, 18};
        }

        constexpr int unreadDotDiameter = 10;
        constexpr int unreadDotGap = 10;
        constexpr int memberIndent = 22;

    } // namespace

    MessageListDelegate::MessageListDelegate(QObject* parent) : QStyledItemDelegate(parent)
    {
    }

    MessageListDelegate::~MessageListDelegate() = default;

    const QIcon& MessageListDelegate::attachmentIcon() const
    {
        if (!m_attachmentIcon.isNull())
        {
            return m_attachmentIcon;
        }

        m_attachmentIcon = QIcon::fromTheme(QStringLiteral("mail-attachment"));
        if (m_attachmentIcon.isNull())
        {
            m_attachmentIcon = QIcon::fromTheme(QStringLiteral("mail-attachment-symbolic"));
        }
        if (m_attachmentIcon.isNull())
        {
            m_attachmentIcon =
                QApplication::style()->standardIcon(QStyle::SP_FileDialogDetailedView);
        }

        return m_attachmentIcon;
    }

    void MessageListDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                    const QModelIndex& index) const
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setRenderHint(QPainter::TextAntialiasing, true);

        const auto outerRect = insetRect(option.rect, 1);
        const auto rowKind =
            static_cast<MessageListModel::RowKind>(index.data(MessageListModel::RowKindRole).toInt());
        const bool isMemberRow = rowKind == MessageListModel::RowKind::ThreadMember;
        const auto cardRect = isMemberRow ? insetRect(outerRect.adjusted(memberIndent, 0, 0, 0), 1)
                                          : insetRect(outerRect, 1);
        const bool isSelected = (option.state & QStyle::State_Selected) != 0;
        const bool isUnread = index.data(MessageListModel::IsUnreadRole).toBool();
        const bool isFlagged = index.data(MessageListModel::IsFlaggedRole).toBool();
        const bool hasAttachment = index.data(MessageListModel::HasAttachmentRole).toBool();
        const auto threadCount = index.data(MessageListModel::ThreadMessageCountRole).toULongLong();
        const bool canExpand = index.data(MessageListModel::CanExpandRole).toBool();
        const bool isExpanded = index.data(MessageListModel::IsExpandedRole).toBool();

        const QColor background = isSelected ? QColor{37, 29, 45}
                                             : (isMemberRow ? QColor{30, 31, 36} : QColor{27, 28, 32});
        const QColor border = isSelected ? QColor{102, 72, 122}
                                         : (isMemberRow ? QColor{70, 72, 80} : QColor{58, 60, 68});
        const QColor senderColor = isUnread ? QColor{235, 236, 240} : QColor{214, 215, 220};
        const QColor textColor = QColor{214, 215, 220};
        const QColor mutedColor = QColor{154, 156, 164};
        const QColor accentColor = QColor{166, 117, 214};
        const QColor alertColor = QColor{230, 87, 87};

        painter->setPen(Qt::NoPen);
        painter->setBrush(background);
        painter->drawRoundedRect(cardRect, 10, 10);

        QPen borderPen{border};
        borderPen.setWidth(1);
        painter->setPen(borderPen);
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(cardRect, 10, 10);

        QRect contentRect = insetRect(cardRect, 14);
        if (canExpand)
        {
            const QRect arrowRect = disclosureRect(contentRect, isMemberRow);
            const auto primitive = isExpanded ? QStyle::PE_IndicatorArrowDown
                                              : QStyle::PE_IndicatorArrowRight;
            QStyleOption arrowOption;
            arrowOption.rect = arrowRect;
            arrowOption.palette = option.palette;
            arrowOption.state = QStyle::State_Enabled;
            QApplication::style()->drawPrimitive(primitive, &arrowOption, painter);
            contentRect.adjust(arrowRect.width() + 8, 0, 0, 0);
        }

        const auto sender = index.data(MessageListModel::SenderDisplayRole).toString();
        const auto subject = index.data(MessageListModel::SubjectRole).toString();
        const auto preview = index.data(MessageListModel::PreviewRole).toString();
        const auto timestamp =
            formattedTimestamp(index.data(MessageListModel::ReceivedAtRole).toString());

        auto senderFont = option.font;
        senderFont.setPointSize(senderFont.pointSize() + (isMemberRow ? 0 : 1));
        senderFont.setBold(true);
        painter->setFont(senderFont);
        painter->setPen(senderColor);

        const auto senderMetrics = QFontMetrics{senderFont};
        const auto timestampMetrics = QFontMetrics{option.font};
        const int timestampWidth = timestampMetrics.boundingRect(timestamp).width() + 12;
        const int unreadDotReserve = isUnread ? unreadDotDiameter + unreadDotGap : 0;
        const QRect rightHeaderRect{
            contentRect.left() + contentRect.width() - timestampWidth,
            contentRect.top(),
            timestampWidth,
            24,
        };
        const QRect leftHeaderRect{contentRect.left() + unreadDotReserve, contentRect.top(),
                                   std::max(0, contentRect.width() - timestampWidth - 10 -
                                                   unreadDotReserve),
                                   24};

        if (isUnread)
        {
            const QRect dotRect{contentRect.left(),
                                contentRect.top() + (leftHeaderRect.height() - unreadDotDiameter) / 2,
                                unreadDotDiameter, unreadDotDiameter};
            painter->setPen(Qt::NoPen);
            painter->setBrush(accentColor);
            painter->drawEllipse(dotRect);
        }

        painter->setFont(senderFont);
        painter->setPen(senderColor);
        painter->drawText(leftHeaderRect, Qt::AlignLeft | Qt::AlignVCenter,
                          senderMetrics.elidedText(sender, Qt::ElideRight, leftHeaderRect.width()));

        painter->setPen(textColor);
        painter->drawText(rightHeaderRect, Qt::AlignRight | Qt::AlignVCenter, timestamp);

        auto subjectFont = option.font;
        subjectFont.setPointSize(subjectFont.pointSize() + (isMemberRow ? 1 : 2));
        painter->setFont(subjectFont);
        painter->setPen(textColor);
        const auto subjectMetrics = QFontMetrics{subjectFont};
        const QRect subjectRect{contentRect.left(), contentRect.top() + 26, contentRect.width(),
                                28};
        painter->drawText(subjectRect, Qt::AlignLeft | Qt::AlignVCenter,
                          subjectMetrics.elidedText(subject, Qt::ElideRight, subjectRect.width()));

        auto previewFont = option.font;
        previewFont.setPointSize(previewFont.pointSize() - 1);
        painter->setFont(previewFont);
        painter->setPen(mutedColor);
        const auto previewMetrics = QFontMetrics{previewFont};
        QString lowerLine = preview;
        if (!isMemberRow && threadCount > 1)
        {
            lowerLine = QStringLiteral("Reply thread  %1 messages").arg(threadCount);
        }
        constexpr int attachmentWidth = 18;
        constexpr int attachmentGap = 8;
        const int attachmentReserve = hasAttachment ? attachmentWidth + attachmentGap : 0;
        const QRect previewRect{contentRect.left(), contentRect.top() + 58,
                                std::max(0, contentRect.width() - attachmentReserve), 22};
        painter->drawText(
            previewRect, Qt::AlignLeft | Qt::AlignVCenter,
            previewMetrics.elidedText(lowerLine, Qt::ElideRight, previewRect.width()));

        painter->setFont(subjectFont);
        if (isFlagged)
        {
            painter->setPen(alertColor);
            painter->drawText(QRect{contentRect.right() - 24, contentRect.bottom() - 4, 24, 20},
                              Qt::AlignRight | Qt::AlignBottom, QStringLiteral("HOT"));
        }
        else if (!isMemberRow && threadCount > 1)
        {
            painter->setPen(accentColor);
            painter->drawText(QRect{contentRect.right() - 32, contentRect.bottom() - 4, 32, 20},
                              Qt::AlignRight | Qt::AlignBottom, QStringLiteral("THR"));
        }

        if (hasAttachment)
        {
            attachmentIcon().paint(painter,
                                   QRect{contentRect.left() + contentRect.width() - attachmentWidth,
                                         previewRect.top() + 2, attachmentWidth, attachmentWidth},
                                   Qt::AlignCenter, QIcon::Normal, QIcon::Off);
        }

        painter->restore();
    }

    QSize MessageListDelegate::sizeHint(const QStyleOptionViewItem& option,
                                        const QModelIndex& index) const
    {
        Q_UNUSED(option);
        const auto rowKind =
            static_cast<MessageListModel::RowKind>(index.data(MessageListModel::RowKindRole).toInt());
        return {
            0,
            rowKind == MessageListModel::RowKind::ThreadMember ? 98 : 112,
        };
    }

    bool MessageListDelegate::editorEvent(QEvent* event, QAbstractItemModel* model,
                                          const QStyleOptionViewItem& option,
                                          const QModelIndex& index)
    {
        Q_UNUSED(model);
        if (!index.isValid() || !index.data(MessageListModel::CanExpandRole).toBool())
        {
            return QStyledItemDelegate::editorEvent(event, model, option, index);
        }

        if (event->type() != QEvent::MouseButtonPress &&
            event->type() != QEvent::MouseButtonRelease &&
            event->type() != QEvent::MouseButtonDblClick)
        {
            return QStyledItemDelegate::editorEvent(event, model, option, index);
        }

        const auto* mouseEvent = static_cast<QMouseEvent*>(event);
        QRect contentRect = insetRect(insetRect(option.rect, 1), 15);
        const bool isMemberRow =
            static_cast<MessageListModel::RowKind>(index.data(MessageListModel::RowKindRole).toInt()) ==
            MessageListModel::RowKind::ThreadMember;
        if (disclosureRect(contentRect, isMemberRow).contains(mouseEvent->position().toPoint()))
        {
            if (event->type() == QEvent::MouseButtonRelease &&
                mouseEvent->button() == Qt::LeftButton)
            {
                Q_EMIT threadExpansionToggled(index);
            }
            return true;
        }

        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }

} // namespace javelin::gui::messages
