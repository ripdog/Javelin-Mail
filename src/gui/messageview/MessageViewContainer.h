#pragma once

#include "gui/messageview/MessageBodyPresenter.h"
#include "gui/translation/LanguageDetection.h"
#include "gui/translation/TranslationTypes.h"
#include "jmap/cache/MessageListReadTypes.h"
#include "jmap/cache/MessageViewReader.h"

#include <QString>
#include <QWidget>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

class QAction;
class QLabel;
class QEvent;
class QGridLayout;
class QLineEdit;
class QMenu;
class QProgressBar;
class QScrollArea;
class QResizeEvent;
class QStackedWidget;
class QTextBrowser;
class QToolButton;
class QVBoxLayout;
class KMessageWidget;
class QWidget;

namespace javelin::gui::translation
{
    class TranslationService;
}
namespace javelin::jmap::contacts
{
    class ContactIdentityLookup;
}
namespace javelin::gui::settings
{
    class GuiSettings;
}

namespace javelin::gui::messageview
{
    class HtmlMessageView;
    class MessageAttachmentPanel;
    class MessageBannerCoordinator;
    class MessageTranslationController;
    class RemoteContentController;

    class MessageViewContainer : public QWidget
    {
        Q_OBJECT

      public:
        explicit MessageViewContainer(
            javelin::gui::settings::GuiSettings& settings,
            javelin::gui::translation::TranslationService& translationService,
            javelin::jmap::contacts::ContactIdentityLookup& contactIdentityLookup,
            QWidget* parent = nullptr);
        ~MessageViewContainer() override;

        void setSelection(javelin::jmap::cache::MessageViewReader& messageViewReader,
                          std::optional<std::string> accountId,
                          std::optional<std::string> mailboxId, std::optional<std::string> emailId,
                          std::optional<std::string> junkMailboxId);
        void setMultipleSelection(std::optional<std::string> accountId,
                                  std::optional<std::string> mailboxId,
                                  std::vector<javelin::jmap::cache::MessageListItem> messages);
        void refresh(javelin::jmap::cache::MessageViewReader& messageViewReader);
        void setErrorState(const QString& errorMessage);
        void focusMessageBody();
        [[nodiscard]] bool readerActionsAvailable() const;
        void showFindBar();
        void findNext();
        void findPrevious();
        void zoomIn();
        void zoomOut();
        void resetZoom();
        void printMessage();
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
        void notJunkRequested(QString accountId, QString mailboxId, QString emailId);
        void readerActionsAvailabilityChanged(bool available);

      private:
        using ActiveView = MessageBodyPresenter::View;

        void setActiveView(ActiveView view);
        void updateAccessibleDocument();
        void startSnapshotLoad(javelin::jmap::cache::MessageViewReader& messageViewReader,
                               bool requestContentIfMissing);
        void updatePresentation(bool reloadBody = true);
        void updateSenderRemoteContentPermit();
        void updateRemoteContentButton();
        void updateJunkBanner();
        void updateUnsubscribeBanner();
        void updateLanguageBanner();
        void startLanguageDetection();
        void translateCurrentMessage();
        void restoreCurrentTranslation();
        void updateTranslateOptionsMenu();
        void setAutoTranslateSender(bool enabled);
        void setAutoTranslateDomain(bool enabled);
        void maybeAutoTranslateCurrentMessage();
        void rebuildMultipleSelectionRows();
        void runFind(bool backwards);
        void clearFindHighlights();
        void updateFindResult(int activeMatch, int matchCount);
        void applyZoom();
        void permitRemoteContentForCurrentSender();
        void permitRemoteContentForCurrentDomain();
        [[nodiscard]] QString currentSenderAddress() const;
        [[nodiscard]] QString currentSenderDomain() const;
        [[nodiscard]] QString contactAwareSenderLabel() const;
        [[nodiscard]] bool messageBannerDismissed(std::string_view bannerId) const;
        void dismissMessageBanner(std::string_view bannerId);
        [[nodiscard]] QString serverDisplayName() const;
        void changeEvent(QEvent* event) override;

        std::optional<std::string> m_accountId;
        std::optional<std::string> m_mailboxId;
        std::optional<std::string> m_emailId;
        std::optional<std::string> m_junkMailboxId;
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
        MessageAttachmentPanel* m_attachmentPanel = nullptr;
        QWidget* m_findBarContainer = nullptr;
        KMessageWidget* m_findBar = nullptr;
        QLineEdit* m_findEdit = nullptr;
        QLabel* m_findResultLabel = nullptr;
        QToolButton* m_findPreviousButton = nullptr;
        QToolButton* m_findNextButton = nullptr;
        QAction* m_permitSenderRemoteContentAction = nullptr;
        QAction* m_permitDomainRemoteContentAction = nullptr;
        QAction* m_remoteContentAction = nullptr;
        KMessageWidget* m_remoteContentBanner = nullptr;
        KMessageWidget* m_junkBanner = nullptr;
        KMessageWidget* m_unsubscribeBanner = nullptr;
        KMessageWidget* m_translationBanner = nullptr;
        QAction* m_notJunkAction = nullptr;
        QAction* m_translateAction = nullptr;
        QAction* m_translateOptionsAction = nullptr;
        QMenu* m_translateOptionsMenu = nullptr;
        javelin::gui::settings::GuiSettings& m_settings;
        javelin::gui::translation::TranslationService& m_translationService;
        javelin::jmap::contacts::ContactIdentityLookup& m_contactIdentityLookup;
        QStackedWidget* m_bodyStack = nullptr;
        QProgressBar* m_loadingIndicator = nullptr;
        QScrollArea* m_multipleSelectionScrollArea = nullptr;
        QWidget* m_multipleSelectionWidget = nullptr;
        QVBoxLayout* m_multipleSelectionLayout = nullptr;
        QTextBrowser* m_plainTextView = nullptr;
        HtmlMessageView* m_htmlView = nullptr;
        std::unique_ptr<MessageBodyPresenter> m_bodyPresenter;
        std::unique_ptr<MessageTranslationController> m_translationController;
        std::uint64_t m_snapshotLoadToken = 0;
        QString m_plainTextFindQuery;
        int m_plainTextFindIndex = -1;
        int m_zoomSteps = 0;
        bool m_readerActionsAvailable = false;
        std::unique_ptr<MessageBannerCoordinator> m_bannerCoordinator;
        std::unique_ptr<RemoteContentController> m_remoteContentController;
    };

} // namespace javelin::gui::messageview
