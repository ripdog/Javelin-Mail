#include "gui/messageview/HtmlMessageView.h"

#include "jmap/render/InlineMessageUrl.h"

#include <QAction>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMenu>
#include <QPointer>
#include <QString>
#include <QVBoxLayout>
#include <QWebEngineContextMenuRequest>
#include <QWebEnginePage>
#include <QWebEngineSettings>
#include <QWebEngineView>

#include <array>
#include <memory>

namespace javelin::gui::messageview
{

    namespace
    {
        [[nodiscard]] bool shouldOpenExternally(const QUrl& url)
        {
            const auto scheme = url.scheme();
            return scheme == QStringLiteral("http") || scheme == QStringLiteral("https") ||
                   scheme == QStringLiteral("mailto");
        }

        class ExternalNavigationPage final : public QWebEnginePage
        {
          public:
            using QWebEnginePage::QWebEnginePage;

          protected:
            bool acceptNavigationRequest(const QUrl& url, NavigationType, bool) override
            {
                if (shouldOpenExternally(url))
                {
                    QDesktopServices::openUrl(url);
                }
                deleteLater();
                return false;
            }
        };

        class MessageWebEnginePage final : public QWebEnginePage
        {
          public:
            using QWebEnginePage::QWebEnginePage;

          protected:
            QWebEnginePage* createWindow(WebWindowType) override
            {
                return new ExternalNavigationPage(this);
            }

            bool acceptNavigationRequest(const QUrl& url, NavigationType type,
                                         bool isMainFrame) override
            {
                if (type == QWebEnginePage::NavigationTypeLinkClicked && isMainFrame)
                {
                    if (shouldOpenExternally(url))
                    {
                        QDesktopServices::openUrl(url);
                    }
                    return false;
                }
                return QWebEnginePage::acceptNavigationRequest(url, type, isMainFrame);
            }
        };

