#pragma once

#include "gui/messageview/MessageAppearance.h"

#include <QWidget>

#include <QElapsedTimer>
#include <QStringList>
#include <QUrl>
#include <QVector>

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

class QEvent;
class QWebEngineView;

namespace javelin::gui::messageview
{

    class HtmlMessageView : public QWidget
    {
        Q_OBJECT

      public:
        explicit HtmlMessageView(QWidget* parent = nullptr);
        HtmlMessageView(MessageAppearanceSettings appearanceSettings, QWidget* parent);
        ~HtmlMessageView() override;

        void setDocumentHtml(std::string_view html, std::string_view documentId = {});
        void clearDocument();
        void setRemoteContentEnabled(bool enabled);
        [[nodiscard]] bool remoteContentEnabled() const;
        void setAppearanceSettings(MessageAppearanceSettings appearanceSettings);
        void collectTranslationChunks(std::function<void(QVector<QStringList>)> callback);
        void applyTranslationChunks(const QVector<QStringList>& translatedChunks);
        void restoreOriginalText();
        void findText(const QString& text, bool backwards,
                      std::function<void(int activeMatch, int matchCount)> callback = {});
        void clearFindHighlights();
        void setZoomFactor(qreal factor);
        void printDocument(const QString& documentName = {});

      Q_SIGNALS:
        void viewSourceRequested();
        void findRequested();
        void zoomInRequested();
        void zoomOutRequested();
        void resetZoomRequested();
        void printRequested();
        void documentLoaded(QString documentId);
        void hoveredLinkChanged(QString url);

      private:
        void applyRemoteContentPolicy(std::function<void()> callback = {});
        void applyDarkModePolicy(std::function<void()> callback = {});
        void awaitRenderedDocument(const QUrl& documentUrl, const QString& readyTitle);
        void probeDocumentReady(std::uint64_t generation);
        [[nodiscard]] bool shouldUseDarkMode() const;
        void toggleDarkModeForCurrentDocument();
        void schedulePaletteRefresh();
        void updatePageBackground();
        void updateLoadingCover(bool revealForLoad);
        void changeEvent(QEvent* event) override;
        bool eventFilter(QObject* watched, QEvent* event) override;
        void installRenderEventFilter(QObject* object);
        void recordViewPaint(QObject* paintedObject);

        QWebEngineView* m_view = nullptr;
        QWidget* m_loadingCover = nullptr;
        MessageAppearanceSettings m_appearanceSettings;
        std::optional<bool> m_darkModeOverride;
        bool m_remoteContentEnabled = false;
        bool m_darkReaderLoaded = false;
        std::uint64_t m_documentGeneration = 0;
        QString m_expectedDocumentId;
        QUrl m_expectedDocumentUrl;
        QString m_expectedReadyTitle;
        QElapsedTimer m_renderTimer;
        int m_readyPaintCount = 0;
        bool m_tracePaints = false;
        bool m_waitingForSurfacePaint = false;
        bool m_documentReadyAccepted = false;
        bool m_paletteRefreshPending = false;
    };

} // namespace javelin::gui::messageview
