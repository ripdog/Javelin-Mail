#pragma once

#include "jmap/cache/Database.h"

#include <QAbstractTableModel>
#include <QObject>
#include <QTimer>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace javelin::app
{
    enum class WorkKind
    {
        FullMailSync,
        MessageDownload,
        SearchIndex,
        LegacyMigration,
        VaultProjection,
        ContactRefresh,
        CalendarRefresh,
        Maintenance,
    };

    enum class WorkPriority : int
    {
        Maintenance = 100,
        Derived = 200,
        Bulk = 300,
        Freshness = 400,
        Foreground = 500,
        Interactive = 600,
    };

    enum class WorkStatus
    {
        Queued,
        Running,
        Paused,
        WaitingForSpace,
        WaitingForNetwork,
        WaitingForAuth,
        Failed,
        Complete,
    };

    struct WorkProgress
    {
        std::uint64_t completedUnits = 0;
        std::optional<std::uint64_t> totalUnits;
        std::uint64_t completedBytes = 0;
        std::optional<std::uint64_t> totalBytes;
        QString detail;
    };

    struct WorkRecord
    {
        std::string jobId;
        std::optional<std::string> parentJobId;
        std::optional<std::string> accountId;
        WorkKind kind = WorkKind::Maintenance;
        WorkPriority priority = WorkPriority::Maintenance;
        WorkStatus status = WorkStatus::Queued;
        QString title;
        WorkProgress progress;
        QString checkpointJson = QStringLiteral("{}");
        std::optional<QString> errorText;
        bool pauseRequested = false;
    };

    struct WorkSpec
    {
        std::string jobId;
        std::optional<std::string> parentJobId;
        std::optional<std::string> accountId;
        WorkKind kind = WorkKind::Maintenance;
        WorkPriority priority = WorkPriority::Maintenance;
        QString title;
        QString checkpointJson = QStringLiteral("{}");
    };

    class WorkScheduler final : public QObject
    {
        Q_OBJECT

      public:
        explicit WorkScheduler(javelin::jmap::cache::DatabaseConnection& connection,
                               QObject* parent = nullptr,
                               std::chrono::milliseconds quietPeriod = std::chrono::milliseconds{
                                   5000});

        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        ensure(const WorkSpec& spec);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        update(std::string_view jobId, WorkStatus status, const WorkProgress& progress,
               QString checkpointJson = QStringLiteral("{}"),
               std::optional<QString> errorText = std::nullopt);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        pause(std::string_view jobId);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        resume(std::string_view jobId);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        retry(std::string_view jobId);
        [[nodiscard]] std::variant<std::vector<WorkRecord>, javelin::jmap::cache::DatabaseError>
        list() const;
        [[nodiscard]] std::variant<std::optional<WorkRecord>, javelin::jmap::cache::DatabaseError>
        find(std::string_view jobId) const;

        void beginForegroundWork();
        void endForegroundWork();
        [[nodiscard]] bool mayStartBackgroundNetwork() const;
        [[nodiscard]] QString summary() const;

      Q_SIGNALS:
        void jobsChanged();
        void foregroundAvailabilityChanged();

      private:
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        setControlStatus(std::string_view jobId, WorkStatus status, bool pauseRequested);

        javelin::jmap::cache::DatabaseConnection& m_connection;
        int m_foregroundDepth = 0;
        QTimer m_quietTimer;
    };

    class WorkTaskModel final : public QAbstractTableModel
    {
        Q_OBJECT

      public:
        explicit WorkTaskModel(WorkScheduler& scheduler, QObject* parent = nullptr);
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
        WorkScheduler& m_scheduler;
        std::vector<WorkRecord> m_records;
    };

    [[nodiscard]] std::string_view toString(WorkKind kind);
    [[nodiscard]] std::string_view toString(WorkStatus status);

} // namespace javelin::app
