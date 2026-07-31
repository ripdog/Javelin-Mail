#include "app/WorkTaskModel.h"

#include <algorithm>
#include <chrono>

namespace javelin::app
{
    WorkTaskModel::WorkTaskModel(WorkTaskPort& taskPort, QObject* parent)
        : QAbstractTableModel(parent), m_taskPort(taskPort)
    {
        m_reloadTimer.setSingleShot(true);
        m_reloadTimer.setInterval(std::chrono::milliseconds{100});
        connect(&m_reloadTimer, &QTimer::timeout, this, &WorkTaskModel::reload);
        static_cast<void>(m_taskPort.connectChanged(this,
                                                    [this]
                                                    {
                                                        if (!m_reloadTimer.isActive())
                                                            m_reloadTimer.start();
                                                    }));
        reload();
    }

    int WorkTaskModel::rowCount(const QModelIndex& parent) const
    {
        return parent.isValid() ? 0 : static_cast<int>(m_records.size());
    }

    int WorkTaskModel::columnCount(const QModelIndex& parent) const
    {
        return parent.isValid() ? 0 : 5;
    }

    QVariant WorkTaskModel::data(const QModelIndex& index, const int role) const
    {
        if (!index.isValid() || index.row() < 0 ||
            static_cast<std::size_t>(index.row()) >= m_records.size())
            return {};
        const auto& item = m_records.at(static_cast<std::size_t>(index.row()));
        if (role != Qt::DisplayRole)
            return {};
        switch (index.column())
        {
        case 0:
            return item.title;
        case 1:
            return QString::fromStdString(std::string{toString(item.status)});
        case 2:
            if (item.progress.totalUnits && *item.progress.totalUnits > 0)
                return QStringLiteral("%1 / %2")
                    .arg(item.progress.completedUnits)
                    .arg(*item.progress.totalUnits);
            return item.progress.completedUnits > 0 ? QString::number(item.progress.completedUnits)
                                                    : QStringLiteral("—");
        case 3:
            return item.errorText.value_or(item.progress.detail);
        case 4:
            return {};
        default:
            return {};
        }
    }

    QVariant WorkTaskModel::headerData(const int section, const Qt::Orientation orientation,
                                       const int role) const
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
            return {};
        switch (section)
        {
        case 0:
            return QStringLiteral("Task");
        case 1:
            return QStringLiteral("State");
        case 2:
            return QStringLiteral("Progress");
        case 3:
            return QStringLiteral("Details");
        case 4:
            return QStringLiteral("Actions");
        default:
            return {};
        }
    }

    const WorkRecord* WorkTaskModel::recordAt(const int row) const
    {
        return row >= 0 && static_cast<std::size_t>(row) < m_records.size()
                   ? &m_records.at(static_cast<std::size_t>(row))
                   : nullptr;
    }

    void WorkTaskModel::reload()
    {
        const auto result = m_taskPort.list();
        const auto* records = std::get_if<std::vector<WorkRecord>>(&result);
        if (records == nullptr)
            return;
        const bool sameRows =
            m_records.size() == records->size() &&
            std::ranges::equal(m_records, *records, {}, &WorkRecord::jobId, &WorkRecord::jobId);
        if (!sameRows)
        {
            beginResetModel();
            m_records = *records;
            endResetModel();
            return;
        }
        m_records = *records;
        if (!m_records.empty())
            Q_EMIT dataChanged(index(0, 0),
                               index(static_cast<int>(m_records.size()) - 1, columnCount() - 1));
    }
} // namespace javelin::app
