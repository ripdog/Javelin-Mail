#pragma once

#include "jmap/domain/MailEntities.h"

#include <QDialog>

#include <optional>
#include <string>
#include <vector>

class KActionCollection;
class QAction;
class QCheckBox;
class QLineEdit;
class QPushButton;
class QToolBar;
class QTreeWidget;
class QTreeWidgetItem;

namespace javelin::app
{
    class IdentityCommandPort;
    class MailApplicationEventsPort;
} // namespace javelin::app
namespace javelin::jmap::cache
{
    class AccountReader;
    class IdentityReader;
} // namespace javelin::jmap::cache
namespace javelin::gui::settings
{
    class GuiSettings;
}

namespace javelin::gui::compose
{
    class JavelinComposerEdit;

    class SendingIdentitiesDialog final : public QDialog
    {
        Q_OBJECT

      public:
        SendingIdentitiesDialog(javelin::gui::settings::GuiSettings& settings,
                                javelin::jmap::cache::AccountReader& accountReader,
                                javelin::jmap::cache::IdentityReader& identityReader,
                                javelin::app::IdentityCommandPort& commandPort,
                                javelin::app::MailApplicationEventsPort& mailEvents,
                                QWidget* parent = nullptr);

        void selectIdentity(std::string accountId, std::string identityId);

      private:
        struct AccountEntry
        {
            std::string accountId;
            QString displayName;
        };

        void setupUi();
        void createFormattingActions();
        void reloadTree(std::optional<std::string> selectedAccountId = std::nullopt,
                        std::optional<std::string> selectedIdentityId = std::nullopt);
        void loadCurrentSelection();
        void presentIdentity(std::string accountId, javelin::jmap::domain::Identity identity,
                             bool pending,
                             std::optional<javelin::jmap::domain::Identity> revertIdentity);
        void clearEditor();
        void beginNewIdentity();
        void duplicateCurrentIdentity();
        void saveCurrentIdentity();
        void revertCurrentIdentity();
        void deleteCurrentIdentity();
        void refreshCurrentAccount();
        void switchEditorMode(bool richText);
        void setBusy(bool busy);
        void updateActions();
        [[nodiscard]] std::optional<AccountEntry> accountEntry(std::string_view accountId) const;
        [[nodiscard]] std::optional<std::string> selectedAccountId() const;
        [[nodiscard]] std::optional<std::string> selectedIdentityId() const;

        javelin::gui::settings::GuiSettings& m_settings;
        javelin::jmap::cache::AccountReader& m_accountReader;
        javelin::jmap::cache::IdentityReader& m_identityReader;
        javelin::app::IdentityCommandPort& m_commandPort;
        javelin::app::MailApplicationEventsPort& m_mailEvents;
        std::vector<AccountEntry> m_accounts;
        std::string m_editorAccountId;
        std::optional<javelin::jmap::domain::Identity> m_editorIdentity;
        std::optional<javelin::jmap::domain::Identity> m_revertIdentity;
        bool m_editorPending = false;
        bool m_editorDirty = false;
        bool m_busy = false;
        bool m_switchingMode = false;
        QTreeWidget* m_tree = nullptr;
        QLineEdit* m_nameEdit = nullptr;
        QLineEdit* m_emailEdit = nullptr;
        QLineEdit* m_replyToEdit = nullptr;
        QLineEdit* m_bccEdit = nullptr;
        QCheckBox* m_richTextCheck = nullptr;
        QToolBar* m_formatToolbar = nullptr;
        KActionCollection* m_actionCollection = nullptr;
        JavelinComposerEdit* m_signatureEdit = nullptr;
        QPushButton* m_newButton = nullptr;
        QPushButton* m_duplicateButton = nullptr;
        QPushButton* m_deleteButton = nullptr;
        QPushButton* m_refreshButton = nullptr;
        QPushButton* m_saveButton = nullptr;
        QPushButton* m_revertButton = nullptr;
    };
} // namespace javelin::gui::compose
