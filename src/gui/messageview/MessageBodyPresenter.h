#pragma once

#include "jmap/cache/MessageViewReader.h"

#include <QString>

#include <optional>
#include <string>

class QScrollArea;
class QStackedWidget;
class QTextBrowser;
class QWidget;

namespace javelin::gui::messageview
{
    class HtmlMessageView;

    class MessageBodyPresenter final
    {
      public:
        enum class View
        {
            Placeholder,
            Multiple,
            PlainText,
            Html,
        };

        MessageBodyPresenter(QStackedWidget& stack, QWidget& placeholder,
                             QScrollArea& multipleSelection, QTextBrowser& plainText,
                             HtmlMessageView& html);

        void setActiveView(View view);
        [[nodiscard]] View activeView() const;

        void
        prepareForReload(const std::optional<javelin::jmap::cache::MessageViewSnapshot>& snapshot);
        [[nodiscard]] bool
        renderMessageBody(const javelin::jmap::cache::MessageViewSnapshot& snapshot,
                          const std::optional<std::string>& accountId,
                          const std::optional<std::string>& emailId, bool reloadBody);

        [[nodiscard]] bool acceptHtmlDocumentLoaded(const QString& documentId,
                                                    const std::optional<std::string>& accountId,
                                                    const std::optional<std::string>& emailId);
        [[nodiscard]] bool htmlDocumentLoaded() const;

      private:
        QStackedWidget& m_stack;
        QWidget& m_placeholder;
        QScrollArea& m_multipleSelection;
        QTextBrowser& m_plainText;
        HtmlMessageView& m_html;
        View m_activeView = View::Placeholder;
        bool m_htmlDocumentLoaded = false;
    };
} // namespace javelin::gui::messageview
