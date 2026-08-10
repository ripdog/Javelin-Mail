#pragma once

#include <QColor>
#include <QPointer>
#include <QToolButton>

class QResizeEvent;
class QWidget;

namespace javelin::gui::calendar
{
    class CalendarEventButton final : public QToolButton
    {
        Q_OBJECT

      public:
        explicit CalendarEventButton(QWidget* parent = nullptr);

        void setEventPresentation(QString visualText, QString accessibleText, QColor color,
                                  bool segmentBegins = true, bool segmentEnds = true);
        void setControlledWidget(QWidget* widget);
        [[nodiscard]] QWidget* controlledWidget() const;

      protected:
        void resizeEvent(QResizeEvent* event) override;

      private:
        void applyPresentation();

        QString m_fullText;
        QColor m_color;
        bool m_segmentBegins = true;
        bool m_segmentEnds = true;
        QPointer<QWidget> m_controlledWidget;
    };
} // namespace javelin::gui::calendar
