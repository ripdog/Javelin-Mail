#include "gui/messages/MessageListDelegate.h"

#include "gui/IconUtils.h"
#include "gui/messages/MessageListModel.h"

#include <QApplication>
#include <QDateTime>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QToolTip>

namespace javelin::gui::messages
{
    namespace
    {
        constexpr int unreadDotDiameter = 10;
        constexpr int unreadDotGap = 10;
        constexpr int memberIndent = 22;
        const QColor starredColor{245, 181, 42};

        [[nodiscard]] QWidget* tooltipWidget(const QStyleOptionViewItem& option)
        {
            return const_cast<QWidget*>(option.widget);
        }

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

        [[nodiscard]] QRect cardRectForOption(const QStyleOptionViewItem& option,
                                              const bool isMemberRow)
        {
            const auto outerRect = insetRect(option.rect, 1);
            return isMemberRow ? insetRect(outerRect.adjusted(memberIndent, 0, 0, 0), 1)
                               : insetRect(outerRect, 1);
        }

        [[nodiscard]] QRect contentRectForCard(const QRect& cardRect)
        {
            return insetRect(cardRect, 14);
        }

        [[nodiscard]] QRect repliesButtonRect(const QRect& contentRect, const bool canExpand)
        {
            if (!canExpand)
            {
                return {};
            }
            return QRect{contentRect.left(), contentRect.bottom() - 29, 116, 28};
        }

        [[nodiscard]] QRect starButtonRect(const QRect& contentRect)
        {
            return QRect{contentRect.right() - 29, contentRect.bottom() - 29, 28, 28};
        }

        [[nodiscard]] QRect attachmentButtonRect(const QRect& contentRect, const bool hasAttachment)
        {
            if (!hasAttachment)
            {
                return {};
            }
            return QRect{starButtonRect(contentRect).left() - 34, contentRect.bottom() - 29, 28,
                         28};
        }

        void drawButton(QPainter* painter, const QStyleOptionViewItem& option, const QRect& rect,
                        const QIcon& icon, const QString& text, const bool hovered,
                        const bool pressed)
        {
            if (rect.isEmpty())
            {
                return;
            }

            QStyleOptionButton buttonOption;
            buttonOption.rect = rect;
            buttonOption.palette = option.palette;
            buttonOption.fontMetrics = option.fontMetrics;
            buttonOption.icon = icon;
            buttonOption.iconSize = QSize{16, 16};
            buttonOption.text = text;
            buttonOption.state = QStyle::State_Enabled;
            if (hovered)
            {
                buttonOption.state |= QStyle::State_MouseOver;
            }
            if (pressed)
            {
                buttonOption.state |= QStyle::State_Sunken;
            }

            QApplication::style()->drawControl(QStyle::CE_PushButton, &buttonOption, painter);
        }

    } // namespace

    MessageListDelegate::MessageListDelegate(QObject* parent) : QStyledItemDelegate(parent)
    {
    }

    MessageListDelegate::~MessageListDelegate() = default;

