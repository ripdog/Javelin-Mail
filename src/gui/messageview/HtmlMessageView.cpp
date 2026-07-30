#include "gui/messageview/HtmlMessageView.h"

#include "jmap/render/InlineMessageUrl.h"

#include <QAction>
#include <QChildEvent>
#include <QColor>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMenu>
#include <QPalette>
#include <QPointer>
#include <QRegularExpression>
#include <QResource>
#include <QStackedLayout>
#include <QString>
#include <QStyleHints>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>
#include <QWebEngineContextMenuRequest>
#include <QWebEnginePage>
#include <QWebEngineScript>
#include <QWebEngineSettings>
#include <QWebEngineView>

#include <array>
#include <memory>

static void initializeDarkReaderResource()
{
    Q_INIT_RESOURCE(javelin_darkreader);
}

namespace javelin::gui::messageview
{

    namespace
    {
        constexpr auto darkBackground = "#181a1b";
        constexpr auto darkText = "#e8e6e3";
        constexpr auto darkBorder = "#736b5e";
        constexpr auto darkModeBootstrapId = "__javelin-dark-mode-bootstrap";

        struct DarkReaderThemeColors
        {
            QString background;
            QString text;
            QString selection;
            QString scrollbar;
            QString border;
        };

        [[nodiscard]] QString cssColor(const QColor& color)
        {
            return color.name(QColor::HexRgb);
        }

        [[nodiscard]] DarkReaderThemeColors darkReaderThemeColors(const QPalette& palette)
        {
            const auto background = palette.color(QPalette::Base);
            if (background.lightness() >= 128)
            {
                return {
                    .background = QString::fromLatin1(darkBackground),
                    .text = QString::fromLatin1(darkText),
                    .selection = QStringLiteral("auto"),
                    .scrollbar = QStringLiteral("auto"),
                    .border = QString::fromLatin1(darkBorder),
                };
            }

            return {
                .background = cssColor(background),
                .text = cssColor(palette.color(QPalette::Text)),
                .selection = cssColor(palette.color(QPalette::Highlight)),
                .scrollbar = cssColor(palette.color(QPalette::Mid)),
                .border = cssColor(palette.color(QPalette::Mid)),
            };
        }

        [[nodiscard]] bool shouldOpenExternally(const QUrl& url)
        {
            const auto scheme = url.scheme();
            return scheme == QStringLiteral("http") || scheme == QStringLiteral("https") ||
                   scheme == QStringLiteral("mailto");
        }

        [[nodiscard]] const QString& darkReaderSource()
        {
            static const QString source = []
            {
                QFile file{QStringLiteral(":/vendor/darkreader/darkreader.js")};
                if (!file.open(QIODevice::ReadOnly))
                {
                    return QString{};
                }
                return QString::fromUtf8(file.readAll());
            }();
            return source;
        }

        [[nodiscard]] QString darkModeBootstrapStyle(const DarkReaderThemeColors& colors)
        {
            return QStringLiteral("<style id=\"%1\">html,body,body :not(iframe){"
                                  "background-color:%2!important;border-color:%4!important;"
                                  "color:%3!important}html{color-scheme:dark!important}</style>")
                .arg(QLatin1StringView{darkModeBootstrapId}, colors.background, colors.text,
                     colors.border);
        }

        [[nodiscard]] QString darkReaderEnableScript(const DarkReaderThemeColors& colors)
        {
            return QStringLiteral(R"JS(
;(() => {
  if (!globalThis.DarkReader) return false;
  globalThis.DarkReader.enable({
    mode: 1,
    brightness: 100,
    contrast: 100,
    grayscale: 0,
    sepia: 0,
    darkSchemeBackgroundColor: "%1",
    darkSchemeTextColor: "%2",
    selectionColor: "%3",
    scrollbarColor: "%4",
    styleSystemControls: true,
    immediateModify: true,
  }, {
    invert: [],
    css: "",
    ignoreInlineStyle: [],
    ignoreImageAnalysis: ["*"],
    disableStyleSheetsProxy: true,
    disableCustomElementRegistryProxy: true,
    ignoreCSSUrl: [],
  });
  document.getElementById("%5")?.remove();
  return true;
})();
)JS")
                .arg(colors.background, colors.text, colors.selection, colors.scrollbar,
                     QLatin1StringView{darkModeBootstrapId});
        }

