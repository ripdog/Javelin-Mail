#pragma once

#include "app/ComposeApplicationPorts.h"
#include "jmap/submission/ComposeTypes.h"

#include <QTextCursor>
#include <QWidget>

#include <chrono>
#include <optional>
#include <string>
#include <unordered_set>
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
class QTimer;
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

    class JavelinComposerEdit;

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
        ~ComposeTabWidget() override = default;

        [[nodiscard]] QString tabTitle() const;
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
        enum class RecipientType
        {
            To,
            Cc,
            Bcc,
        };

        struct RecipientRow
        {
            QWidget* widget = nullptr;
            QComboBox* typeCombo = nullptr;
            QLineEdit* edit = nullptr;
        };

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
        [[nodiscard]] QString signaturePlainTextForIndex(int index) const;
        [[nodiscard]] QString signatureHtmlForIndex(int index) const;
        [[nodiscard]] int defaultSignatureInsertionPosition() const;
        void populateAttachments();
        void refreshPreview();
        void syncSnapshotFromUi();
        void switchBodyFormat(bool richText);
        void addRecipientRow(RecipientType type, const QString& text = {});
        void resetRecipientRows();
        void ensureTrailingRecipientRow();
        void setRecipientText(RecipientType type, const QString& text);
        [[nodiscard]] QString recipientText(RecipientType type) const;
        [[nodiscard]] std::vector<javelin::jmap::domain::EmailAddress>
        recipientAddresses(RecipientType type) const;
        void updateRecipientRowWidths();
        void scheduleWorkingCopySave();
        void persistWorkingCopy();
        void setBusy(bool busy);
        void updateEditorModeUi();
        void updateTabTitle();
        void addAttachments();
        void addAttachmentPaths(const QStringList& filePaths);
        void addInlineImagePath(const QString& filePath);
        void addPastedInlineImage(const QImage& image);
        void insertImage();
        void adoptInsertedComposerImage(int insertionPosition, const QString& sourceFilePath);
        void finishInlineImagePreparation();
        void removeAttachmentAt(std::size_t index);
        void setAttachmentEmbedded(std::size_t index, bool embedded);
        void insertEmbeddedImage(std::size_t index);
        void removeEmbeddedImageReference(const std::string& contentId);
        void setEditorHtml(const QString& html);
        void loadInlineImageResources();
        [[nodiscard]] QString stableEditorHtml();
        void reconcileInlineAttachmentReferences(const QString& html);
        void startSaveDraft(bool closeAfterSave);
        void startSend(std::optional<std::chrono::system_clock::time_point> sendAt = std::nullopt);
        [[nodiscard]] std::optional<std::uint64_t> currentMaxDelayedSendSeconds() const;
        void toggleCode();

        javelin::gui::settings::GuiSettings& m_settings;
        javelin::app::ComposeCommandPort& m_composeCommandPort;
        javelin::jmap::cache::AccountReader& m_accountReader;
        javelin::jmap::cache::IdentityReader& m_identityRepository;
        javelin::jmap::contacts::ContactIdentityLookup& m_contactIdentityLookup;
        javelin::jmap::submission::DraftSnapshot m_snapshot;
        std::unordered_set<std::string> m_identityLoadsStarted;
        bool m_syncingUi = false;
        bool m_operationInFlight = false;
        bool m_closeWithoutPrompt = false;
        bool m_hasUnsavedChanges = false;
        bool m_closeAfterSave = false;
        bool m_signatureProgrammaticEdit = false;
        bool m_signatureTracked = false;
        bool m_signatureCustom = false;
        bool m_signatureExplicitlyRemoved = false;
        int m_signatureInsertionPosition = 0;
        int m_previousIdentityIndex = -1;
        QTextCursor m_signatureCursor;
        std::size_t m_pendingInlineImageJobs = 0;
        DeferredOperation m_deferredOperation = DeferredOperation::None;
        QTimer* m_autosaveTimer = nullptr;
        QComboBox* m_fromCombo = nullptr;
        QMenu* m_signatureMenu = nullptr;
        QLineEdit* m_subjectEdit = nullptr;
        QVBoxLayout* m_recipientRowsLayout = nullptr;
        std::vector<RecipientRow> m_recipientRows;
        QLabel* m_fromLabel = nullptr;
        QLabel* m_subjectLabel = nullptr;
        int m_headerLabelWidth = 0;
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
