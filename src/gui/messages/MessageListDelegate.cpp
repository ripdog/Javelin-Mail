#include "gui/messages/MessageListDelegate.h"

#include "gui/FontUtils.h"
#include "gui/IconUtils.h"
#include "gui/messages/MessageListModel.h"

#include <KLocalizedString>

#include <QApplication>
#include <QDateTime>
#include <QHelpEvent>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QToolTip>

#include <algorithm>
#include <cmath>

namespace javelin::gui::messages
{
    namespace
    {
        constexpr int unreadDotDiameter = 10;
        constexpr int unreadDotGap = 10;
        constexpr int memberIndent = 22;
        const QColor starredColor{245, 181, 42};

        constexpr int cardMargin = 1;
        constexpr int cardBorderWidth = 1;
        constexpr int cardCornerRadius = 10;
        // Horizontal inset between card border and content. Vertical padding is split
        // out below so the header text can sit closer to the top of the card and the
        // button row closer to the bottom, halving the visual padding the previous
        // uniform 14px inset produced.
        constexpr int contentHInset = 14;
        constexpr int contentTopInset = 7;
        constexpr int contentBottomInset = 7;
        constexpr int headerHeight = 24;
        constexpr int headerGap = 10;
        constexpr int timestampPadding = 16;
        constexpr int subjectOffset = 26;
        constexpr int subjectHeight = 24;
        constexpr int senderFontSizeIncreaseParent = 1;
        constexpr int subjectFontSizeIncreaseMember = 1;
        constexpr int subjectFontSizeIncreaseParent = 2;
        constexpr int buttonSize = 28;
        constexpr int buttonMargin = 29;
        constexpr int buttonGap = 6;
        constexpr int buttonIconSize = 16;
        // Tight internal horizontal padding for the icon+text inside our buttons.
        // We draw only CE_PushButtonBevel (the frame) and position the label
        // ourselves, so these values — not the style's PM_ButtonMargin — define the
        // visible left/right inset and keep "N replies" from being clipped.
        constexpr int buttonContentHPadding = 6;
        constexpr int buttonIconTextGap = 4;
        // Trailing chevron shown on the replies button to indicate the open/close
        // state of the thread (arrow-down-12.svg when expanded, arrow-right-12.svg
        // when collapsed). Smaller than the leading icon so it visually reads as
        // an indicator rather than another action.
        constexpr int buttonChevronSize = 12;
        constexpr int buttonTextChevronGap = 6;
        constexpr int tagPillHeight = 22;
        constexpr int tagPillHorizontalPadding = 7;
        constexpr int tagPillIconSize = 14;
        constexpr int tagPillIconTextGap = 4;
        constexpr int tagPillMaxWidth = 160;
        constexpr int memberRowHeight = 100;
        constexpr int parentRowHeight = 104;

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

            return QLocale{}.toString(dateTime.toLocalTime(), QLocale::ShortFormat);
        }

        [[nodiscard]] QRect insetRect(const QRect& rect, const int amount)
        {
            return rect.adjusted(amount, amount, -amount, -amount);
        }

        [[nodiscard]] QRect cardRectForOption(const QStyleOptionViewItem& option,
                                              const bool isMemberRow)
        {
            const auto outerRect = insetRect(option.rect, cardMargin);
            return isMemberRow ? insetRect(outerRect.adjusted(memberIndent, 0, 0, 0), cardMargin)
                               : insetRect(outerRect, cardMargin);
        }

        [[nodiscard]] QRect contentRectForCard(const QRect& cardRect)
        {
            return cardRect.adjusted(contentHInset, contentTopInset, -contentHInset,
                                     -contentBottomInset);
        }

