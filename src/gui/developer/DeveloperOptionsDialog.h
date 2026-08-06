#pragma once

#include "app/DeveloperDiagnostics.h"
#include "app/DeveloperMaintenance.h"

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
        DeveloperOptionsDialog(javelin::app::DeveloperDiagnosticsPort& diagnostics,
                               javelin::app::DeveloperMaintenancePort& maintenance,
                               QWidget* parent = nullptr);
        ~DeveloperOptionsDialog() override;

      private:
        void refresh();
        void applySnapshot(javelin::app::DeveloperDiagnosticsResult result);
        void updateDetails(const QModelIndex& current);
        void clearSelectedSqlite();
        void clearSelectedBodies();
        void runClear(javelin::app::DeveloperMailboxClearCommand command);
        void applyClearResult(javelin::app::DeveloperMailboxClearResult result);
        [[nodiscard]] const javelin::app::DeveloperMailboxRecord* selectedMailbox() const;
        void setLoading(bool loading);
        void restoreUiState();
        void saveUiState() const;
        void closeEvent(QCloseEvent* event) override;

        javelin::app::DeveloperDiagnosticsPort& m_diagnostics;
        javelin::app::DeveloperMaintenancePort& m_maintenance;
        std::vector<javelin::app::DeveloperMailboxRecord> m_mailboxes;
        QStandardItemModel* m_mailboxModel = nullptr;
        QSortFilterProxyModel* m_filterModel = nullptr;
        QTreeView* m_mailboxView = nullptr;
        QLineEdit* m_filterEdit = nullptr;
        QPlainTextEdit* m_details = nullptr;
        QProgressBar* m_progress = nullptr;
        QLabel* m_status = nullptr;
        QPushButton* m_refreshButton = nullptr;
        QPushButton* m_clearSqliteButton = nullptr;
        QPushButton* m_clearBodiesButton = nullptr;
        QString m_postRefreshStatus;
        bool m_loading = false;
    };

} // namespace javelin::gui::developer
