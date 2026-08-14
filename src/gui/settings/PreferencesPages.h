#pragma once

#include "gui/messageview/MessageAppearance.h"
#include "gui/settings/GuiSettings.h"
#include "gui/translation/TranslationTypes.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QRadioButton;
class QSpinBox;

namespace javelin::gui::mailboxes
{
    class MailboxTreeModel;
    class MailboxTreeView;
} // namespace javelin::gui::mailboxes

namespace javelin::jmap::cache
{
    class AccountReader;
    class MailboxReader;
} // namespace javelin::jmap::cache

namespace javelin::gui::translation
{
    class TranslationService;
}

namespace javelin::gui::settings
{
    class AccountsPage final : public QWidget
    {
      public:
        explicit AccountsPage(QWidget* parent = nullptr);

        [[nodiscard]] QListWidget* accountList() const;
        [[nodiscard]] QPushButton* addButton() const;
        [[nodiscard]] QPushButton* removeButton() const;
        [[nodiscard]] QPushButton* reauthenticateButton() const;
        [[nodiscard]] QLineEdit* displayNameEdit() const;
        [[nodiscard]] QLabel* loginEmailLabel() const;
        [[nodiscard]] QLabel* sessionUrlLabel() const;

      private:
        QListWidget* m_accountList = nullptr;
        QPushButton* m_addButton = nullptr;
        QPushButton* m_removeButton = nullptr;
        QPushButton* m_reauthenticateButton = nullptr;
        QLineEdit* m_displayNameEdit = nullptr;
        QLabel* m_loginEmailLabel = nullptr;
        QLabel* m_sessionUrlLabel = nullptr;
    };

    class MailboxSyncPage final : public QWidget
    {
      public:
        MailboxSyncPage(GuiSettings& settings, javelin::jmap::cache::AccountReader& accountReader,
                        javelin::jmap::cache::MailboxReader& mailboxReader,
                        QWidget* parent = nullptr);

        [[nodiscard]] QComboBox* accountCombo() const;
        [[nodiscard]] javelin::gui::mailboxes::MailboxTreeView* treeView() const;
        [[nodiscard]] javelin::gui::mailboxes::MailboxTreeModel* model() const;

      private:
        QComboBox* m_accountCombo = nullptr;
        javelin::gui::mailboxes::MailboxTreeView* m_treeView = nullptr;
        javelin::gui::mailboxes::MailboxTreeModel* m_model = nullptr;
    };

    class RemoteContentPage final : public QWidget
    {
      public:
        explicit RemoteContentPage(QWidget* parent = nullptr);

        [[nodiscard]] QListWidget* permitList() const;
        [[nodiscard]] QPushButton* removeButton() const;

      private:
        QListWidget* m_permitList = nullptr;
        QPushButton* m_removeButton = nullptr;
    };

    class AppearancePage final : public QWidget
    {
      public:
        explicit AppearancePage(
            const javelin::gui::messageview::MessageAppearanceSettings& settings,
            QWidget* parent = nullptr);

        [[nodiscard]] QComboBox* messageColorMode() const;

      private:
        QComboBox* m_messageColorMode = nullptr;
    };

    class TranslationPage final : public QWidget
    {
      public:
        TranslationPage(javelin::gui::translation::TranslationService& service,
                        const javelin::gui::translation::TranslationSettings& settings,
                        QWidget* parent = nullptr);

        [[nodiscard]] QComboBox* provider() const;
        [[nodiscard]] QWidget* translationControls() const;
        [[nodiscard]] QComboBox* targetLanguage() const;
        [[nodiscard]] QWidget* googleControls() const;
        [[nodiscard]] QLineEdit* apiKeyEdit() const;
        [[nodiscard]] QWidget* localControls() const;
        [[nodiscard]] QComboBox* localSource() const;
        [[nodiscard]] QComboBox* localTarget() const;
        [[nodiscard]] QPushButton* downloadLocalModelsButton() const;
        [[nodiscard]] QListWidget* installedLocalModels() const;
        [[nodiscard]] QPushButton* removeLocalModelsButton() const;
        [[nodiscard]] QLabel* localModelStatus() const;
        [[nodiscard]] QListWidget* autoTranslateList() const;
        [[nodiscard]] QPushButton* removeAutoTranslateButton() const;

      private:
        QComboBox* m_provider = nullptr;
        QWidget* m_translationControls = nullptr;
        QComboBox* m_targetLanguage = nullptr;
        QWidget* m_googleControls = nullptr;
        QLineEdit* m_apiKeyEdit = nullptr;
        QWidget* m_localControls = nullptr;
        QComboBox* m_localSource = nullptr;
        QComboBox* m_localTarget = nullptr;
        QPushButton* m_downloadLocalModelsButton = nullptr;
        QListWidget* m_installedLocalModels = nullptr;
        QPushButton* m_removeLocalModelsButton = nullptr;
        QLabel* m_localModelStatus = nullptr;
        QListWidget* m_autoTranslateList = nullptr;
        QPushButton* m_removeAutoTranslateButton = nullptr;
    };

    class AttachmentsPage final : public QWidget
    {
      public:
        explicit AttachmentsPage(const AttachmentSaveSettings& settings, QWidget* parent = nullptr);

        [[nodiscard]] QRadioButton* askDirectoryRadio() const;
        [[nodiscard]] QRadioButton* saveDirectoryRadio() const;
        [[nodiscard]] QLineEdit* directoryEdit() const;
        [[nodiscard]] QPushButton* directoryButton() const;

      private:
        QRadioButton* m_askDirectoryRadio = nullptr;
        QRadioButton* m_saveDirectoryRadio = nullptr;
        QLineEdit* m_directoryEdit = nullptr;
        QPushButton* m_directoryButton = nullptr;
    };

    class ComposingPage final : public QWidget
    {
      public:
        explicit ComposingPage(int undoSendDelaySeconds, bool undoSendUsesDialog,
                               QWidget* parent = nullptr);

        [[nodiscard]] QSpinBox* undoSendDelaySpinBox() const;
        [[nodiscard]] QComboBox* undoSendPresentationCombo() const;

      private:
        QSpinBox* m_undoSendDelaySpinBox = nullptr;
        QComboBox* m_undoSendPresentationCombo = nullptr;
    };
} // namespace javelin::gui::settings