        [[nodiscard]] QString repliesLabelForIndex(const QModelIndex& index)
        {
            if (!index.data(MessageListModel::CanExpandRole).toBool())
            {
                return {};
            }
            const auto mailboxThreadCount = index.data(MessageListModel::ThreadMessageCountRole);
            const auto globalThreadCount =
                index.data(MessageListModel::GlobalThreadMessageCountRole);
            if (!mailboxThreadCount.isValid() && !globalThreadCount.isValid())
            {
                return i18nc("@action:button open conversation", "Replies");
            }
            // Counts include the parent summary row itself; replies are the rest. Conversation
            // expansion is global, so prefer the larger known count when some members live in
            // another mailbox.
            const auto mailboxCount =
                mailboxThreadCount.isValid() ? mailboxThreadCount.toULongLong() : qulonglong{0};
            const auto globalCount =
                globalThreadCount.isValid() ? globalThreadCount.toULongLong() : qulonglong{0};
            const auto count = std::max(mailboxCount, globalCount);
            if (count <= 1)
            {
                return i18nc("@action:button open conversation", "Replies");
            }
            const auto replyCount = static_cast<qulonglong>(count - 1);
            return i18np("%1 reply", "%1 replies", replyCount);
        }

        // Horizontal extent a button needs to display its leading icon, label and
        // optional trailing chevron without clipping. Used both for layout (so the
        // button grows with the reply count) and for hit-testing.
        [[nodiscard]] int buttonNaturalWidth(const QFontMetrics& metrics, const bool hasLeadingIcon,
                                             const QString& text, const bool hasTrailingIcon)
        {
            int width = 2 * buttonContentHPadding;
            if (hasLeadingIcon)
            {
                width += buttonIconSize;
                if (!text.isEmpty())
                {
                    width += buttonIconTextGap;
                }
            }
            if (!text.isEmpty())
            {
                width += metrics.horizontalAdvance(text);
            }
            if (hasTrailingIcon)
            {
                if (!text.isEmpty())
                {
                    width += buttonTextChevronGap;
                }
                width += buttonChevronSize;
            }
            return width;
        }

        [[nodiscard]] QRect repliesButtonRect(const QRect& contentRect,
                                              const QStyleOptionViewItem& option,
                                              const QString& label, const bool hasTrailingIcon)
        {
            if (label.isEmpty())
            {
                return {};
            }
            const int width = buttonNaturalWidth(option.fontMetrics, true, label, hasTrailingIcon);
            return QRect{contentRect.left(), contentRect.bottom() - buttonMargin, width,
                         buttonSize};
        }

        [[nodiscard]] QRect starButtonRect(const QRect& contentRect)
        {
            return QRect{contentRect.right() - buttonMargin, contentRect.bottom() - buttonMargin,
                         buttonSize, buttonSize};
        }

        [[nodiscard]] QRect attachmentButtonRect(const QRect& contentRect, const bool hasAttachment)
        {
            if (!hasAttachment)
            {
                return {};
            }
            return QRect{starButtonRect(contentRect).left() - buttonSize - buttonGap,
                         contentRect.bottom() - buttonMargin, buttonSize, buttonSize};
        }

        [[nodiscard]] double linearSrgbChannel(const double channel)
        {
            return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
        }

        [[nodiscard]] QColor readablePillForeground(const QColor& background)
        {
            const double luminance = 0.2126 * linearSrgbChannel(background.redF()) +
                                     0.7152 * linearSrgbChannel(background.greenF()) +
                                     0.0722 * linearSrgbChannel(background.blueF());
            const double blackContrast = (luminance + 0.05) / 0.05;
            const double whiteContrast = 1.05 / (luminance + 0.05);
            return blackContrast >= whiteContrast ? QColor{Qt::black} : QColor{Qt::white};
        }