    void MessageListDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                    const QModelIndex& index) const
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setRenderHint(QPainter::TextAntialiasing, true);

        const auto rowKind = static_cast<MessageListModel::RowKind>(
            index.data(MessageListModel::RowKindRole).toInt());
        const bool isMemberRow = rowKind == MessageListModel::RowKind::ThreadMember;
        const auto cardRect = cardRectForOption(option, isMemberRow);
        const bool isSelected = (option.state & QStyle::State_Selected) != 0;
        const bool isUnread = index.data(MessageListModel::IsUnreadRole).toBool();
        const bool isFlagged = index.data(MessageListModel::IsFlaggedRole).toBool();
        const bool hasAttachment = index.data(MessageListModel::HasAttachmentRole).toBool();
        const auto threadCount = index.data(MessageListModel::ThreadMessageCountRole).toULongLong();
        const bool canExpand = index.data(MessageListModel::CanExpandRole).toBool();
        const auto& palette = option.palette;
        const QColor background =
            palette.color(isSelected ? QPalette::Highlight
                                     : (isMemberRow ? QPalette::AlternateBase : QPalette::Base));
        const QColor border =
            palette.color(isSelected ? QPalette::HighlightedText
                                     : (isMemberRow ? QPalette::Midlight : QPalette::Mid));
        const QColor textColor =
            palette.color(isSelected ? QPalette::HighlightedText : QPalette::Text);
        const QColor senderColor =
            palette.color(isSelected ? QPalette::HighlightedText
                                     : (isUnread ? QPalette::Text : QPalette::WindowText));
        const QColor accentColor = palette.color(QPalette::Highlight);

        painter->setPen(Qt::NoPen);
        painter->setBrush(background);
        painter->drawRoundedRect(cardRect, 10, 10);

        QPen borderPen{border};
        borderPen.setWidth(1);
        painter->setPen(borderPen);
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(cardRect, 10, 10);

        QRect contentRect = contentRectForCard(cardRect);

        const auto sender = index.data(MessageListModel::SenderDisplayRole).toString();
        const auto subject = index.data(MessageListModel::SubjectRole).toString();
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
        const QRect leftHeaderRect{
            contentRect.left() + unreadDotReserve, contentRect.top(),
            std::max(0, contentRect.width() - timestampWidth - 10 - unreadDotReserve), 24};

        if (isUnread)
        {
            const QRect dotRect{contentRect.left(),
                                contentRect.top() +
                                    ((leftHeaderRect.height() - unreadDotDiameter) / 2),
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

        const auto buttonColor = palette.color(QPalette::ButtonText);
        const auto repliesIcon = javelin::gui::themedSvgIcon(
            QStringLiteral(":/icons/thunderbird-icons/replies.svg"), buttonColor);
        const auto attachmentIcon = javelin::gui::themedSvgIcon(
            QStringLiteral(":/icons/thunderbird-icons/attachment.svg"), buttonColor);
        const auto starIcon = javelin::gui::themedSvgIcon(
            isFlagged ? QStringLiteral(":/icons/thunderbird-icons/starred.svg")
                      : QStringLiteral(":/icons/thunderbird-icons/star.svg"),
            isFlagged ? starredColor : buttonColor);
        const bool repliesHovered =
            m_hoveredIndex == index && m_hoveredButton == ButtonKind::Replies;
        const bool attachmentHovered =
            m_hoveredIndex == index && m_hoveredButton == ButtonKind::Attachment;
        const bool starHovered = m_hoveredIndex == index && m_hoveredButton == ButtonKind::Star;
        const bool repliesPressed =
            m_pressedIndex == index && m_pressedButton == ButtonKind::Replies;
        const bool attachmentPressed =
            m_pressedIndex == index && m_pressedButton == ButtonKind::Attachment;
        const bool starPressed = m_pressedIndex == index && m_pressedButton == ButtonKind::Star;

        const auto repliesLabel =
            canExpand ? QStringLiteral("%1 replies").arg(static_cast<qulonglong>(threadCount - 1))
                      : QString{};
        drawButton(painter, option, repliesButtonRect(contentRect, canExpand), repliesIcon,
                   repliesLabel, repliesHovered, repliesPressed);
        drawButton(painter, option, attachmentButtonRect(contentRect, hasAttachment),
                   attachmentIcon, QString{}, attachmentHovered, attachmentPressed);
        drawButton(painter, option, starButtonRect(contentRect), starIcon, QString{}, starHovered,
                   starPressed);

        painter->restore();
    }

    QSize MessageListDelegate::sizeHint(const QStyleOptionViewItem& option,
                                        const QModelIndex& index) const
    {
        Q_UNUSED(option);
        const auto rowKind = static_cast<MessageListModel::RowKind>(
            index.data(MessageListModel::RowKindRole).toInt());
        return {
            0,
            rowKind == MessageListModel::RowKind::ThreadMember ? 92 : 104,
        };
    }

    MessageListDelegate::ButtonKind
    MessageListDelegate::buttonAt(const QStyleOptionViewItem& option, const QModelIndex& index,
                                  const QPoint position) const
    {
        if (!index.isValid())
        {
            return ButtonKind::None;
        }

        const auto rowKind = static_cast<MessageListModel::RowKind>(
            index.data(MessageListModel::RowKindRole).toInt());
        const bool isMemberRow = rowKind == MessageListModel::RowKind::ThreadMember;
        const auto contentRect = contentRectForCard(cardRectForOption(option, isMemberRow));
        if (starButtonRect(contentRect).contains(position))
        {
            return ButtonKind::Star;
        }
        if (attachmentButtonRect(contentRect,
                                 index.data(MessageListModel::HasAttachmentRole).toBool())
                .contains(position))
        {
            return ButtonKind::Attachment;
        }
        if (repliesButtonRect(contentRect, index.data(MessageListModel::CanExpandRole).toBool())
                .contains(position))
        {
            return ButtonKind::Replies;
        }
        return ButtonKind::None;
    }

    bool MessageListDelegate::editorEvent(QEvent* event, QAbstractItemModel* model,
                                          const QStyleOptionViewItem& option,
                                          const QModelIndex& index)
    {
        if (!index.isValid())
        {
            return QStyledItemDelegate::editorEvent(event, model, option, index);
        }

        if (event->type() == QEvent::ToolTip)
        {
            auto* helpEvent = static_cast<QHelpEvent*>(event);
            switch (buttonAt(option, index, helpEvent->pos()))
            {
            case ButtonKind::Replies:
                QToolTip::showText(helpEvent->globalPos(),
                                   index.data(MessageListModel::IsExpandedRole).toBool()
                                       ? QStringLiteral("Collapse thread")
                                       : QStringLiteral("Expand thread"),
                                   tooltipWidget(option));
                return true;
            case ButtonKind::Attachment:
                QToolTip::showText(helpEvent->globalPos(), QStringLiteral("Has attachments"),
                                   tooltipWidget(option));
                return true;
            case ButtonKind::Star:
                QToolTip::showText(helpEvent->globalPos(),
                                   index.data(MessageListModel::IsFlaggedRole).toBool()
                                       ? QStringLiteral("Remove star")
                                       : QStringLiteral("Add star"),
                                   tooltipWidget(option));
                return true;
            case ButtonKind::None:
                QToolTip::hideText();
                break;
            }
        }

        if (event->type() == QEvent::MouseMove)
        {
            const auto* mouseEvent = static_cast<QMouseEvent*>(event);
            const auto hoveredButton = buttonAt(option, index, mouseEvent->position().toPoint());
            if (m_hoveredIndex != index || m_hoveredButton != hoveredButton)
            {
                const auto previousIndex = m_hoveredIndex;
                m_hoveredIndex = index;
                m_hoveredButton = hoveredButton;
                if (previousIndex.isValid())
                {
                    Q_EMIT const_cast<QAbstractItemModel*>(model)->dataChanged(previousIndex,
                                                                               previousIndex);
                }
                Q_EMIT model->dataChanged(index, index);
            }
            return hoveredButton != ButtonKind::None;
        }

        if (event->type() == QEvent::Leave)
        {
            const auto previousIndex = m_hoveredIndex;
            m_hoveredIndex = QModelIndex{};
            m_hoveredButton = ButtonKind::None;
            if (previousIndex.isValid())
            {
                Q_EMIT model->dataChanged(previousIndex, previousIndex);
            }
            return false;
        }

        if (event->type() != QEvent::MouseButtonPress &&
            event->type() != QEvent::MouseButtonRelease &&
            event->type() != QEvent::MouseButtonDblClick)
        {
            return QStyledItemDelegate::editorEvent(event, model, option, index);
        }

        const auto* mouseEvent = static_cast<QMouseEvent*>(event);
        const auto button = buttonAt(option, index, mouseEvent->position().toPoint());
        if (button == ButtonKind::None)
        {
            return QStyledItemDelegate::editorEvent(event, model, option, index);
        }

        if (event->type() == QEvent::MouseButtonPress && mouseEvent->button() == Qt::LeftButton)
        {
            m_pressedIndex = index;
            m_pressedButton = button;
            Q_EMIT model->dataChanged(index, index);
            return true;
        }

        if (event->type() == QEvent::MouseButtonRelease && mouseEvent->button() == Qt::LeftButton)
        {
            const bool activated = m_pressedIndex == index && m_pressedButton == button;
            m_pressedIndex = QModelIndex{};
            m_pressedButton = ButtonKind::None;
            Q_EMIT model->dataChanged(index, index);
            if (activated)
            {
                switch (button)
                {
                case ButtonKind::Replies:
                    Q_EMIT threadExpansionToggled(index);
                    break;
                case ButtonKind::Attachment:
                    Q_EMIT attachmentButtonClicked(index);
                    break;
                case ButtonKind::Star:
                    Q_EMIT flaggedToggled(index);
                    break;
                case ButtonKind::None:
                    break;
                }
            }
            return true;
        }

        return true;
    }

} // namespace javelin::gui::messages