        [[nodiscard]] QString darkReaderInitialScript(const DarkReaderThemeColors& colors)
        {
            const auto cleanReservedMarkers = QStringLiteral(R"JS(
;(() => {
  document.querySelectorAll('meta[name="darkreader"],meta[name="darkreader-lock"]')
    .forEach((element) => element.remove());
  document.querySelectorAll(".darkreader")
    .forEach((element) => element.classList.remove("darkreader"));
  document.querySelectorAll("*").forEach((element) => {
    for (const attribute of Array.from(element.attributes)) {
      if (attribute.name.startsWith("data-darkreader-")) {
        element.removeAttribute(attribute.name);
      }
    }
  });
})();
)JS");
            return cleanReservedMarkers + darkReaderSource() + darkReaderEnableScript(colors);
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
            explicit FilteredWebEngineView(std::function<void()> viewSourceAction,
                                           std::function<void()> toggleDarkModeAction,
                                           std::function<bool()> darkModeEnabled, QWidget* parent)
                : QWebEngineView(parent), m_viewSourceAction(std::move(viewSourceAction)),
                  m_toggleDarkModeAction(std::move(toggleDarkModeAction)),
                  m_darkModeEnabled(std::move(darkModeEnabled))
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
                auto* appearanceAction =
                    menu.addAction(m_darkModeEnabled && m_darkModeEnabled()
                                       ? QStringLiteral("Use Original Colours")
                                       : QStringLiteral("Use Dark Appearance"));

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
                else if (chosen == appearanceAction && m_toggleDarkModeAction)
                {
                    m_toggleDarkModeAction();
                }
            }

