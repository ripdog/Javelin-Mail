#pragma once

#include "gui/messages/MessageDragPayload.h"

#include <QListView>
#include <QUrl>

#include <functional>

class QMimeData;

namespace javelin::gui::messages
{
    [[nodiscard]] qsizetype representedMessageCountForDrag(const QModelIndexList& indexes);

    using ExternalDragUrlProvider = std::function<QList<QUrl>()>;
    using MessageExternalFileProvider =
        std::function<QList<QUrl>(const javelin::gui::messages::MessageDragPayload&)>;

    [[nodiscard]] QMimeData*
    buildMessageDragMimeData(const QByteArray& internalPayload,
                             ExternalDragUrlProvider externalUrlProvider = {});

    class MessageDragListView final : public QListView
    {
        Q_OBJECT

      public:
        using QListView::QListView;

        void setExternalFileProvider(MessageExternalFileProvider provider);

      protected:
        void focusInEvent(QFocusEvent* event) override;
        void startDrag(Qt::DropActions supportedActions) override;

      private:
        MessageExternalFileProvider m_externalFileProvider;
    };

} // namespace javelin::gui::messages
