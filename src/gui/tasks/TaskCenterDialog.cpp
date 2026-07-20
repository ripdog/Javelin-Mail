#include "gui/tasks/TaskCenterDialog.h"

#include "app/WorkScheduler.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>

namespace javelin::gui::tasks
{
    TaskCenterDialog::TaskCenterDialog(javelin::app::WorkScheduler& scheduler, QWidget* parent)
        : QDialog(parent), m_scheduler(scheduler)
    {
        setWindowTitle(QStringLiteral("Task Center"));
        setAttribute(Qt::WA_DeleteOnClose);
        resize(760, 360);

        auto* layout = new QVBoxLayout(this);
        m_model = new javelin::app::WorkTaskModel(scheduler, this);
        m_table = new QTableView(this);
        m_table->setModel(m_model);
        m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_table->setSelectionMode(QAbstractItemView::SingleSelection);
        m_table->setAlternatingRowColors(true);
        m_table->horizontalHeader()->setStretchLastSection(true);
        m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        layout->addWidget(m_table, 1);

        auto* actionLayout = new QHBoxLayout;
        m_pauseButton = new QPushButton(QStringLiteral("Pause"), this);
        m_resumeButton = new QPushButton(QStringLiteral("Resume"), this);
        m_retryButton = new QPushButton(QStringLiteral("Retry"), this);
        actionLayout->addWidget(m_pauseButton);
        actionLayout->addWidget(m_resumeButton);
        actionLayout->addWidget(m_retryButton);
        actionLayout->addStretch(1);
        auto* closeButton = new QPushButton(QStringLiteral("Close"), this);
        actionLayout->addWidget(closeButton);
        layout->addLayout(actionLayout);

        connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
        connect(m_pauseButton, &QPushButton::clicked, this, &TaskCenterDialog::pauseSelected);
        connect(m_resumeButton, &QPushButton::clicked, this, &TaskCenterDialog::resumeSelected);
        connect(m_retryButton, &QPushButton::clicked, this, &TaskCenterDialog::retrySelected);
        connect(m_table->selectionModel(), &QItemSelectionModel::selectionChanged, this,
                [this]() { updateActions(); });
        updateActions();
    }

    void TaskCenterDialog::updateActions()
    {
        const auto* item = m_model->recordAt(m_table->currentIndex().row());
        m_pauseButton->setEnabled(item != nullptr &&
                                  (item->status == javelin::app::WorkStatus::Queued ||
                                   item->status == javelin::app::WorkStatus::Running ||
                                   item->status == javelin::app::WorkStatus::WaitingForNetwork ||
                                   item->status == javelin::app::WorkStatus::WaitingForSpace));
        m_resumeButton->setEnabled(item != nullptr &&
                                   item->status == javelin::app::WorkStatus::Paused);
        m_retryButton->setEnabled(item != nullptr &&
                                  item->status == javelin::app::WorkStatus::Failed);
    }

    void TaskCenterDialog::pauseSelected()
    {
        if (const auto* item = m_model->recordAt(m_table->currentIndex().row()))
            static_cast<void>(m_scheduler.pause(item->jobId));
        updateActions();
    }

    void TaskCenterDialog::resumeSelected()
    {
        if (const auto* item = m_model->recordAt(m_table->currentIndex().row()))
            static_cast<void>(m_scheduler.resume(item->jobId));
        updateActions();
    }

    void TaskCenterDialog::retrySelected()
    {
        if (const auto* item = m_model->recordAt(m_table->currentIndex().row()))
            static_cast<void>(m_scheduler.retry(item->jobId));
        updateActions();
    }
} // namespace javelin::gui::tasks
