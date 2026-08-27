#include "gui/messages/MessageDragListView.h"

#include "gui/messages/MessageListModel.h"

#include <KLocalizedString>

#include <QDrag>
#include <QFocusEvent>
#include <QItemSelectionModel>
#include <QMimeData>
#include <QPainter>
#include <QVariant>

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>

namespace javelin::gui::messages
{
    namespace
    {
        const QString externalFileMimeType = QStringLiteral("text/uri-list");

        class MessageDragMimeData final : public QMimeData
        {
          public:
            MessageDragMimeData(QByteArray internalPayload,
                                ExternalDragUrlProvider externalUrlProvider)
                : m_externalUrlProvider(std::move(externalUrlProvider))
            {
                setData(QString::fromLatin1(messageDragMimeType), std::move(internalPayload));
            }

            [[nodiscard]] bool hasFormat(const QString& mimeType) const override
            {
                if (mimeType == externalFileMimeType && externalFilesAvailable())
                    return true;
                return QMimeData::hasFormat(mimeType);
            }

            [[nodiscard]] QStringList formats() const override
            {
                auto result = QMimeData::formats();
                if (externalFilesAvailable() && !result.contains(externalFileMimeType))
                    result.push_back(externalFileMimeType);
                return result;
            }

          protected:
            [[nodiscard]] QVariant retrieveData(const QString& mimeType,
                                                const QMetaType preferredType) const override
            {
                if (mimeType != externalFileMimeType || !externalFilesAvailable())
                    return QMimeData::retrieveData(mimeType, preferredType);

                if (!m_externalUrls.has_value())
                {
                    m_externalUrls = m_externalUrlProvider();
                    m_externalUrlProvider = {};
                }

                QVariantList variantUrls;
                variantUrls.reserve(m_externalUrls->size());
                for (const auto& url : *m_externalUrls)
                    variantUrls.push_back(url);
                if (preferredType.id() == QMetaType::QVariantList)
                    return variantUrls;

                if (preferredType.id() == QMetaType::QByteArray)
                {
                    QByteArray encoded;
                    for (const auto& url : *m_externalUrls)
                    {
                        encoded += url.toEncoded();
                        encoded += "\r\n";
                    }
                    return encoded;
                }

                return variantUrls;
            }

          private:
            [[nodiscard]] bool externalFilesAvailable() const
            {
                return static_cast<bool>(m_externalUrlProvider) || m_externalUrls.has_value();
            }

            mutable ExternalDragUrlProvider m_externalUrlProvider;
            mutable std::optional<QList<QUrl>> m_externalUrls;
        };
    } // namespace

    QMimeData* buildMessageDragMimeData(const QByteArray& internalPayload,
                                        ExternalDragUrlProvider externalUrlProvider)
    {
        if (internalPayload.isEmpty())
            return nullptr;
        return new MessageDragMimeData(internalPayload, std::move(externalUrlProvider));
    }

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

    void MessageDragListView::focusInEvent(QFocusEvent* event)
    {
        QListView::focusInEvent(event);
        auto* selection = selectionModel();
        if (selection == nullptr || !currentIndex().isValid() ||
            !selection->selectedRows().isEmpty())
            return;

        selection->select(currentIndex(),
                          QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }

    void MessageDragListView::setExternalFileProvider(MessageExternalFileProvider provider)
    {
        m_externalFileProvider = std::move(provider);
    }

    void MessageDragListView::startDrag(const Qt::DropActions supportedActions)
    {
        auto indexes = selectionModel()->selectedRows();
        if (indexes.isEmpty() && currentIndex().isValid())
            indexes.push_back(currentIndex());

        std::unique_ptr<QMimeData> modelMime{model()->mimeData(indexes)};
        if (modelMime == nullptr)
            return;
        const auto internalPayload = modelMime->data(QString::fromLatin1(messageDragMimeType));
        const auto decoded = decodeMessageDragPayload(internalPayload);
        if (internalPayload.isEmpty() || !decoded.has_value())
            return;

        ExternalDragUrlProvider externalUrlProvider;
        if (m_externalFileProvider)
        {
            externalUrlProvider = [provider = m_externalFileProvider, payload = *decoded]
            { return provider(payload); };
        }

        // External file data is promised from drag start but materialized only if the target asks
        // for text/uri-list. Internal mailbox targets consume only Javelin's private transfer MIME,
        // so ordinary Move/Copy drags never trigger RFC 5322 export work.
        auto* dragMimeData =
            buildMessageDragMimeData(internalPayload, std::move(externalUrlProvider));
        if (dragMimeData == nullptr)
            return;

        const QString label =
            i18np("%1 message", "%1 messages", representedMessageCountForDrag(indexes));
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

        QDrag drag{this};
        drag.setMimeData(dragMimeData);
        drag.setPixmap(badge);
        drag.setHotSpot(QPoint{-10, -10});
        static_cast<void>(drag.exec(supportedActions, defaultDropAction()));
    }

} // namespace javelin::gui::messages
