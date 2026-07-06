#pragma once

#include <QWidget>

#include <QStringList>
#include <QVector>

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

        void setDocumentHtml(std::string_view html);
        void clearDocument();
        void setRemoteContentEnabled(bool enabled);
        [[nodiscard]] bool remoteContentEnabled() const;
        void collectTranslationChunks(std::function<void(QVector<QStringList>)> callback);
        void applyTranslationChunks(const QVector<QStringList>& translatedChunks);
        void restoreOriginalText();

      Q_SIGNALS:
        void viewSourceRequested();

      private:
        void applyRemoteContentPolicy();

        QWebEngineView* m_view = nullptr;
        bool m_remoteContentEnabled = false;
    };

} // namespace javelin::gui::messageview
