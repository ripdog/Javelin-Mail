#include "gui/messageview/HtmlMessageView.h"

#include "jmap/render/InlineMessageUrl.h"

#include <QAction>
#include <QChildEvent>
#include <QContextMenuEvent>
#include <QCryptographicHash>
#include <QDebug>
#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMenu>
#include <QPointer>
#include <QRegularExpression>
#include <QString>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>
#include <QWebEngineContextMenuRequest>
#include <QWebEngineLoadingInfo>
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

        [[nodiscard]] QString summarizeUrl(const QUrl& url)
        {
            const auto encoded = url.toEncoded();
            if (url.scheme() != QStringLiteral("data") && encoded.size() <= 256)
            {
                return QString::fromUtf8(encoded);
            }

            const auto digest =
                QCryptographicHash::hash(encoded, QCryptographicHash::Sha256).toHex().left(16);
            return QStringLiteral("%1:[bytes=%2 sha256=%3]")
                .arg(url.scheme())
                .arg(encoded.size())
                .arg(QString::fromLatin1(digest));
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
        installRenderEventFilter(m_view);
        connect(m_view->page(), &QWebEnginePage::linkHovered, this,
                [this](const QString& url) { Q_EMIT hoveredLinkChanged(url); });
        connect(m_view->page(), &QWebEnginePage::titleChanged, this,
                [this](const QString& title)
                {
                    if (m_expectedReadyTitle.isEmpty() || title != m_expectedReadyTitle)
                    {
                        return;
                    }

                    traceRenderEvent(QStringLiteral("render-ready-title"));
                    m_expectedReadyTitle.clear();
                    m_waitingForSurfacePaint = true;
                });
        auto* settings = m_view->settings();
        settings->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
        settings->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
        settings->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
        settings->setAttribute(QWebEngineSettings::ErrorPageEnabled, false);
        settings->setAttribute(QWebEngineSettings::PluginsEnabled, false);
        settings->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, true);
        connect(m_view->page(), &QWebEnginePage::loadingChanged, this,
                [this](const QWebEngineLoadingInfo& loadingInfo)
                {
                    auto detail = QStringLiteral("status=%1 matches=%2")
                                      .arg(static_cast<int>(loadingInfo.status()))
                                      .arg(loadingInfo.url() == m_expectedDocumentUrl);
                    detail += QStringLiteral(" eventUrl=") + summarizeUrl(loadingInfo.url());
                    detail += QStringLiteral(" expected=") + summarizeUrl(m_expectedDocumentUrl);
                    traceRenderEvent(QStringLiteral("loading-changed"), detail);
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

    void HtmlMessageView::setDocumentHtml(const std::string_view html,
                                          const std::string_view documentId)
    {
        m_remoteContentEnabled = false;
        m_expectedDocumentId =
            QString::fromUtf8(documentId.data(), static_cast<qsizetype>(documentId.size()));
        m_expectedDocumentUrl = QUrl(
            QStringLiteral("%1://message/").arg(javelin::jmap::render::inlineMessageUrlScheme()));
        m_expectedDocumentUrl.setFragment(QStringLiteral("%1:%2").arg(
            m_expectedDocumentId,
            QString::number(static_cast<qulonglong>(++m_documentGeneration))));
        m_expectedReadyTitle = QStringLiteral("__javelin_render_ready_%1")
                                   .arg(static_cast<qulonglong>(m_documentGeneration));
        m_renderTimer.restart();
        m_tracedPaintCount = 0;
        m_readyPaintCount = 0;
        m_tracePaints = true;
        m_waitingForSurfacePaint = false;
        m_documentReadyAccepted = false;
        traceRenderEvent(QStringLiteral("navigation-requested"),
                         QStringLiteral("htmlBytes=%1").arg(static_cast<qulonglong>(html.size())));

        auto documentHtml = QString::fromUtf8(html.data(), static_cast<qsizetype>(html.size()));
        const auto generationMarker =
            QStringLiteral("<meta name=\"javelin-document-generation\" content=\"%1\">")
                .arg(static_cast<qulonglong>(m_documentGeneration));
        const QRegularExpression headElement{QStringLiteral("<head\\b[^>]*>"),
                                             QRegularExpression::CaseInsensitiveOption};
        const auto headMatch = headElement.match(documentHtml);
        if (headMatch.hasMatch())
        {
            documentHtml.insert(headMatch.capturedEnd(), generationMarker);
        }
        else
        {
            documentHtml.prepend(generationMarker);
        }

        m_view->setHtml(documentHtml, m_expectedDocumentUrl);
        const auto generation = m_documentGeneration;
        QTimer::singleShot(0, this, [this, generation] { probeDocumentReady(generation); });
    }

    void HtmlMessageView::clearDocument()
    {
        m_remoteContentEnabled = false;
        ++m_documentGeneration;
        m_expectedDocumentId.clear();
        m_expectedDocumentUrl = {};
        m_expectedReadyTitle.clear();
        m_tracePaints = false;
        m_waitingForSurfacePaint = false;
        m_documentReadyAccepted = false;
        m_view->setUrl(QUrl{QStringLiteral("about:blank")});
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

    void HtmlMessageView::applyRemoteContentPolicy(std::function<void()> callback)
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
                .arg(m_remoteContentEnabled ? QStringLiteral("true") : QStringLiteral("false")),
            [callback = std::move(callback)](const QVariant&)
            {
                if (callback)
                {
                    callback();
                }
            });
    }

    void HtmlMessageView::probeDocumentReady(const std::uint64_t generation)
    {
        if (generation != m_documentGeneration || m_documentReadyAccepted)
        {
            return;
        }

        const QPointer<HtmlMessageView> guard{this};
        m_view->page()->runJavaScript(
            QStringLiteral(
                R"JS(
(() => {
  const marker = document.querySelector('meta[name="javelin-document-generation"]');
  return `${marker ? marker.content : ''}:${document.readyState}`;
})();
)JS"),
            [guard, generation](const QVariant& result)
            {
                if (!guard || generation != guard->m_documentGeneration ||
                    guard->m_documentReadyAccepted)
                {
                    return;
                }

                const auto expected =
                    QStringLiteral("%1:complete").arg(static_cast<qulonglong>(generation));
                if (result.toString() == expected)
                {
                    guard->m_documentReadyAccepted = true;
                    guard->traceRenderEvent(QStringLiteral("document-ready-probe"));
                    const auto documentUrl = guard->m_expectedDocumentUrl;
                    const auto readyTitle = guard->m_expectedReadyTitle;
                    guard->applyRemoteContentPolicy(
                        [guard, documentUrl, readyTitle]
                        {
                            if (guard)
                            {
                                guard->awaitRenderedDocument(documentUrl, readyTitle);
                            }
                        });
                    return;
                }

                if (guard->m_renderTimer.elapsed() >= 30000)
                {
                    guard->traceRenderEvent(QStringLiteral("document-ready-probe-timeout"),
                                            QStringLiteral("observed=%1").arg(result.toString()));
                    return;
                }

                QTimer::singleShot(25, guard,
                                   [guard, generation]
                                   {
                                       if (guard)
                                       {
                                           guard->probeDocumentReady(generation);
                                       }
                                   });
            });
    }

    void HtmlMessageView::awaitRenderedDocument(const QUrl& documentUrl, const QString& readyTitle)
    {
        if (documentUrl != m_expectedDocumentUrl || readyTitle != m_expectedReadyTitle)
        {
            traceRenderEvent(QStringLiteral("render-wait-discarded"));
            return;
        }

        // LoadSucceededStatus only covers navigation. Keeping the view visible behind the native
        // overlay and crossing two animation frames gives Chromium an opportunity to submit the
        // replacement document. The first internal render-widget paint after that milestone can
        // still contain the previous framebuffer, so the overlay remains through two such paints.
        traceRenderEvent(QStringLiteral("render-wait-scheduled"));
        m_view->page()->runJavaScript(QStringLiteral(
                                          R"JS(
(() => {
  const readyTitle = "%1";
  requestAnimationFrame(() => {
    requestAnimationFrame(() => {
      document.title = readyTitle;
    });
  });
})();
)JS")
                                          .arg(readyTitle));
    }

    bool HtmlMessageView::eventFilter(QObject* watched, QEvent* event)
    {
        if (event->type() == QEvent::ChildAdded)
        {
            const auto* childEvent = static_cast<QChildEvent*>(event);
            if (childEvent->child() != nullptr)
            {
                installRenderEventFilter(childEvent->child());
            }
        }
        else if (event->type() == QEvent::Paint)
        {
            recordViewPaint(watched);
        }

        return QWidget::eventFilter(watched, event);
    }

    void HtmlMessageView::installRenderEventFilter(QObject* object)
    {
        auto* widget = qobject_cast<QWidget*>(object);
        if (widget == nullptr)
        {
            return;
        }

        widget->installEventFilter(this);
        const auto children = widget->children();
        for (QObject* child : children)
        {
            installRenderEventFilter(child);
        }
    }

    void HtmlMessageView::recordViewPaint(QObject* paintedObject)
    {
        if (!m_tracePaints)
        {
            return;
        }

        const bool readyWasReported = m_expectedReadyTitle.isEmpty();
        const bool isRenderSurface = paintedObject->inherits("QQuickWidget");
        if (readyWasReported && isRenderSurface)
        {
            ++m_readyPaintCount;
        }

        if (m_tracedPaintCount < 8 || (readyWasReported && m_readyPaintCount <= 3))
        {
            ++m_tracedPaintCount;
            traceRenderEvent(
                QStringLiteral("view-painted"),
                QStringLiteral("paint=%1 readyPaint=%2 readyReported=%3 object=%4")
                    .arg(m_tracedPaintCount)
                    .arg(m_readyPaintCount)
                    .arg(readyWasReported)
                    .arg(QString::fromLatin1(paintedObject->metaObject()->className())));
        }

        if (m_waitingForSurfacePaint && isRenderSurface && m_readyPaintCount >= 2)
        {
            m_waitingForSurfacePaint = false;
            const auto generation = m_documentGeneration;
            const auto documentId = m_expectedDocumentId;
            const auto documentUrl = m_expectedDocumentUrl;
            QMetaObject::invokeMethod(
                this,
                [this, generation, documentId, documentUrl]
                {
                    if (generation != m_documentGeneration || documentId != m_expectedDocumentId ||
                        documentUrl != m_expectedDocumentUrl)
                    {
                        traceRenderEvent(QStringLiteral("surface-paint-release-discarded"));
                        return;
                    }

                    traceRenderEvent(QStringLiteral("surface-paint-ready"),
                                     QStringLiteral("readyPaints=%1").arg(m_readyPaintCount));
                    Q_EMIT documentLoaded(documentId);
                },
                Qt::QueuedConnection);
        }
        else if (m_waitingForSurfacePaint && isRenderSurface && m_readyPaintCount == 1)
        {
            const auto generation = m_documentGeneration;
            const QPointer<QWidget> renderWidget{qobject_cast<QWidget*>(paintedObject)};
            QMetaObject::invokeMethod(
                this,
                [this, generation, renderWidget]
                {
                    if (generation != m_documentGeneration || !m_waitingForSurfacePaint ||
                        renderWidget.isNull())
                    {
                        return;
                    }

                    traceRenderEvent(QStringLiteral("surface-repaint-requested"));
                    renderWidget->update();
                },
                Qt::QueuedConnection);
        }

        if (m_readyPaintCount >= 3)
        {
            m_tracePaints = false;
        }
    }

    void HtmlMessageView::traceRenderEvent(const QString& event, const QString& detail) const
    {
        auto documentId = m_expectedDocumentId;
        documentId.replace(QLatin1Char('\n'), QLatin1Char('/'));
        qInfo().noquote() << QStringLiteral(
                                 "HTML message render generation=%1 elapsedMs=%2 event=%3 "
                                 "document=%4 viewVisible=%5 pageVisible=%6 url=%7 %8")
                                 .arg(static_cast<qulonglong>(m_documentGeneration))
                                 .arg(m_renderTimer.isValid() ? m_renderTimer.elapsed() : -1)
                                 .arg(event, documentId)
                                 .arg(m_view->isVisible())
                                 .arg(m_view->page()->isVisible())
                                 .arg(summarizeUrl(m_view->url()), detail);
    }

} // namespace javelin::gui::messageview
