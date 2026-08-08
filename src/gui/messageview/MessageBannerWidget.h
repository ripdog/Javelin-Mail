#pragma once

#include <QWidget>

class QHBoxLayout;
class QIcon;
class QLabel;
class QString;
class QToolButton;

namespace javelin::gui::messageview
{
    class MessageBannerWidget final : public QWidget
    {
        Q_OBJECT

      public:
        explicit MessageBannerWidget(QWidget* parent = nullptr);

        void setIcon(const QIcon& icon);
        void setText(const QString& text);
        [[nodiscard]] QToolButton* addButton(const QString& text);

      Q_SIGNALS:
        void dismissed();

      private:
        QHBoxLayout* m_layout = nullptr;
        QLabel* m_iconLabel = nullptr;
        QLabel* m_textLabel = nullptr;
        QToolButton* m_closeButton = nullptr;
    };
} // namespace javelin::gui::messageview
