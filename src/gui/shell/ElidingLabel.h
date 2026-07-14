#pragma once

#include <QLabel>

namespace javelin::gui::shell
{
    class ElidingLabel final : public QLabel
    {
      public:
        explicit ElidingLabel(QWidget* parent = nullptr);

        void setText(const QString& text);
        [[nodiscard]] QSize minimumSizeHint() const override;

      protected:
        void resizeEvent(QResizeEvent* event) override;

      private:
        void updateElidedText();

        QString m_fullText;
    };
} // namespace javelin::gui::shell