          private:
            std::function<void()> m_viewSourceAction;
            std::function<void()> m_toggleDarkModeAction;
            std::function<bool()> m_darkModeEnabled;
        };

    } // namespace

    HtmlMessageView::HtmlMessageView(QWidget* parent)
        : QWidget(parent), m_appearanceSettings(loadMessageAppearanceSettings())
    {
        initializeDarkReaderResource();
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);

        auto* renderSurface = new QWidget(this);
        auto* renderLayout = new QStackedLayout(renderSurface);
        renderLayout->setContentsMargins(0, 0, 0, 0);
        renderLayout->setStackingMode(QStackedLayout::StackAll);
        m_view = new FilteredWebEngineView([this] { Q_EMIT viewSourceRequested(); },
                                           [this] { toggleDarkModeForCurrentDocument(); },
                                           [this] { return shouldUseDarkMode(); }, renderSurface);
        m_view->setPage(new MessageWebEnginePage(m_view));
        renderLayout->addWidget(m_view);
        m_loadingCover = new QWidget(renderSurface);
        m_loadingCover->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_loadingCover->setAutoFillBackground(true);
        m_loadingCover->hide();
        renderLayout->addWidget(m_loadingCover);
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
        connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this,
                [this]
                {
                    updateLoadingCover(false);
                    applyDarkModePolicy();
                });
        updatePageBackground();
        layout->addWidget(renderSurface);
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
        m_darkModeOverride.reset();
        m_darkReaderLoaded = false;
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
        m_readyPaintCount = 0;
        m_tracePaints = true;
        m_waitingForSurfacePaint = false;
        m_documentReadyAccepted = false;
        updateLoadingCover(true);

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

        if (shouldUseDarkMode())
        {
            const auto colors = darkReaderThemeColors(palette());
            const QRegularExpression closingHead{QStringLiteral("</head\\s*>"),
                                                 QRegularExpression::CaseInsensitiveOption};
            const auto closingHeadMatch = closingHead.match(documentHtml);
            if (closingHeadMatch.hasMatch())
            {
                documentHtml.insert(closingHeadMatch.capturedStart(),
                                    darkModeBootstrapStyle(colors));
            }
            else
            {
                documentHtml.prepend(darkModeBootstrapStyle(colors));
            }
        }

        updatePageBackground();
        m_view->setHtml(documentHtml, m_expectedDocumentUrl);
        const auto generation = m_documentGeneration;
        QTimer::singleShot(0, this, [this, generation] { probeDocumentReady(generation); });
    }

    void HtmlMessageView::clearDocument()
    {
        m_remoteContentEnabled = false;
        m_darkModeOverride.reset();
        m_darkReaderLoaded = false;
        ++m_documentGeneration;
        m_expectedDocumentId.clear();
        m_expectedDocumentUrl = {};
        m_expectedReadyTitle.clear();
        m_tracePaints = false;
        m_waitingForSurfacePaint = false;
        m_documentReadyAccepted = false;
        m_loadingCover->hide();
        updatePageBackground();
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

    void HtmlMessageView::reloadAppearanceSettings()
    {
        m_appearanceSettings = loadMessageAppearanceSettings();
        m_darkModeOverride.reset();
        updatePageBackground();
        applyDarkModePolicy();
    }

    bool HtmlMessageView::shouldUseDarkMode() const
    {
        if (m_darkModeOverride.has_value())
        {
            return *m_darkModeOverride;
        }

        const auto baseColor = palette().color(QPalette::Base);
        return shouldUseDarkMessageColors(m_appearanceSettings.colorMode,
                                          QGuiApplication::styleHints()->colorScheme(),
                                          baseColor.lightness() < 128);
    }

    void HtmlMessageView::toggleDarkModeForCurrentDocument()
    {
        m_darkModeOverride = !shouldUseDarkMode();
        updatePageBackground();
        applyDarkModePolicy();
    }

    void HtmlMessageView::updatePageBackground()
    {
        const auto background = darkReaderThemeColors(palette()).background;
        m_view->page()->setBackgroundColor(shouldUseDarkMode() ? QColor{background}
                                                               : palette().color(QPalette::Base));
    }

    void HtmlMessageView::updateLoadingCover(const bool revealForLoad)
    {
        auto coverPalette = m_loadingCover->palette();
        const auto background = darkReaderThemeColors(palette()).background;
        coverPalette.setColor(QPalette::Window, shouldUseDarkMode()
                                                    ? QColor{background}
                                                    : palette().color(QPalette::Base));
        m_loadingCover->setPalette(coverPalette);
        if (revealForLoad || !shouldUseDarkMode())
        {
            m_loadingCover->setVisible(shouldUseDarkMode());
        }
        if (m_loadingCover->isVisible())
        {
            m_loadingCover->raise();
        }
    }

    void HtmlMessageView::changeEvent(QEvent* event)
    {
        QWidget::changeEvent(event);
        if (event->type() != QEvent::PaletteChange &&
            event->type() != QEvent::ApplicationPaletteChange)
        {
            return;
        }

        updatePageBackground();
        updateLoadingCover(false);
        applyDarkModePolicy();
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

    void HtmlMessageView::applyDarkModePolicy(std::function<void()> callback)
    {
        updatePageBackground();
        if (!m_documentReadyAccepted)
        {
            if (callback)
            {
                callback();
            }
            return;
        }

        const auto generation = m_documentGeneration;
        const QPointer<HtmlMessageView> guard{this};
        if (!shouldUseDarkMode())
        {
            if (!m_darkReaderLoaded)
            {
                m_view->page()->runJavaScript(
                    QStringLiteral("document.getElementById(\"%1\")?.remove();")
                        .arg(QLatin1StringView{darkModeBootstrapId}),
                    QWebEngineScript::ApplicationWorld,
                    [callback = std::move(callback)](const QVariant&)
                    {
                        if (callback)
                        {
                            callback();
                        }
                    });
                return;
            }

            m_view->page()->runJavaScript(QStringLiteral(R"JS(
(() => {
  globalThis.DarkReader?.disable();
  document.getElementById("%1")?.remove();
  return true;
})();
)JS")
                                              .arg(QLatin1StringView{darkModeBootstrapId}),
                                          QWebEngineScript::ApplicationWorld,
                                          [callback = std::move(callback)](const QVariant&)
                                          {
                                              if (callback)
                                              {
                                                  callback();
                                              }
                                          });
            return;
        }

        const auto& source = darkReaderSource();
        if (source.isEmpty())
        {
            if (callback)
            {
                callback();
            }
            return;
        }

        const auto colors = darkReaderThemeColors(palette());
        const auto script =
            m_darkReaderLoaded ? darkReaderEnableScript(colors) : darkReaderInitialScript(colors);
        m_view->page()->runJavaScript(
            script, QWebEngineScript::ApplicationWorld,
            [guard, generation, callback = std::move(callback)](const QVariant& result)
            {
                if (auto* view = guard.data();
                    view != nullptr && view->m_documentGeneration == generation)
                {
                    view->m_darkReaderLoaded = result.toBool();
                }
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
                auto* view = guard.data();
                if (view == nullptr || generation != view->m_documentGeneration ||
                    view->m_documentReadyAccepted)
                {
                    return;
                }

                const auto expected =
                    QStringLiteral("%1:complete").arg(static_cast<qulonglong>(generation));
                if (result.toString() == expected)
                {
                    view->m_documentReadyAccepted = true;
                    const auto documentUrl = view->m_expectedDocumentUrl;
                    const auto readyTitle = view->m_expectedReadyTitle;
                    view->applyRemoteContentPolicy(
                        [guard, documentUrl, readyTitle]
                        {
                            if (auto* activeView = guard.data())
                            {
                                activeView->applyDarkModePolicy(
                                    [guard, documentUrl, readyTitle]
                                    {
                                        if (auto* themedView = guard.data())
                                        {
                                            themedView->awaitRenderedDocument(documentUrl,
                                                                              readyTitle);
                                        }
                                    });
                            }
                        });
                    return;
                }

                if (view->m_renderTimer.elapsed() >= 30000)
                {
                    view->m_loadingCover->hide();
                    return;
                }

                QTimer::singleShot(25, view,
                                   [guard, generation]
                                   {
                                       if (auto* activeView = guard.data())
                                       {
                                           activeView->probeDocumentReady(generation);
                                       }
                                   });
            });
    }

    void HtmlMessageView::awaitRenderedDocument(const QUrl& documentUrl, const QString& readyTitle)
    {
        if (documentUrl != m_expectedDocumentUrl || readyTitle != m_expectedReadyTitle)
        {
            return;
        }

        // Crossing two animation frames gives Chromium an opportunity to submit the replacement
        // document. The first internal render-widget paint after that milestone can still contain
        // the previous framebuffer, so documentLoaded waits through two such paints.
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
                        return;
                    }

                    m_loadingCover->hide();
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

                    renderWidget->update();
                },
                Qt::QueuedConnection);
        }

        if (m_readyPaintCount >= 3)
        {
            m_tracePaints = false;
        }
    }

} // namespace javelin::gui::messageview
