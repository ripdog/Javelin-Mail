#pragma once

#include <QObject>
#include <QString>

#include <functional>

class QEvent;
class QLabel;
class QLineEdit;
class QTextBrowser;
class QToolButton;
class QWidget;

namespace javelin::gui::messageview
{
    class HtmlMessageView;
    class MessageBodyPresenter;

    class MessageReaderCommandController final : public QObject
    {
        Q_OBJECT

      public:
        MessageReaderCommandController(MessageBodyPresenter& bodyPresenter,
                                       QTextBrowser& plainTextView, HtmlMessageView& htmlView,
                                       QWidget& findBarContainer, QLineEdit& findEdit,
                                       QLabel& findResultLabel, QToolButton& findPreviousButton,
                                       QToolButton& findNextButton,
                                       std::function<bool()> contentAvailable,
                                       std::function<void()> focusMessageBody,
                                       std::function<QString()> documentName, QWidget& dialogParent,
                                       QObject* parent = nullptr);

        [[nodiscard]] bool available() const;
        void showFindBar();
        void findNext();
        void findPrevious();
        void dismissFindBar();
        void resetFind(bool hideBar);
        void activeViewChanged();
        void zoomIn();
        void zoomOut();
        void resetZoom();
        void printMessage();

      private:
        void runFind(bool backwards);
        void clearFindHighlights();
        void updateFindResult(int activeMatch, int matchCount);
        void applyZoom();
        bool eventFilter(QObject* watched, QEvent* event) override;

        MessageBodyPresenter& m_bodyPresenter;
        QTextBrowser& m_plainTextView;
        HtmlMessageView& m_htmlView;
        QWidget& m_findBarContainer;
        QLineEdit& m_findEdit;
        QLabel& m_findResultLabel;
        QToolButton& m_findPreviousButton;
        QToolButton& m_findNextButton;
        std::function<bool()> m_contentAvailable;
        std::function<void()> m_focusMessageBody;
        std::function<QString()> m_documentName;
        QWidget& m_dialogParent;
        QString m_plainTextFindQuery;
        int m_plainTextFindIndex = -1;
        int m_zoomSteps = 0;
    };

} // namespace javelin::gui::messageview
