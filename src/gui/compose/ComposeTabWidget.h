#pragma once

#include "app/ComposeApplicationPorts.h"
#include "jmap/submission/ComposeTypes.h"

#include <QWidget>

#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

class QAction;
class QComboBox;
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QScrollArea;
class QTabWidget;
class QTextEdit;
class QTimer;
class QToolBar;
class QToolButton;

namespace KTextEditor
{
    class Document;
    class View;
} // namespace KTextEditor

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
    class IdentityReader;
}

namespace javelin::jmap::contacts
{
    class ContactIdentityLookup;
}

namespace javelin::gui::compose
{

    class ComposeTabWidget : public QWidget
    {
        Q_OBJECT

      public:
        ComposeTabWidget(javelin::gui::settings::GuiSettings& settings,
                         javelin::app::ComposeCommandPort& composeCommandPort,
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

        void saveDraftAndClose();

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

      private:
        void setupUi();
        void createToolbarActions();
        void loadIdentities();
        void applySnapshotToUi();
        void populateAttachments();
        void refreshPreview();
        void syncSnapshotFromUi();
        void syncRichTextFromHtmlSource();
        void syncHtmlSourceFromRichText();
        void switchBodyFormat(int index);
        void setOptionalRecipientVisible(QWidget* row, QToolButton* button, bool visible);
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
        void startSaveDraft(bool closeAfterSave);
        void startSend();
        void toggleBold();
        void toggleItalic();
        void toggleUnderline();
        void toggleStrikethrough();
        void toggleCode();
        void insertBulletList();
        void insertNumberedList();
        void alignLeft();
        void alignCenter();
        void alignRight();
        void clearFormatting();
        void insertLink();

        javelin::gui::settings::GuiSettings& m_settings;
        javelin::app::ComposeCommandPort& m_composeCommandPort;
        javelin::jmap::cache::IdentityReader& m_identityRepository;
        javelin::jmap::contacts::ContactIdentityLookup& m_contactIdentityLookup;
        javelin::jmap::submission::DraftSnapshot m_snapshot;
        std::unordered_set<std::string> m_identityLoadsStarted;
        bool m_syncingUi = false;
        bool m_operationInFlight = false;
        bool m_closeWithoutPrompt = false;
        bool m_closeAfterSave = false;
        QTimer* m_autosaveTimer = nullptr;
        QComboBox* m_fromCombo = nullptr;
        QComboBox* m_bodyFormatCombo = nullptr;
        QLineEdit* m_toEdit = nullptr;
        QLineEdit* m_ccEdit = nullptr;
        QLineEdit* m_bccEdit = nullptr;
        QLineEdit* m_subjectEdit = nullptr;
        QWidget* m_ccRow = nullptr;
        QWidget* m_bccRow = nullptr;
        QToolButton* m_ccButton = nullptr;
        QToolButton* m_bccButton = nullptr;
        QToolBar* m_formatToolbar = nullptr;
        QTextEdit* m_richTextEdit = nullptr;
        KTextEditor::Document* m_htmlSourceDocument = nullptr;
        KTextEditor::View* m_htmlSourceView = nullptr;
        KTextEditor::Document* m_plainTextDocument = nullptr;
        KTextEditor::View* m_plainTextView = nullptr;
        javelin::gui::messageview::HtmlMessageView* m_previewView = nullptr;
        QTabWidget* m_editorTabs = nullptr;
        QScrollArea* m_attachmentScrollArea = nullptr;
        QWidget* m_attachmentStrip = nullptr;
        QHBoxLayout* m_attachmentStripLayout = nullptr;
        QAction* m_boldAction = nullptr;
        QAction* m_italicAction = nullptr;
        QAction* m_underlineAction = nullptr;
        QAction* m_strikethroughAction = nullptr;
        QAction* m_codeAction = nullptr;
    };

} // namespace javelin::gui::compose
