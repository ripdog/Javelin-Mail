#pragma once

#include <QWidget>

class QEvent;
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
        void setButtonHoverText(QToolButton* button, const QString& text);

      Q_SIGNALS:
        void dismissed();

      private:
        bool eventFilter(QObject* watched, QEvent* event) override;

        QHBoxLayout* m_layout = nullptr;
        QLabel* m_iconLabel = nullptr;
        QLabel* m_textLabel = nullptr;
        QLabel* m_previewLabel = nullptr;
        QToolButton* m_closeButton = nullptr;
        QObject* m_previewSource = nullptr;
    };
} // namespace javelin::gui::messageview
