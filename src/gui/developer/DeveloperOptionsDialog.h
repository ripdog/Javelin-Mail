#pragma once

#include "app/DeveloperDiagnostics.h"

#include <KPageDialog>

#include <memory>
#include <vector>

class QCloseEvent;
class QLabel;
class QLineEdit;
class QModelIndex;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSortFilterProxyModel;
class QStandardItemModel;
class QTreeView;

namespace javelin::gui::developer
{

    class DeveloperOptionsDialog final : public KPageDialog
    {
        Q_OBJECT

      public:
        explicit DeveloperOptionsDialog(javelin::app::DeveloperDiagnosticsPort& diagnostics,
                                        QWidget* parent = nullptr);
        ~DeveloperOptionsDialog() override;

      private:
        void refresh();
        void applySnapshot(javelin::app::DeveloperDiagnosticsResult result);
        void updateDetails(const QModelIndex& current);
        void setLoading(bool loading);
        void restoreUiState();
        void saveUiState() const;
        void closeEvent(QCloseEvent* event) override;

        javelin::app::DeveloperDiagnosticsPort& m_diagnostics;
        std::vector<javelin::app::DeveloperMailboxRecord> m_mailboxes;
        QStandardItemModel* m_mailboxModel = nullptr;
        QSortFilterProxyModel* m_filterModel = nullptr;
        QTreeView* m_mailboxView = nullptr;
        QLineEdit* m_filterEdit = nullptr;
        QPlainTextEdit* m_details = nullptr;
        QProgressBar* m_progress = nullptr;
        QLabel* m_status = nullptr;
        QPushButton* m_refreshButton = nullptr;
        bool m_loading = false;
    };

} // namespace javelin::gui::developer
