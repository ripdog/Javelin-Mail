#pragma once

#include "jmap/cache/MessageViewService.h"

#include <QWidget>

#include <cstddef>
#include <optional>
#include <string>

class QLabel;
class QPlainTextEdit;
class QStackedWidget;
class QToolButton;
class QString;

namespace javelin::gui::messageview
{
    class HtmlMessageView;

    class MessageViewContainer : public QWidget
    {
        Q_OBJECT

      public:
        explicit MessageViewContainer(QWidget* parent = nullptr);
        ~MessageViewContainer() override;

        void setSelection(javelin::jmap::cache::MessageViewService& messageViewService,
                          std::optional<std::string> accountId,
                          std::optional<std::string> mailboxId, std::optional<std::string> emailId);
        void refresh(javelin::jmap::cache::MessageViewService& messageViewService);

      private:
        enum class ActiveView
        {
            Placeholder,
            PlainText,
            Html,
        };

        void setActiveView(ActiveView view);
        void updateBodyButtons();
        void updatePresentation();
        [[nodiscard]] QString attachmentSummaryText() const;

        std::optional<std::string> m_accountId;
        std::optional<std::string> m_mailboxId;
        std::optional<std::string> m_emailId;
        std::optional<javelin::jmap::cache::MessageViewSnapshot> m_snapshot;
        QLabel* m_titleLabel = nullptr;
        QLabel* m_detailLabel = nullptr;
        QLabel* m_placeholderLabel = nullptr;
        QLabel* m_attachmentLabel = nullptr;
        QToolButton* m_plainTextButton = nullptr;
        QToolButton* m_htmlButton = nullptr;
        QStackedWidget* m_bodyStack = nullptr;
        QPlainTextEdit* m_plainTextView = nullptr;
        HtmlMessageView* m_htmlView = nullptr;
        ActiveView m_activeView = ActiveView::Placeholder;
    };

} // namespace javelin::gui::messageview
