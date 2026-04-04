#include "gui/messageview/HtmlMessageView.h"

#include "jmap/render/InlineMessageUrl.h"

#include <QVBoxLayout>
#include <QWebEngineSettings>
#include <QWebEngineView>

namespace javelin::gui::messageview
{

    HtmlMessageView::HtmlMessageView(QWidget* parent) : QWidget(parent)
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);

        m_view = new QWebEngineView(this);
        m_view->setContextMenuPolicy(Qt::NoContextMenu);
        auto* settings = m_view->settings();
        settings->setAttribute(QWebEngineSettings::JavascriptEnabled, false);
        settings->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
        settings->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
        settings->setAttribute(QWebEngineSettings::ErrorPageEnabled, false);
        settings->setAttribute(QWebEngineSettings::PluginsEnabled, false);
        settings->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, true);

        layout->addWidget(m_view);
    }

    HtmlMessageView::~HtmlMessageView() = default;

    void HtmlMessageView::setDocumentHtml(const std::string_view html)
    {
        m_view->setHtml(QString::fromUtf8(html.data(), static_cast<qsizetype>(html.size())),
                        QUrl(QStringLiteral("%1://message/")
                                 .arg(javelin::jmap::render::inlineMessageUrlScheme())));
    }

    void HtmlMessageView::clearDocument()
    {
        m_view->setHtml(QString{});
    }

} // namespace javelin::gui::messageview
