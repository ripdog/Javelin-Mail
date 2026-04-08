#include "gui/messageview/HtmlMessageView.h"

#include "jmap/render/InlineMessageUrl.h"

#include <QContextMenuEvent>
#include <QMenu>
#include <QAction>
#include <QPointer>
#include <QVBoxLayout>
#include <QString>
#include <QWebEnginePage>
#include <QWebEngineContextMenuRequest>
#include <QWebEngineSettings>
#include <QWebEngineView>

#include <array>
#include <memory>

namespace javelin::gui::messageview
{

    namespace
    {

        class FilteredWebEngineView final : public QWebEngineView
        {
          public:
            explicit FilteredWebEngineView(std::function<void()> viewSourceAction,
                                           QWidget* parent)
                : QWebEngineView(parent), m_viewSourceAction(std::move(viewSourceAction))
            {
            }

          protected:
            void contextMenuEvent(QContextMenuEvent* event) override
            {
                const QPointer<QWebEngineContextMenuRequest> request = lastContextMenuRequest();
                if (request.isNull())
                {
                    return;
                }

                QMenu menu(this);
                auto* sourceAction = menu.addAction(QStringLiteral("View Source"));

                if (request->editFlags().testFlag(QWebEngineContextMenuRequest::CanCopy))
                {
                    QAction* copyAction = pageAction(QWebEnginePage::Copy);
                    if (copyAction != nullptr)
                    {
                        menu.addAction(copyAction);
                    }
                }

                if (request->mediaType() == QWebEngineContextMenuRequest::MediaTypeImage)
                {
                    QAction* copyImageAction = pageAction(QWebEnginePage::CopyImageToClipboard);
                    QAction* saveImageAction = pageAction(QWebEnginePage::DownloadImageToDisk);
                    if (copyImageAction != nullptr)
                    {
                        menu.addAction(copyImageAction);
                    }
                    if (saveImageAction != nullptr)
                    {
                        menu.addAction(saveImageAction);
                    }
                }

                if (request->linkUrl().isValid())
                {
                    QAction* copyLinkAction = pageAction(QWebEnginePage::CopyLinkToClipboard);
                    if (copyLinkAction != nullptr)
                    {
                        menu.addAction(copyLinkAction);
                    }
                }

                const QAction* chosen = menu.exec(event->globalPos());
                if (chosen == sourceAction && m_viewSourceAction)
                {
                    m_viewSourceAction();
                }
            }

          private:
            std::function<void()> m_viewSourceAction;
        };

    } // namespace

    HtmlMessageView::HtmlMessageView(QWidget* parent) : QWidget(parent)
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);

        m_view = new FilteredWebEngineView([this] { Q_EMIT viewSourceRequested(); }, this);
        auto* settings = m_view->settings();
        settings->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
        settings->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
        settings->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
        settings->setAttribute(QWebEngineSettings::ErrorPageEnabled, false);
        settings->setAttribute(QWebEngineSettings::PluginsEnabled, false);
        settings->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, true);
        connect(m_view, &QWebEngineView::loadFinished, this,
                [this](const bool)
                {
                    applyRemoteContentPolicy();
                });

        layout->addWidget(m_view);
    }

    HtmlMessageView::~HtmlMessageView()
    {
        if (m_view && m_view->page())
        {
            m_view->setPage(nullptr);
        }
    }

    void HtmlMessageView::setDocumentHtml(const std::string_view html)
    {
        m_remoteContentEnabled = false;
        m_view->setHtml(QString::fromUtf8(html.data(), static_cast<qsizetype>(html.size())),
                        QUrl(QStringLiteral("%1://message/")
                                 .arg(javelin::jmap::render::inlineMessageUrlScheme())));
    }

    void HtmlMessageView::clearDocument()
    {
        m_remoteContentEnabled = false;
        m_view->setHtml(QString{});
    }

    void HtmlMessageView::setRemoteContentEnabled(const bool enabled)
    {
        m_remoteContentEnabled = enabled;
        m_view->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls,
                                         enabled);
        applyRemoteContentPolicy();
    }

    bool HtmlMessageView::remoteContentEnabled() const
    {
        return m_remoteContentEnabled;
    }

    void HtmlMessageView::applyRemoteContentPolicy()
    {
        m_view->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls,
                                         m_remoteContentEnabled);
        m_view->page()->runJavaScript(
            QStringLiteral(
                R"JS(
(() => {
  const enabled = %1;
  document.querySelectorAll('[data-javelin-blocked-src]').forEach((element) => {
    const attr = element.getAttribute('data-javelin-remote-attr') || 'src';
    const blocked = element.getAttribute('data-javelin-blocked-src');
    if (!blocked) {
      return;
    }
    element.setAttribute(attr, enabled ? blocked : 'about:blank');
  });
  document.querySelectorAll('[data-javelin-blocked-style]').forEach((element) => {
    const enabledStyle = element.getAttribute('data-javelin-blocked-style');
    const disabledStyle = element.getAttribute('data-javelin-disabled-style') || '';
    if (!enabledStyle) {
      return;
    }
    element.setAttribute('style', enabled ? enabledStyle : disabledStyle);
  });
})();
)JS")
                .arg(m_remoteContentEnabled ? QStringLiteral("true") : QStringLiteral("false")));
    }

} // namespace javelin::gui::messageview
