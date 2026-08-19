#pragma once

#include "app/ComposeApplicationPorts.h"
#include "gui/compose/ComposeRecipientController.h"
#include "jmap/submission/ComposeTypes.h"

#include <QTextCursor>
#include <QWidget>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QAction;
class KActionCollection;
class QComboBox;
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QHBoxLayout;
class QImage;
class QLabel;
class QLineEdit;
class QMenu;
class QScrollArea;
class QTabWidget;
class QToolBar;
class QToolButton;
class QVBoxLayout;

namespace javelin::gui::messageview
{
    class HtmlMessageView;
}
namespace javelin::gui::settings
{
    class GuiSettings;
}

namespace javelin::jmap::cache
{
    class AccountReader;
    class IdentityReader;
} // namespace javelin::jmap::cache

namespace javelin::jmap::contacts
{
    class ContactIdentityLookup;
}

namespace javelin::gui::compose
{

    class AttachmentController;
    class ComposeAutosaveController;
    class ComposeIdentityController;
    class InlineImageController;
    class JavelinComposerEdit;
    class SignatureController;

    class ComposeTabWidget : public QWidget
    {
        Q_OBJECT

      public:
        ComposeTabWidget(javelin::gui::settings::GuiSettings& settings,
                         javelin::app::ComposeCommandPort& composeCommandPort,
                         javelin::jmap::cache::AccountReader& accountReader,
                         javelin::jmap::cache::IdentityReader& identityRepository,
                         javelin::jmap::contacts::ContactIdentityLookup& contactIdentityLookup,
                         javelin::jmap::submission::DraftSnapshot snapshot, QWidget* parent,
                         bool hasUnsavedChanges);
        ~ComposeTabWidget() override;

        [[nodiscard]] QString tabTitle() const;
        [[nodiscard]] QString confirmationDetails() const;
        [[nodiscard]] std::string composeSessionId() const;
        [[nodiscard]] std::optional<std::string> draftEmailId() const;
        [[nodiscard]] bool isEmptyDraft() const;
        [[nodiscard]] bool closeWithoutPrompt() const;
        [[nodiscard]] bool hasUnsavedChanges() const;
        [[nodiscard]] bool operationInFlight() const;
        [[nodiscard]] bool canSend() const;
        [[nodiscard]] bool canScheduleSend() const;
        [[nodiscard]] bool richTextEnabled() const;
        [[nodiscard]] QMenu* signatureMenu() const;

        void saveDraftAndClose();
        void setRichTextEnabled(bool enabled);
        void editCurrentSignature();

      public Q_SLOTS:
        void attachFiles();
        void saveDraft();
        void sendMessage();
        void scheduleMessage();
        void reloadSenderIdentities(const QString& changedAccountId = {});

      protected:
        void dragEnterEvent(QDragEnterEvent* event) override;
        void dragMoveEvent(QDragMoveEvent* event) override;
        void dropEvent(QDropEvent* event) override;

      Q_SIGNALS:
        void titleChanged(const QString& title);
        void accountChanged(const QString& accountId);
        void statusMessageRequested(const QString& message, int timeoutMs = 5000);
        void userInterventionRequired(const QString& message);
        void closeRequested();
        void toolbarStateChanged();
        void manageIdentitiesRequested(QString accountId, QString identityId);

      private:
        enum class DeferredOperation
        {
            None,
            SaveDraft,
            SaveDraftAndClose,
            Send,
            ScheduleSend,
        };

        void setupUi();
        void createToolbarActions();
        void loadIdentities();
        void refreshSignatureMenu();
        void applySnapshotToUi();
        void initializeSignatureTracking();
        void replaceTrackedSignatureForIndex(int index, bool forceInsert = false);
        void removeTrackedSignature();
        void populateAttachments();
        void refreshPreview();
        void syncSnapshotFromUi();
        void switchBodyFormat(bool richText);
        using RecipientType = ComposeRecipientController::RecipientType;
        void setRecipientText(RecipientType type, const QString& text);
        [[nodiscard]] QString recipientText(RecipientType type) const;
        [[nodiscard]] std::vector<javelin::jmap::domain::EmailAddress>
        recipientAddresses(RecipientType type) const;
        void scheduleWorkingCopySave();
        void persistWorkingCopy();
        void setBusy(bool busy);
        void updateEditorModeUi();
        void updateTabTitle();
        void addAttachments();
        void addAttachmentPaths(const QStringList& filePaths);
        void addPastedInlineImage(const QImage& image);
        void insertImage();
        void adoptInsertedComposerImage(int insertionPosition, const QString& sourceFilePath);
        void finishInlineImagePreparation(bool succeeded);
        void removeAttachmentAt(std::size_t index);
        void setAttachmentEmbedded(std::size_t index, bool embedded);
        void insertEmbeddedImage(std::size_t index);
        void removeEmbeddedImageReference(const std::string& contentId);
        void setEditorHtml(const QString& html);
        [[nodiscard]] QString stableEditorHtml();
        void reconcileInlineAttachmentReferences(const QString& html);
        void startSaveDraft(bool closeAfterSave);
        void startSend(std::optional<std::chrono::system_clock::time_point> sendAt = std::nullopt);
        [[nodiscard]] std::optional<std::uint64_t> currentMaxDelayedSendSeconds() const;
        void toggleCode();

        javelin::gui::settings::GuiSettings& m_settings;
        javelin::app::ComposeCommandPort& m_composeCommandPort;
        javelin::jmap::cache::AccountReader& m_accountReader;
        javelin::jmap::contacts::ContactIdentityLookup& m_contactIdentityLookup;
        javelin::jmap::submission::DraftSnapshot m_snapshot;
        std::unique_ptr<ComposeIdentityController> m_identityController;
        bool m_syncingUi = false;
        bool m_operationInFlight = false;
        bool m_closeWithoutPrompt = false;
        bool m_closeAfterSave = false;
        int m_previousIdentityIndex = -1;
        DeferredOperation m_deferredOperation = DeferredOperation::None;
        ComposeAutosaveController* m_autosaveController = nullptr;
        std::unique_ptr<AttachmentController> m_attachmentController;
        std::unique_ptr<InlineImageController> m_inlineImageController;
        QComboBox* m_fromCombo = nullptr;
        QMenu* m_signatureMenu = nullptr;
        QLineEdit* m_subjectEdit = nullptr;
        std::unique_ptr<ComposeRecipientController> m_recipientController;
        std::unique_ptr<SignatureController> m_signatureController;
        QLabel* m_fromLabel = nullptr;
        QLabel* m_subjectLabel = nullptr;
        QToolBar* m_formatToolbar = nullptr;
        KActionCollection* m_actionCollection = nullptr;
        JavelinComposerEdit* m_richTextEdit = nullptr;
        javelin::gui::messageview::HtmlMessageView* m_previewView = nullptr;
        QTabWidget* m_editorTabs = nullptr;
        QScrollArea* m_attachmentScrollArea = nullptr;
        QWidget* m_attachmentStrip = nullptr;
        QHBoxLayout* m_attachmentStripLayout = nullptr;
        QAction* m_codeAction = nullptr;
        QAction* m_insertImageAction = nullptr;
    };

} // namespace javelin::gui::compose
