#pragma once

#include "jmap/sieve/SieveService.h"

#include <QDialog>

#include <string>
#include <vector>

class QLabel;
class QListWidget;
class QPushButton;
class QCheckBox;

namespace KTextEditor
{
    class Document;
}

namespace javelin::app
{
    class MailApplicationService;
}

namespace javelin::gui::sieve
{
    class SieveEditorDialog final : public QDialog
    {
        Q_OBJECT

      public:
        SieveEditorDialog(javelin::app::MailApplicationService& service, std::string ownerAccountId,
                          QWidget* parent = nullptr);

      private:
        void loadScripts();
        void selectScript(int row);
        void newScript();
        void deleteScript();
        void setScriptActive(bool active);
        void removeCurrentScriptFromList();
        void validateScript();
        void saveScript();
        void setBusy(bool busy);
        void showError(const javelin::jmap::OperationError& error);
        void updateActions();

        javelin::app::MailApplicationService& m_service;
        std::string m_ownerAccountId;
        std::vector<javelin::jmap::sieve::SieveScript> m_scripts;
        int m_currentRow = -1;
        bool m_busy = false;
        bool m_loaded = false;
        bool m_dirty = false;
        QListWidget* m_scriptList = nullptr;
        KTextEditor::Document* m_document = nullptr;
        QLabel* m_statusLabel = nullptr;
        QPushButton* m_validateButton = nullptr;
        QPushButton* m_saveButton = nullptr;
        QPushButton* m_newButton = nullptr;
        QPushButton* m_deleteButton = nullptr;
        QCheckBox* m_activeCheckBox = nullptr;
    };
} // namespace javelin::gui::sieve
