#include "gui/tasks/TaskCenterDialog.h"

#include "app/WorkTaskModel.h"
#include "app/WorkTaskPort.h"

#include <QApplication>
#include <QClipboard>
#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QStyleOptionButton>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QVBoxLayout>

namespace javelin::gui::tasks
{
    namespace
    {
        enum class RowAction
        {
            None,
            Pause,
            Resume,
            Retry,
        };

        [[nodiscard]] RowAction actionForStatus(const javelin::app::WorkStatus status)
        {
            using javelin::app::WorkStatus;
            switch (status)
            {
            case WorkStatus::Queued:
            case WorkStatus::Running:
            case WorkStatus::WaitingForSpace:
            case WorkStatus::WaitingForNetwork:
            case WorkStatus::WaitingForAuth:
                return RowAction::Pause;
            case WorkStatus::Paused:
                return RowAction::Resume;
            case WorkStatus::Failed:
                return RowAction::Retry;
            case WorkStatus::Complete:
                return RowAction::None;
            }
            return RowAction::None;
        }

        [[nodiscard]] QString actionLabel(const RowAction action)
        {
            switch (action)
            {
            case RowAction::Pause:
                return QStringLiteral("Pause");
            case RowAction::Resume:
                return QStringLiteral("Resume");
            case RowAction::Retry:
                return QStringLiteral("Retry");
            case RowAction::None:
                return {};
            }
            return {};
        }

        [[nodiscard]] QRect buttonRect(const QStyleOptionViewItem& option)
        {
            constexpr int buttonWidth = 76;
            constexpr int buttonHeight = 26;
            return QRect{option.rect.center().x() - buttonWidth / 2,
                         option.rect.center().y() - buttonHeight / 2, buttonWidth, buttonHeight};
        }

        class TaskActionDelegate final : public QStyledItemDelegate
        {
          public:
            TaskActionDelegate(javelin::app::WorkTaskPort& taskPort,
                               javelin::app::WorkTaskModel& model, QObject* parent)
                : QStyledItemDelegate(parent), m_taskPort(taskPort), m_model(model)
            {
            }

            void paint(QPainter* painter, const QStyleOptionViewItem& option,
                       const QModelIndex& index) const override
            {
                QStyledItemDelegate::paint(painter, option, index);
                const auto* record = m_model.recordAt(index.row());
                if (record == nullptr)
                    return;
                const auto action = actionForStatus(record->status);
                if (action == RowAction::None)
                    return;
                QStyleOptionButton button;
                button.rect = buttonRect(option);
                button.palette = option.palette;
                button.fontMetrics = option.fontMetrics;
                button.state = QStyle::State_Enabled;
                button.text = actionLabel(action);
                QApplication::style()->drawControl(QStyle::CE_PushButton, &button, painter);
            }

            bool editorEvent(QEvent* event, QAbstractItemModel*, const QStyleOptionViewItem& option,
                             const QModelIndex& index) override
            {
                if (event->type() != QEvent::MouseButtonRelease)
                    return false;
                const auto* mouseEvent = static_cast<QMouseEvent*>(event);
                if (mouseEvent->button() != Qt::LeftButton ||
                    !buttonRect(option).contains(mouseEvent->position().toPoint()))
                    return false;
                const auto* record = m_model.recordAt(index.row());
                if (record == nullptr)
                    return false;
                const std::string jobId = record->jobId;
                switch (actionForStatus(record->status))
                {
                case RowAction::Pause:
                    static_cast<void>(m_taskPort.pause(jobId));
                    return true;
                case RowAction::Resume:
                    static_cast<void>(m_taskPort.resume(jobId));
                    return true;
                case RowAction::Retry:
                    static_cast<void>(m_taskPort.retry(jobId));
                    return true;
                case RowAction::None:
                    return false;
                }
                return false;
            }

          private:
            javelin::app::WorkTaskPort& m_taskPort;
            javelin::app::WorkTaskModel& m_model;
        };
    } // namespace

    TaskCenterDialog::TaskCenterDialog(javelin::app::WorkTaskPort& taskPort, QWidget* parent)
        : QDialog(parent)
    {
        setWindowTitle(QStringLiteral("Task Center"));
        setAttribute(Qt::WA_DeleteOnClose);
        resize(760, 360);

        auto* layout = new QVBoxLayout(this);
        m_model = new javelin::app::WorkTaskModel(taskPort, this);
        m_table = new QTableView(this);
        m_table->setModel(m_model);
        m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_table->setSelectionMode(QAbstractItemView::SingleSelection);
        m_table->setAlternatingRowColors(true);
        m_table->setContextMenuPolicy(Qt::CustomContextMenu);
        m_table->setItemDelegateForColumn(4, new TaskActionDelegate(taskPort, *m_model, m_table));
        m_table->horizontalHeader()->setStretchLastSection(false);
        m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
        m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Fixed);
        m_table->setColumnWidth(4, 96);
        m_table->verticalHeader()->setDefaultSectionSize(34);
        layout->addWidget(m_table, 1);

        auto* actionLayout = new QHBoxLayout;
        actionLayout->addStretch(1);
        auto* closeButton = new QPushButton(QStringLiteral("Close"), this);
        actionLayout->addWidget(closeButton);
        layout->addLayout(actionLayout);

        connect(m_table, &QTableView::customContextMenuRequested, this,
                [this](const QPoint& position)
                {
                    const QModelIndex index = m_table->indexAt(position);
                    if (!index.isValid())
                        return;
                    const QString details = m_model->index(index.row(), 3).data().toString();
                    QMenu menu{m_table};
                    auto* copy = menu.addAction(QStringLiteral("Copy"));
                    copy->setEnabled(!details.isEmpty());
                    if (menu.exec(m_table->viewport()->mapToGlobal(position)) == copy)
                        QApplication::clipboard()->setText(details);
                });
        connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
    }
} // namespace javelin::gui::tasks
