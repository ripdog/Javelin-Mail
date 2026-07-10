#pragma once

#include "jmap/submission/ComposeTypes.h"

#include <QWidget>

#include <optional>
#include <string>
#include <vector>

class QAction;
class QComboBox;
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QTabWidget;
class QTextEdit;
class QTimer;
class QToolBar;

namespace javelin::gui::messageview
{
    class HtmlMessageView;
}

namespace javelin::jmap::cache
{
    class IdentityRepository;
}

namespace javelin::jmap::contacts
{
    class ContactIdentityLookup;
}

namespace javelin::jmap::submission
{
    class ComposeService;
}

namespace javelin::gui::compose
{

    class ComposeTabWidget : public QWidget
    {
        Q_OBJECT

      public:
        ComposeTabWidget(javelin::jmap::submission::ComposeService& composeService,
                         javelin::jmap::cache::IdentityRepository& identityRepository,
                         javelin::jmap::contacts::ContactIdentityLookup& contactIdentityLookup,
                         javelin::jmap::submission::DraftSnapshot snapshot,
                         QWidget* parent = nullptr);
        ~ComposeTabWidget() override = default;

        [[nodiscard]] QString tabTitle() const;
        [[nodiscard]] std::string accountId() const;
        [[nodiscard]] std::string composeSessionId() const;
        [[nodiscard]] std::optional<std::string> draftEmailId() const;
        [[nodiscard]] javelin::jmap::submission::DraftSnapshot snapshot() const;
        [[nodiscard]] bool isEmptyDraft() const;
        [[nodiscard]] bool closeWithoutPrompt() const;
        [[nodiscard]] bool operationInFlight() const;

        void saveDraftAndClose();

      protected:
        void dragEnterEvent(QDragEnterEvent* event) override;
        void dragMoveEvent(QDragMoveEvent* event) override;
        void dropEvent(QDropEvent* event) override;

      Q_SIGNALS:
        void titleChanged(const QString& title);
        void statusMessageRequested(const QString& message, int timeoutMs = 5000);
        void userInterventionRequired(const QString& message);
        void closeRequested();

      private:
        void setupUi();
        void createToolbarActions();
        void loadIdentities();
        void setupContactCompletion();
        void applySnapshotToUi();
        void populateAttachments();
        void refreshPreview();
        void syncSnapshotFromUi();
        void syncRichTextFromHtmlSource();
        void syncHtmlSourceFromRichText();
        void scheduleWorkingCopySave();
        void persistWorkingCopy();
        void setBusy(bool busy);
        void updateEditorModeUi();
        void updateTabTitle();
        void addAttachments();
        void addAttachmentPaths(const QStringList& filePaths);
        void removeAttachmentAt(std::size_t index);
        void setAttachmentEmbedded(std::size_t index, bool embedded);
        void insertEmbeddedImage(std::size_t index);
        void removeEmbeddedImageReference(const std::string& contentId);
        void requestClose();
        void startSaveDraft(bool closeAfterSave);
        void startSend();
        void toggleBold();
        void toggleItalic();
        void toggleUnderline();
        void toggleStrikethrough();
        void insertBulletList();
        void insertNumberedList();
        void alignLeft();
        void alignCenter();
        void alignRight();
        void clearFormatting();
        void insertLink();

        javelin::jmap::submission::ComposeService& m_composeService;
        javelin::jmap::cache::IdentityRepository& m_identityRepository;
        javelin::jmap::contacts::ContactIdentityLookup& m_contactIdentityLookup;
        javelin::jmap::submission::DraftSnapshot m_snapshot;
        std::vector<javelin::jmap::domain::Identity> m_identities;
        bool m_syncingUi = false;
        bool m_operationInFlight = false;
        bool m_closeWithoutPrompt = false;
        bool m_closeAfterSave = false;
        QTimer* m_autosaveTimer = nullptr;
        QComboBox* m_fromCombo = nullptr;
        QLineEdit* m_toEdit = nullptr;
        QLineEdit* m_ccEdit = nullptr;
        QLineEdit* m_bccEdit = nullptr;
        QLineEdit* m_subjectEdit = nullptr;
        QToolBar* m_formatToolbar = nullptr;
        QTextEdit* m_richTextEdit = nullptr;
        QPlainTextEdit* m_htmlSourceEdit = nullptr;
        javelin::gui::messageview::HtmlMessageView* m_previewView = nullptr;
        QTabWidget* m_editorTabs = nullptr;
        QLabel* m_modeHintLabel = nullptr;
        QPushButton* m_addAttachmentButton = nullptr;
        QScrollArea* m_attachmentScrollArea = nullptr;
        QWidget* m_attachmentStrip = nullptr;
        QHBoxLayout* m_attachmentStripLayout = nullptr;
        QPushButton* m_saveDraftButton = nullptr;
        QPushButton* m_sendButton = nullptr;
        QPushButton* m_closeButton = nullptr;
        QAction* m_boldAction = nullptr;
        QAction* m_italicAction = nullptr;
        QAction* m_underlineAction = nullptr;
        QAction* m_strikethroughAction = nullptr;
    };

} // namespace javelin::gui::compose
