#pragma once

#include "app/WorkTaskPort.h"

#include "jmap/cache/Database.h"

#include <QObject>
#include <QTimer>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace javelin::app
{
    enum class WorkClass
    {
        ForegroundCommand,
        VisibleMaterialization,
        Prefetch,
        Indexing,
        OfflineSynchronization,
        Maintenance,
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
        bool restartCompleted = false;
    };

    struct WorkAdmission
    {
        std::string jobId;
        std::optional<std::string> accountId;
        WorkPriority priority = WorkPriority::Maintenance;
        std::uint64_t sequence = 0;
        std::chrono::steady_clock::time_point admittedAt;
    };

    struct WorkAdmissionMetrics
    {
        std::uint64_t admitted = 0;
        std::uint64_t completed = 0;
        std::uint64_t rejected = 0;
        std::chrono::microseconds totalQueueWait{};
        std::chrono::microseconds maximumQueueWait{};
        std::chrono::microseconds totalTransactionTime{};
        std::chrono::microseconds totalForegroundTime{};
        std::chrono::microseconds totalForegroundAdmissionLatency{};
    };

    class WorkScheduler final : public QObject, public WorkTaskPort
    {
        Q_OBJECT

      public:
        static constexpr std::size_t maximumQueuedWork = 256;

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
        pause(std::string_view jobId) override;
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        resume(std::string_view jobId) override;
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        retry(std::string_view jobId) override;
        [[nodiscard]] std::variant<std::vector<WorkRecord>, javelin::jmap::cache::DatabaseError>
        list() const override;
        [[nodiscard]] std::variant<std::optional<WorkRecord>, javelin::jmap::cache::DatabaseError>
        find(std::string_view jobId) const;

        [[nodiscard]] std::optional<WorkAdmission> admit(std::string_view jobId);
        [[nodiscard]] std::optional<WorkAdmission>
        admitTransient(std::string jobId, std::optional<std::string> accountId,
                       WorkPriority priority);
        void release(std::string_view jobId);
        void recordTransactionDuration(std::chrono::microseconds duration);
        void recordForegroundAdmissionLatency(std::chrono::microseconds duration);
        [[nodiscard]] WorkAdmissionMetrics admissionMetrics() const;
        [[nodiscard]] std::size_t activeAdmissions() const;

        void beginForegroundWork();
        void endForegroundWork();
        [[nodiscard]] bool mayStartBackgroundNetwork() const;
        [[nodiscard]] QString summary() const override;
        [[nodiscard]] QMetaObject::Connection
        connectChanged(QObject* context, std::function<void()> callback) override;

      Q_SIGNALS:
        void jobsChanged();
        void foregroundAvailabilityChanged();

      private:
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        setControlStatus(std::string_view jobId, WorkStatus status, bool pauseRequested);
        [[nodiscard]] bool mayStartNetwork(WorkPriority priority) const;

        javelin::jmap::cache::DatabaseConnection& m_connection;
        int m_foregroundDepth = 0;
        QTimer m_quietTimer;
        std::uint64_t m_nextAdmissionSequence = 1;
        std::unordered_map<std::string, WorkAdmission> m_admissions;
        std::unordered_map<std::string, std::size_t> m_activeAccounts;
        std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_firstQueuedAt;
        WorkAdmissionMetrics m_admissionMetrics;
        std::optional<std::chrono::steady_clock::time_point> m_foregroundStartedAt;
        std::size_t m_maxConcurrentAdmissions = 2;
    };

    [[nodiscard]] WorkClass classify(WorkKind kind);
    [[nodiscard]] WorkClass classify(WorkKind kind, WorkPriority priority);

} // namespace javelin::app