        void drawTagPill(QPainter* painter, const QStyleOptionViewItem& option, const QRect& rect,
                         const QString& name, QColor background)
        {
            if (!background.isValid())
                background = option.palette.color(QPalette::Highlight);
            const auto foreground = readablePillForeground(background);

            painter->save();
            painter->setPen(Qt::NoPen);
            painter->setBrush(background);
            painter->drawRoundedRect(rect, rect.height() / 2.0, rect.height() / 2.0);

            QColor outline = foreground;
            outline.setAlpha(48);
            painter->setPen(QPen{outline, 1});
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(rect.adjusted(0, 0, -1, -1), rect.height() / 2.0,
                                     rect.height() / 2.0);

            const auto tagIcon = javelin::gui::themedSvgIcon(
                QStringLiteral(":/icons/thunderbird-icons/tag.svg"), foreground);
            const QRect iconRect{rect.left() + tagPillHorizontalPadding,
                                 rect.center().y() - tagPillIconSize / 2, tagPillIconSize,
                                 tagPillIconSize};
            tagIcon.paint(painter, iconRect);

            const int textLeft = iconRect.right() + 1 + tagPillIconTextGap;
            const QRect textRect{
                textLeft, rect.top(),
                std::max(0, rect.right() - tagPillHorizontalPadding - textLeft + 1), rect.height()};
            painter->setFont(option.font);
            painter->setPen(foreground);
            painter->drawText(
                textRect, Qt::AlignLeft | Qt::AlignVCenter,
                option.fontMetrics.elidedText(name, Qt::ElideRight, textRect.width()));
            painter->restore();
        }

        [[nodiscard]] int drawTagPills(QPainter* painter, const QStyleOptionViewItem& option,
                                       const QStringList& names, const QStringList& colors,
                                       int left, const int right, const int centerY)
        {
            const auto count = std::min(names.size(), colors.size());
            for (qsizetype index = 0; index < count; ++index)
            {
                const int naturalWidth = 2 * tagPillHorizontalPadding + tagPillIconSize +
                                         tagPillIconTextGap +
                                         option.fontMetrics.horizontalAdvance(names[index]);
                const int width = std::min(tagPillMaxWidth, naturalWidth);
                if (left + width - 1 > right)
                    break;

                const QRect pillRect{left, centerY - tagPillHeight / 2, width, tagPillHeight};
                drawTagPill(painter, option, pillRect, names[index], QColor{colors[index]});
                left = pillRect.right() + 1 + buttonGap;
            }
            return left;
        }

        void drawButton(QPainter* painter, const QStyleOptionViewItem& option, const QRect& rect,
                        const QIcon& icon, const QString& text, const bool hovered,
                        const bool pressed, const QIcon& trailingIcon = {})
        {
            if (rect.isEmpty())
            {
                return;
            }

            QStyleOptionButton buttonOption;
            buttonOption.rect = rect;
            buttonOption.palette = option.palette;
            buttonOption.fontMetrics = option.fontMetrics;
            buttonOption.state = QStyle::State_Enabled;
            if (hovered)
            {
                buttonOption.state |= QStyle::State_MouseOver;
            }
            if (pressed)
            {
                buttonOption.state |= QStyle::State_Sunken;
            }

            const auto* style = QApplication::style();
            // Draw only the frame; icon, text and trailing chevron are laid out
            // manually below so we control the internal horizontal padding. The
            // style's CE_PushButton reserves ~10-12px per side via PM_ButtonMargin /
            // SE_PushButtonContents, which clipped the "N replies" label.
            buttonOption.icon = QIcon{};
            buttonOption.text = QString{};
            style->drawControl(QStyle::CE_PushButtonBevel, &buttonOption, painter);

            // A sunken button shifts its label slightly; mirror the style's metric so
            // our manually placed content stays visually consistent with the bevel.
            const int shiftH =
                pressed ? style->pixelMetric(QStyle::PM_ButtonShiftHorizontal, &buttonOption) : 0;
            const int shiftV =
                pressed ? style->pixelMetric(QStyle::PM_ButtonShiftVertical, &buttonOption) : 0;

            painter->save();
            painter->translate(shiftH, shiftV);

            const bool hasTrailing = !trailingIcon.isNull();
            const QRect contentRect =
                rect.adjusted(buttonContentHPadding, 0, -buttonContentHPadding, 0);

            // Trailing chevron is anchored to the right edge; everything else flows
            // left-to-right from the leading icon.
            QRect textRect = contentRect;
            int chevronX = 0;
            if (hasTrailing)
            {
                chevronX = contentRect.right() - buttonChevronSize + 1;
                textRect.setRight(chevronX - buttonTextChevronGap);
            }

            const int iconY = contentRect.center().y() - buttonIconSize / 2;
            if (!icon.isNull())
            {
                // Center horizontally when there is no text (icon-only buttons);
                // otherwise anchor the icon at the left edge and reserve room for
                // the trailing label.
                const int iconX = text.isEmpty() ? contentRect.center().x() - buttonIconSize / 2
                                                 : contentRect.left();
                icon.paint(painter,
                           QRect{QPoint{iconX, iconY}, QSize{buttonIconSize, buttonIconSize}});
                if (!text.isEmpty())
                {
                    textRect.setLeft(iconX + buttonIconSize + buttonIconTextGap);
                }
            }

            if (!text.isEmpty())
            {
                painter->setFont(option.font);
                painter->setPen(option.palette.color(QPalette::ButtonText));
                painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, text);
            }

