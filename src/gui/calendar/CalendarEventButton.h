#pragma once

#include <QColor>
#include <QPointer>
#include <QToolButton>

class QDate;
class QDateTime;
class QContextMenuEvent;
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
        void setCalendarEventPresentation(const QString& title, const QDateTime& start,
                                          const QDateTime& end, bool allDay, bool recurring,
                                          const QDate& displayDate, QString accessibleText,
                                          QColor color);
        void setControlledWidget(QWidget* widget);
        [[nodiscard]] QWidget* controlledWidget() const;

      Q_SIGNALS:
        void contextMenuRequested(QPoint globalPosition);

      protected:
        void contextMenuEvent(QContextMenuEvent* event) override;
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
