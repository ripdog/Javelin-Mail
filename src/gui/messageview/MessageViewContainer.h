#pragma once

#include "jmap/cache/MessageViewReader.h"
#include "jmap/cache/QueryReader.h"

#include <QString>
#include <QWidget>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

class QLabel;
class QEvent;
class QGridLayout;
class QProgressBar;
class QScrollArea;
class QResizeEvent;
class QStackedWidget;
class QTextBrowser;
class QToolButton;
class QVBoxLayout;
class QWidget;

namespace javelin::app
{
    class TranslationService;
}
namespace javelin::jmap::contacts
{
    class ContactIdentityLookup;
}

namespace javelin::gui::messageview
{
    class HtmlMessageView;

    class MessageViewContainer : public QWidget
    {
        Q_OBJECT

      public:
        explicit MessageViewContainer(
            javelin::app::TranslationService& translationService,
            javelin::jmap::contacts::ContactIdentityLookup& contactIdentityLookup,
            QWidget* parent = nullptr);
        ~MessageViewContainer() override;

        void setSelection(javelin::jmap::cache::MessageViewReader& messageViewReader,
                          std::optional<std::string> accountId,
                          std::optional<std::string> mailboxId, std::optional<std::string> emailId);
        void setMultipleSelection(std::optional<std::string> accountId,
                                  std::optional<std::string> mailboxId,
                                  std::vector<javelin::jmap::cache::MessageListItem> messages);
        void refresh(javelin::jmap::cache::MessageViewReader& messageViewReader);
        void setErrorState(const QString& errorMessage);
        void appearanceSettingsChanged();
        void translationSettingsChanged();

      Q_SIGNALS:
        void saveAttachmentRequested(QString accountId, QString emailId, QString partId);
        void openAttachmentRequested(QString accountId, QString emailId, QString partId);
        void saveAllAttachmentsRequested(QString accountId, QString emailId);
        void viewSourceRequested();
        void messageActivated(QString emailId);
        void hoveredLinkChanged(QString url);
        void contentRequired(QString accountId, QString emailId);

      private:
        enum class ActiveView
        {
            Placeholder,
            Multiple,
            PlainText,
            Html,
        };

        void setActiveView(ActiveView view);
        void startSnapshotLoad(javelin::jmap::cache::MessageViewReader& messageViewReader,
                               bool requestContentIfMissing);
        void updatePresentation(bool reloadBody = true);
        void updateSenderRemoteContentPermit();
        void updateRemoteContentButton();
        void updateLanguageBanner();
        void startLanguageDetection();
        void translateCurrentMessage();
        void translateCurrentMessage(bool automatic, bool allowNetwork);
        void restoreCurrentTranslation();
        void updateTranslateOptionsMenu();
        void setAutoTranslateSender(bool enabled);
        void setAutoTranslateDomain(bool enabled);
        void maybeAutoTranslateCurrentMessage();
        void updateAttachmentSection();
        void rebuildAttachmentRows();
        void rebuildMultipleSelectionRows();
        void permitRemoteContentForCurrentSender();
        void permitRemoteContentForCurrentDomain();
        [[nodiscard]] QString attachmentStatusText() const;
        [[nodiscard]] QString currentSenderAddress() const;
        [[nodiscard]] QString currentSenderDomain() const;
        [[nodiscard]] QString contactAwareSenderLabel() const;
        void changeEvent(QEvent* event) override;
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
        QWidget* m_languageBannerWidget = nullptr;
        QLabel* m_languageStatusLabel = nullptr;
        QToolButton* m_translateButton = nullptr;
        QToolButton* m_translateOptionsButton = nullptr;
        javelin::app::TranslationService& m_translationService;
        javelin::jmap::contacts::ContactIdentityLookup& m_contactIdentityLookup;
        QWidget* m_bodyControlsWidget = nullptr;
        QStackedWidget* m_bodyStack = nullptr;
        QProgressBar* m_loadingIndicator = nullptr;
        QScrollArea* m_multipleSelectionScrollArea = nullptr;
        QWidget* m_multipleSelectionWidget = nullptr;
        QVBoxLayout* m_multipleSelectionLayout = nullptr;
        QTextBrowser* m_plainTextView = nullptr;
        HtmlMessageView* m_htmlView = nullptr;
        QWidget* m_attachmentListWidget = nullptr;
        QGridLayout* m_attachmentListLayout = nullptr;
        ActiveView m_activeView = ActiveView::Placeholder;
        bool m_attachmentsExpanded = false;
        bool m_attachmentsCollapsed = false;
        bool m_translationInProgress = false;
        std::uint64_t m_translationRequestToken = 0;
        std::uint64_t m_snapshotLoadToken = 0;
        bool m_messageTranslated = false;
        QString m_originalPlainText;
        QString m_translationError;
        bool m_autoTranslateAttempted = false;
        bool m_translationWasAutomatic = false;
        bool m_languageDetectionStarted = false;
        bool m_htmlDocumentLoaded = false;
    };

} // namespace javelin::gui::messageview
