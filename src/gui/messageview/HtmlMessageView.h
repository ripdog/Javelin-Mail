#pragma once

#include <QWidget>

#include <string_view>

class QWebEngineView;

namespace javelin::gui::messageview
{

    class HtmlMessageView : public QWidget
    {
      public:
        explicit HtmlMessageView(QWidget* parent = nullptr);
        ~HtmlMessageView() override;

        void setDocumentHtml(std::string_view html);
        void clearDocument();

      private:
        QWebEngineView* m_view = nullptr;
    };

} // namespace javelin::gui::messageview
