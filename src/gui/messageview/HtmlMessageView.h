#pragma once

#include <QWidget>

#include <QElapsedTimer>
#include <QStringList>
#include <QUrl>
#include <QVector>

#include <cstdint>
#include <functional>
#include <string_view>

class QWebEngineView;

namespace javelin::gui::messageview
{

    class HtmlMessageView : public QWidget
    {
        Q_OBJECT

      public:
        explicit HtmlMessageView(QWidget* parent = nullptr);
        ~HtmlMessageView() override;

        void setDocumentHtml(std::string_view html, std::string_view documentId = {});
        void clearDocument();
        void setRemoteContentEnabled(bool enabled);
        [[nodiscard]] bool remoteContentEnabled() const;
        void collectTranslationChunks(std::function<void(QVector<QStringList>)> callback);
        void applyTranslationChunks(const QVector<QStringList>& translatedChunks);
        void restoreOriginalText();

      Q_SIGNALS:
        void viewSourceRequested();
        void documentLoaded(QString documentId);
        void hoveredLinkChanged(QString url);

      private:
        void applyRemoteContentPolicy(std::function<void()> callback = {});
        void awaitRenderedDocument(const QUrl& documentUrl, const QString& readyTitle);
        void recordViewPaint();
        void traceRenderEvent(const QString& event, const QString& detail = {}) const;

        QWebEngineView* m_view = nullptr;
        bool m_remoteContentEnabled = false;
        std::uint64_t m_documentGeneration = 0;
        QString m_expectedDocumentId;
        QUrl m_expectedDocumentUrl;
        QString m_expectedReadyTitle;
        QElapsedTimer m_renderTimer;
        int m_tracedPaintCount = 0;
        int m_readyPaintCount = 0;
        bool m_tracePaints = false;
    };

} // namespace javelin::gui::messageview
