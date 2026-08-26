#pragma once

#include "gui/messages/MessageDragPayload.h"

#include <QHash>
#include <QListView>
#include <QPointer>
#include <QUrl>

#include <cstdint>
#include <optional>

class QMimeData;

namespace javelin::gui::messages
{
    [[nodiscard]] qsizetype representedMessageCountForDrag(const QModelIndexList& indexes);

    [[nodiscard]] QMimeData* buildMessageDragMimeData(const QByteArray& internalPayload,
                                                      const QList<QUrl>& externalUrls = {},
                                                      bool advertiseExternalFiles = false);

    class MessageDragListView final : public QListView
    {
        Q_OBJECT

      public:
        using QListView::QListView;

        void setExternalFileDragEnabled(bool enabled);

      Q_SIGNALS:
        void externalDragPreparationRequested(quint64 requestId,
                                              javelin::gui::messages::MessageDragPayload payload);

      public Q_SLOTS:
        void externalDragPreparationReady(quint64 requestId, QList<QUrl> urls);
        void externalDragPreparationFailed(quint64 requestId);

      protected:
        void focusInEvent(QFocusEvent* event) override;
        void startDrag(Qt::DropActions supportedActions) override;

      private:
        bool m_externalFileDragEnabled = false;
        quint64 m_nextRequestId = 1;
        std::optional<quint64> m_activeRequestId;
        QPointer<QMimeData> m_activeDragMimeData;
        QHash<quint64, QByteArray> m_requestedPayloads;
        QByteArray m_cachedPayload;
        QList<QUrl> m_cachedUrls;
    };

} // namespace javelin::gui::messages
