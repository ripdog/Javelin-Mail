#pragma once

#include <QPersistentModelIndex>
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
                         const QStyleOptionViewItem& option, const QModelIndex& index) override;

      Q_SIGNALS:
        void threadExpansionToggled(QModelIndex index);
        void attachmentButtonClicked(QModelIndex index);
        void flaggedToggled(QModelIndex index);

      private:
        enum class ButtonKind
        {
            None,
            Replies,
            Attachment,
            Star,
        };

        [[nodiscard]] ButtonKind buttonAt(const QStyleOptionViewItem& option,
                                          const QModelIndex& index, QPoint position) const;

        mutable QPersistentModelIndex m_hoveredIndex;
        mutable ButtonKind m_hoveredButton = ButtonKind::None;
        mutable QPersistentModelIndex m_pressedIndex;
        mutable ButtonKind m_pressedButton = ButtonKind::None;
    };

} // namespace javelin::gui::messages
