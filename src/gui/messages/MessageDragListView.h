#pragma once

#include <QListView>

namespace javelin::gui::messages
{

    class MessageDragListView final : public QListView
    {
      public:
        using QListView::QListView;

      protected:
        void focusInEvent(QFocusEvent* event) override;
        void startDrag(Qt::DropActions supportedActions) override;
    };

} // namespace javelin::gui::messages
