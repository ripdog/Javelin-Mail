#pragma once

#include "app/ComposeApplicationPorts.h"
#include "jmap/submission/ComposeTypes.h"

#include <QWidget>

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
class QScrollArea;
class QTabWidget;
class QTimer;
class QToolBar;
class QToolButton;

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
                         javelin::jmap::submission::DraftSnapshot snapshot,
                         QWidget* parent = nullptr);
        ~ComposeTabWidget() override = default;

        [[nodiscard]] QString tabTitle() const;
        [[nodiscard]] std::string composeSessionId() const;
        [[nodiscard]] std::optional<std::string> draftEmailId() const;
        [[nodiscard]] bool isEmptyDraft() const;
        [[nodiscard]] bool closeWithoutPrompt() const;
        [[nodiscard]] bool operationInFlight() const;
        [[nodiscard]] bool richTextEnabled() const;

        void saveDraftAndClose();
        void setRichTextEnabled(bool enabled);

      public Q_SLOTS:
        void attachFiles();
        void saveDraft();
        void sendMessage();

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

      private:
        enum class DeferredOperation
        {
            None,
            SaveDraft,
            SaveDraftAndClose,
            Send,
        };

        void setupUi();
        void createToolbarActions();
        void loadIdentities();
        void applySnapshotToUi();
        void populateAttachments();
        void refreshPreview();
        void syncSnapshotFromUi();
        void switchBodyFormat(bool richText);
        void setOptionalRecipientVisible(QWidget* row, QToolButton* button, bool visible);
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
        void startSend();
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
        bool m_closeAfterSave = false;
        std::size_t m_pendingInlineImageJobs = 0;
        DeferredOperation m_deferredOperation = DeferredOperation::None;
        QTimer* m_autosaveTimer = nullptr;
        QComboBox* m_fromCombo = nullptr;
        QLineEdit* m_toEdit = nullptr;
        QLineEdit* m_ccEdit = nullptr;
        QLineEdit* m_bccEdit = nullptr;
        QLineEdit* m_subjectEdit = nullptr;
        QWidget* m_ccRow = nullptr;
        QWidget* m_bccRow = nullptr;
        QToolButton* m_ccButton = nullptr;
        QToolButton* m_bccButton = nullptr;
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
