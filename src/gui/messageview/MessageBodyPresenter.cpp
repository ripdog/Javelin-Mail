#include "gui/messageview/MessageBodyPresenter.h"

#include "gui/messageview/HtmlMessageView.h"
#include "gui/messageview/PlainTextLinkifier.h"

#include <QScrollArea>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QWidget>

namespace javelin::gui::messageview
{
    MessageBodyPresenter::MessageBodyPresenter(QStackedWidget& stack, QWidget& placeholder,
                                               QScrollArea& multipleSelection,
                                               QTextBrowser& plainText, HtmlMessageView& html)
        : m_stack(stack), m_placeholder(placeholder), m_multipleSelection(multipleSelection),
          m_plainText(plainText), m_html(html)
    {
    }

    void MessageBodyPresenter::setActiveView(const View view)
    {
        m_activeView = view;
        switch (m_activeView)
        {
        case View::Placeholder:
            m_stack.setCurrentWidget(&m_placeholder);
            break;
        case View::Multiple:
            m_stack.setCurrentWidget(&m_multipleSelection);
            break;
        case View::PlainText:
            m_stack.setCurrentWidget(&m_plainText);
            break;
        case View::Html:
            m_stack.setCurrentWidget(&m_html);
            break;
        }
    }

    MessageBodyPresenter::View MessageBodyPresenter::activeView() const
    {
        return m_activeView;
    }

    void MessageBodyPresenter::prepareForReload(
        const std::optional<javelin::jmap::cache::MessageViewSnapshot>& snapshot)
    {
        m_plainText.clear();
        if (!snapshot.has_value() || !snapshot->htmlBody.has_value())
            m_html.clearDocument();
        m_htmlDocumentLoaded = false;
    }

    bool MessageBodyPresenter::renderMessageBody(
        const javelin::jmap::cache::MessageViewSnapshot& snapshot,
        const std::optional<std::string>& accountId, const std::optional<std::string>& emailId,
        const bool reloadBody)
    {
        if (reloadBody && snapshot.plainTextBody.has_value())
        {
            m_plainText.setHtml(
                linkifyPlainText(QString::fromStdString(snapshot.plainTextBody->value)));
        }

        if (reloadBody && snapshot.htmlBody.has_value() && accountId.has_value() &&
            emailId.has_value())
        {
            const auto renderDocument =
                snapshot.htmlRenderDocument.has_value()
                    ? QString::fromStdString(snapshot.htmlRenderDocument->html)
                    : QString::fromStdString(snapshot.htmlBody->value);
            m_htmlDocumentLoaded = false;
            m_html.setDocumentHtml(renderDocument.toStdString(), *accountId + "\n" + *emailId);
            return true;
        }
        return false;
    }

    bool MessageBodyPresenter::acceptHtmlDocumentLoaded(const QString& documentId,
                                                        const std::optional<std::string>& accountId,
                                                        const std::optional<std::string>& emailId)
    {
        if (!accountId.has_value() || !emailId.has_value() ||
            documentId != QString::fromStdString(*accountId + "\n" + *emailId))
            return false;
        m_htmlDocumentLoaded = true;
        return true;
    }

    bool MessageBodyPresenter::htmlDocumentLoaded() const
    {
        return m_htmlDocumentLoaded;
    }
} // namespace javelin::gui::messageview
