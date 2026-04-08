#pragma once

#include <QIcon>
#include <QStyledItemDelegate>

class QEvent;
class QModelIndex;

namespace javelin::gui::messages
{

    class MessageListDelegate : public QStyledItemDelegate
    {
        Q_OBJECT

      public:
        explicit MessageListDelegate(QObject* parent = nullptr);
        ~MessageListDelegate() override;

        void paint(QPainter* painter, const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;
        [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option,
                                     const QModelIndex& index) const override;
        bool editorEvent(QEvent* event, QAbstractItemModel* model,
                         const QStyleOptionViewItem& option,
                         const QModelIndex& index) override;

      Q_SIGNALS:
        void threadExpansionToggled(QModelIndex index);

      private:
        [[nodiscard]] const QIcon& attachmentIcon() const;

        mutable QIcon m_attachmentIcon;
    };

} // namespace javelin::gui::messages