            if (hasTrailing)
            {
                const int chevronY = contentRect.center().y() - buttonChevronSize / 2;
                trailingIcon.paint(painter, QRect{QPoint{chevronX, chevronY},
                                                  QSize{buttonChevronSize, buttonChevronSize}});
            }

            painter->restore();
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
        painter->drawRoundedRect(cardRect, cardCornerRadius, cardCornerRadius);

        QPen borderPen{border};
        borderPen.setWidth(cardBorderWidth);
        painter->setPen(borderPen);
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(cardRect, cardCornerRadius, cardCornerRadius);

        QRect contentRect = contentRectForCard(cardRect);

        const auto sender = index.data(MessageListModel::SenderDisplayRole).toString();
        const auto subject = index.data(MessageListModel::SubjectRole).toString();
        const auto timestamp =
            formattedTimestamp(index.data(MessageListModel::ReceivedAtRole).toString());

        auto senderFont = javelin::gui::fontWithSizeDelta(
            option.font, isMemberRow ? 0 : senderFontSizeIncreaseParent);
        senderFont.setBold(true);
        painter->setFont(senderFont);
        painter->setPen(senderColor);

        const auto senderMetrics = QFontMetrics{senderFont};
        const auto timestampMetrics = QFontMetrics{option.font};
        const int timestampWidth =
            timestampMetrics.boundingRect(timestamp).width() + timestampPadding;
        const int unreadDotReserve = isUnread ? unreadDotDiameter + unreadDotGap : 0;
        const QRect rightHeaderRect{
            contentRect.left() + contentRect.width() - timestampWidth,
            contentRect.top(),
            timestampWidth,
            headerHeight,
        };
        const QRect leftHeaderRect{
            contentRect.left() + unreadDotReserve, contentRect.top(),
            std::max(0, contentRect.width() - timestampWidth - headerGap - unreadDotReserve),
            headerHeight};

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

        auto subjectFont = javelin::gui::fontWithSizeDelta(
            option.font,
            isMemberRow ? subjectFontSizeIncreaseMember : subjectFontSizeIncreaseParent);
        painter->setFont(subjectFont);
        painter->setPen(textColor);
        const auto subjectMetrics = QFontMetrics{subjectFont};
        const QRect subjectRect{contentRect.left(), contentRect.top() + subjectOffset,
                                contentRect.width(), subjectHeight};
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
        // Trailing chevron communicates the open/close state of the thread.
        const auto isExpanded = index.data(MessageListModel::IsExpandedRole).toBool();
        const auto chevronIcon =
            canExpand
                ? javelin::gui::themedSvgIcon(
                      isExpanded ? QStringLiteral(":/icons/thunderbird-icons/arrow-down-12.svg")
                                 : QStringLiteral(":/icons/thunderbird-icons/arrow-right-12.svg"),
                      buttonColor)
                : QIcon{};
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

