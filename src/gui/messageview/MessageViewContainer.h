#pragma once

#include "jmap/cache/MessageViewService.h"

#include <QWidget>

#include <cstddef>
#include <optional>
#include <string>

class QLabel;
class QPlainTextEdit;
class QGridLayout;
class QResizeEvent;
class QStackedWidget;
class QToolButton;
class QString;
class QWidget;

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

      Q_SIGNALS:
        void saveAttachmentRequested(QString accountId, QString emailId, QString partId);
        void openAttachmentRequested(QString accountId, QString emailId, QString partId);
        void saveAllAttachmentsRequested(QString accountId, QString emailId);

      private:
        enum class ActiveView
        {
            Placeholder,
            PlainText,
            Html,
        };

        void setActiveView(ActiveView view);
        void updatePresentation();
        void updateRemoteContentButton();
        void updateAttachmentSection();
        void rebuildAttachmentRows();
        [[nodiscard]] QString attachmentStatusText() const;
        void resizeEvent(QResizeEvent* event) override;

        std::optional<std::string> m_accountId;
        std::optional<std::string> m_mailboxId;
        std::optional<std::string> m_emailId;
        std::optional<javelin::jmap::cache::MessageViewSnapshot> m_snapshot;
        QLabel* m_titleLabel = nullptr;
        QLabel* m_detailLabel = nullptr;
        QLabel* m_remoteContentStatusLabel = nullptr;
        QLabel* m_placeholderLabel = nullptr;
        QLabel* m_attachmentStatusLabel = nullptr;
        QToolButton* m_attachmentExpanderButton = nullptr;
        QWidget* m_attachmentHeaderWidget = nullptr;
        QToolButton* m_saveAllAttachmentsButton = nullptr;
        QToolButton* m_remoteContentButton = nullptr;
        QWidget* m_bodyControlsWidget = nullptr;
        QStackedWidget* m_bodyStack = nullptr;
        QPlainTextEdit* m_plainTextView = nullptr;
        HtmlMessageView* m_htmlView = nullptr;
        QWidget* m_attachmentListWidget = nullptr;
        QGridLayout* m_attachmentListLayout = nullptr;
        ActiveView m_activeView = ActiveView::Placeholder;
        bool m_attachmentsExpanded = false;
    };

} // namespace javelin::gui::messageview
