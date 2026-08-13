#pragma once

#include "app/WorkTaskPort.h"

#include <QAbstractTableModel>
#include <QTimer>

#include <vector>

namespace javelin::app
{
    class WorkTaskModel final : public QAbstractTableModel
    {
        Q_OBJECT

      public:
        explicit WorkTaskModel(WorkTaskPort& taskPort, QObject* parent = nullptr);
        [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
        [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
        [[nodiscard]] QVariant data(const QModelIndex& index,
                                    int role = Qt::DisplayRole) const override;
        [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                          int role = Qt::DisplayRole) const override;
        [[nodiscard]] const WorkRecord* recordAt(int row) const;

      public Q_SLOTS:
        void reload();

      private:
        WorkTaskPort& m_taskPort;
        std::vector<WorkRecord> m_records;
        QTimer m_reloadTimer;
    };
} // namespace javelin::app