        const auto repliesLabel = repliesLabelForIndex(index);
        const auto repliesRect =
            repliesButtonRect(contentRect, option, repliesLabel, !chevronIcon.isNull());
        drawButton(painter, option, repliesRect, repliesIcon, repliesLabel, repliesHovered,
                   repliesPressed, chevronIcon);

        const auto attachmentRect = attachmentButtonRect(contentRect, hasAttachment);
        const auto starRect = starButtonRect(contentRect);
        const int bottomContentRight =
            (attachmentRect.isEmpty() ? starRect.left() : attachmentRect.left()) - buttonGap;
        int bottomContentLeft =
            repliesRect.isEmpty() ? contentRect.left() : repliesRect.right() + 1 + buttonGap;
        const auto tagNames = index.data(MessageListModel::TagNamesRole).toStringList();
        const auto tagColors = index.data(MessageListModel::TagColorsRole).toStringList();
        bottomContentLeft =
            drawTagPills(painter, option, tagNames, tagColors, bottomContentLeft,
                         bottomContentRight, contentRect.bottom() - buttonMargin + buttonSize / 2);

        if (index.data(MessageListModel::IsSearchResultRole).toBool())
        {
            const auto mailboxNames = index.data(MessageListModel::MailboxNamesRole)
                                          .toStringList()
                                          .join(QStringLiteral(", "));
            if (!mailboxNames.isEmpty() && bottomContentLeft <= bottomContentRight)
            {
                const QRect mailboxRect{bottomContentLeft, contentRect.bottom() - buttonMargin,
                                        std::max(0, bottomContentRight - bottomContentLeft + 1),
                                        buttonSize};
                const auto folderIcon = javelin::gui::themedSvgIcon(
                    QStringLiteral(":/icons/thunderbird-icons/folder.svg"), textColor);
                const QRect folderIconRect{mailboxRect.left(),
                                           mailboxRect.center().y() - buttonIconSize / 2,
                                           buttonIconSize, buttonIconSize};
                folderIcon.paint(painter, folderIconRect);
                const QRect mailboxTextRect =
                    mailboxRect.adjusted(buttonIconSize + buttonIconTextGap, 0, 0, 0);
                painter->setFont(option.font);
                painter->setPen(textColor);
                painter->drawText(mailboxTextRect, Qt::AlignLeft | Qt::AlignVCenter,
                                  option.fontMetrics.elidedText(mailboxNames, Qt::ElideRight,
                                                                mailboxTextRect.width()));
            }
        }
        drawButton(painter, option, attachmentRect, attachmentIcon, QString{}, attachmentHovered,
                   attachmentPressed);
        drawButton(painter, option, starRect, starIcon, QString{}, starHovered, starPressed);

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
            rowKind == MessageListModel::RowKind::ThreadMember ? memberRowHeight : parentRowHeight,
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
        const auto repliesLabel = repliesLabelForIndex(index);
        // The chevron always accompanies the replies button, so reserve its width
        // here too — otherwise hit-testing would miss the trailing chevron area.
        if (repliesButtonRect(contentRect, option, repliesLabel, true).contains(position))
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
                                       ? i18n("Collapse thread")
                                       : i18n("Expand thread"),
                                   tooltipWidget(option));
                return true;
            case ButtonKind::Attachment:
                QToolTip::showText(helpEvent->globalPos(), i18n("Has attachments"),
                                   tooltipWidget(option));
                return true;
            case ButtonKind::Star:
                QToolTip::showText(helpEvent->globalPos(),
                                   index.data(MessageListModel::IsFlaggedRole).toBool()
                                       ? i18n("Remove star")
                                       : i18n("Add star"),
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