        class FilteredWebEngineView final : public QWebEngineView
        {
          public:
            explicit FilteredWebEngineView(std::function<void()> viewSourceAction, QWidget* parent)
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
        m_view->setPage(new MessageWebEnginePage(m_view));
        connect(m_view->page(), &QWebEnginePage::linkHovered, this,
                [this](const QString& url) { Q_EMIT hoveredLinkChanged(url); });
        auto* settings = m_view->settings();
        settings->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
        settings->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
        settings->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
        settings->setAttribute(QWebEngineSettings::ErrorPageEnabled, false);
        settings->setAttribute(QWebEngineSettings::PluginsEnabled, false);
        settings->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, true);
        connect(m_view, &QWebEngineView::loadFinished, this,
                [this](const bool ok)
                {
                    applyRemoteContentPolicy();
                    if (ok)
                    {
                        Q_EMIT documentLoaded();
                    }
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

    void
    HtmlMessageView::collectTranslationChunks(std::function<void(QVector<QStringList>)> callback)
    {
        m_view->page()->runJavaScript(QStringLiteral(R"JS(
(() => {
  window.__javelinTranslation = window.__javelinTranslation || {
    translated: false,
    nodesToRestore: [],
    chunks: [],
    activeToken: 0,
  };
  const state = window.__javelinTranslation;

  const skippedElementNames = new Set([
    'SCRIPT', 'STYLE', 'NOSCRIPT', 'TEMPLATE', 'CODE',
    'TEXTAREA', 'INPUT', 'SELECT', 'OPTION',
  ]);

  function isPlainTextMessageElement(element) {
    if (!element || element.nodeType !== Node.ELEMENT_NODE) return false;
    if (element.closest('.moz-text-plain')) return true;
    const body = document.body;
    if (!body) return false;
    const bodyChildren = Array.from(body.children).filter((child) => child.nodeName !== 'BR');
    return bodyChildren.length === 1 && bodyChildren[0] === element;
  }

  function shouldSkipElement(element) {
    if (!element || element.nodeType !== Node.ELEMENT_NODE) return false;
    if (skippedElementNames.has(element.nodeName)) return true;
    if (element.nodeName === 'PRE' && !isPlainTextMessageElement(element)) return true;
    if (element.closest("[translate='no'], .notranslate")) return true;
    return false;
  }

  function collectTextNodes(root) {
    const nodes = [];
    const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT, {
      acceptNode(node) {
        if (!node.textContent || !node.textContent.trim()) return NodeFilter.FILTER_REJECT;
        if (shouldSkipElement(node.parentElement)) return NodeFilter.FILTER_REJECT;
        return NodeFilter.FILTER_ACCEPT;
      },
    });
    while (walker.nextNode()) nodes.push(walker.currentNode);
    return nodes;
  }

  function chunkNodes(nodes) {
    const chunks = [];
    let chunk = [];
    let size = 0;
    for (const node of nodes) {
      const length = node.textContent.length;
      if (chunk.length > 0 && size + length > 1200) {
        chunks.push(chunk);
        chunk = [];
        size = 0;
      }
      chunk.push(node);
      size += length;
    }
    if (chunk.length > 0) chunks.push(chunk);
    return chunks;
  }

  for (const item of state.nodesToRestore) {
    if (item.node) item.node.textContent = item.originalText;
  }
  state.nodesToRestore = [];
  state.translated = false;
  state.activeToken++;
  document.documentElement.removeAttribute('data-javelin-translation-state');

  state.chunks = chunkNodes(collectTextNodes(document.body || document.documentElement));
  return state.chunks.map((chunk) => chunk.map((node) => node.textContent));
})();
)JS"),
                                      [callback = std::move(callback)](const QVariant& result)
                                      {
                                          QVector<QStringList> chunks;
                                          const auto chunkValues = result.toList();
                                          chunks.reserve(chunkValues.size());
                                          for (const auto& chunkValue : chunkValues)
                                          {
                                              QStringList texts;
                                              const auto textValues = chunkValue.toList();
                                              texts.reserve(textValues.size());
                                              for (const auto& textValue : textValues)
                                              {
                                                  texts.push_back(textValue.toString());
                                              }
                                              chunks.push_back(std::move(texts));
                                          }
                                          callback(std::move(chunks));
                                      });
    }

    void HtmlMessageView::applyTranslationChunks(const QVector<QStringList>& translatedChunks)
    {
        QJsonArray chunks;
        for (const auto& translatedChunk : translatedChunks)
        {
            QJsonArray chunk;
            for (const auto& text : translatedChunk)
            {
                chunk.push_back(text);
            }
            chunks.push_back(chunk);
        }

        const auto json = QString::fromUtf8(QJsonDocument{chunks}.toJson(QJsonDocument::Compact));
        m_view->page()->runJavaScript(QStringLiteral(R"JS(
((translatedChunks) => {
  const state = window.__javelinTranslation;
  if (!state || !state.chunks) return false;
  state.translated = true;
  document.documentElement.setAttribute('data-javelin-translation-state', 'translated');
  for (let i = 0; i < state.chunks.length; i++) {
    for (let j = 0; j < state.chunks[i].length; j++) {
      const node = state.chunks[i][j];
      const translated = translatedChunks[i] && translatedChunks[i][j];
      if (!node || translated === undefined || translated === null) continue;
      state.nodesToRestore.push({ node, originalText: node.textContent });
      node.textContent = translated;
    }
  }
  return true;
})(%1);
)JS")
                                          .arg(json));
    }

    void HtmlMessageView::restoreOriginalText()
    {
        m_view->page()->runJavaScript(QStringLiteral(R"JS(
(() => {
  const state = window.__javelinTranslation;
  if (!state) return false;
  state.activeToken++;
  for (const item of state.nodesToRestore || []) {
    if (item.node) item.node.textContent = item.originalText;
  }
  state.nodesToRestore = [];
  state.translated = false;
  document.documentElement.removeAttribute('data-javelin-translation-state');
  return true;
})();
)JS"));
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
