#pragma once

#include <QIcon>
#include <QStyledItemDelegate>

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

      private:
        [[nodiscard]] const QIcon& attachmentIcon() const;

        mutable QIcon m_attachmentIcon;
    };

} // namespace javelin::gui::messages
