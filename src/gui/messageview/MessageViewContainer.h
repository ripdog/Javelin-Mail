#pragma once

#include "jmap/cache/MessageViewService.h"
#include "jmap/cache/QueryService.h"

#include <QString>
#include <QWidget>

#include <cstddef>
#include <optional>
#include <string>

class QLabel;
class QGridLayout;
class QPlainTextEdit;
class QProgressBar;
class QScrollArea;
class QResizeEvent;
class QStackedWidget;
class QToolButton;
class QVBoxLayout;
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
        void setMultipleSelection(std::optional<std::string> accountId,
                                  std::optional<std::string> mailboxId,
                                  std::vector<javelin::jmap::cache::MessageListItem> messages);
        void refresh(javelin::jmap::cache::MessageViewService& messageViewService);
        void setLoadingState(bool loading, const QString& detailText = QString{});
        void setErrorState(const QString& errorMessage);
        [[nodiscard]] bool hasContentSnapshot() const;
        [[nodiscard]] bool hasReadableBody() const;

      Q_SIGNALS:
        void saveAttachmentRequested(QString accountId, QString emailId, QString partId);
        void openAttachmentRequested(QString accountId, QString emailId, QString partId);
        void saveAllAttachmentsRequested(QString accountId, QString emailId);
        void viewSourceRequested();
        void messageActivated(QString emailId);

      private:
        enum class ActiveView
        {
            Placeholder,
            Multiple,
            PlainText,
            Html,
        };

        void setActiveView(ActiveView view);
        void updatePresentation(bool reloadBody = true);
        void updateSenderRemoteContentPermit();
        void updateRemoteContentButton();
        void updateAttachmentSection();
        void rebuildAttachmentRows();
        void rebuildMultipleSelectionRows();
        void permitRemoteContentForCurrentSender();
        void permitRemoteContentForCurrentDomain();
        [[nodiscard]] QString attachmentStatusText() const;
        [[nodiscard]] QString currentSenderAddress() const;
        [[nodiscard]] QString currentSenderDomain() const;
        void resizeEvent(QResizeEvent* event) override;

        std::optional<std::string> m_accountId;
        std::optional<std::string> m_mailboxId;
        std::optional<std::string> m_emailId;
        std::vector<javelin::jmap::cache::MessageListItem> m_multipleMessages;
        std::optional<javelin::jmap::cache::MessageViewSnapshot> m_snapshot;
        bool m_loading = false;
        QString m_errorMessage;
        QLabel* m_titleLabel = nullptr;
        QLabel* m_detailLabel = nullptr;
        QWidget* m_metadataWidget = nullptr;
        QLabel* m_fromLabel = nullptr;
        QLabel* m_toLabel = nullptr;
        QLabel* m_receivedLabel = nullptr;
        QWidget* m_placeholderPanel = nullptr;
        QLabel* m_placeholderTitleLabel = nullptr;
        QLabel* m_placeholderDetailLabel = nullptr;
        QLabel* m_remoteContentIconLabel = nullptr;
        QLabel* m_remoteContentStatusLabel = nullptr;
        QLabel* m_attachmentStatusLabel = nullptr;
        QToolButton* m_attachmentExpanderButton = nullptr;
        QWidget* m_attachmentHeaderWidget = nullptr;
        QToolButton* m_saveAllAttachmentsButton = nullptr;
        QToolButton* m_permitSenderRemoteContentButton = nullptr;
        QToolButton* m_permitDomainRemoteContentButton = nullptr;
        QToolButton* m_remoteContentButton = nullptr;
        QWidget* m_bodyControlsWidget = nullptr;
        QStackedWidget* m_bodyStack = nullptr;
        QProgressBar* m_loadingIndicator = nullptr;
        QScrollArea* m_multipleSelectionScrollArea = nullptr;
        QWidget* m_multipleSelectionWidget = nullptr;
        QVBoxLayout* m_multipleSelectionLayout = nullptr;
        QPlainTextEdit* m_plainTextView = nullptr;
        HtmlMessageView* m_htmlView = nullptr;
        QWidget* m_attachmentListWidget = nullptr;
        QGridLayout* m_attachmentListLayout = nullptr;
        ActiveView m_activeView = ActiveView::Placeholder;
        bool m_attachmentsExpanded = false;
        bool m_attachmentsCollapsed = false;
    };

} // namespace javelin::gui::messageview
