#include "app/WorkScheduler.h"

#include <KLocalizedString>

#include <QSqlError>
#include <QSqlQuery>

#include <algorithm>
#include <utility>

namespace javelin::app
{
    namespace
    {
        [[nodiscard]] javelin::jmap::cache::DatabaseError queryError(const QString& operation,
                                                                     const QSqlQuery& query)
        {
            return {.code = javelin::jmap::cache::DatabaseErrorCode::QueryFailed,
                    .message = operation + QStringLiteral(": ") + query.lastError().text()};
        }

        [[nodiscard]] WorkKind kindFromString(const std::string_view value)
        {
            if (value == "full_mail_sync")
                return WorkKind::FullMailSync;
            if (value == "message_download")
                return WorkKind::MessageDownload;
            if (value == "search_index")
                return WorkKind::SearchIndex;
            if (value == "legacy_migration")
                return WorkKind::LegacyMigration;
            if (value == "vault_projection")
                return WorkKind::VaultProjection;
            if (value == "contact_refresh")
                return WorkKind::ContactRefresh;
            if (value == "calendar_refresh")
                return WorkKind::CalendarRefresh;
            if (value == "tag_deletion")
                return WorkKind::TagDeletion;
            return WorkKind::Maintenance;
        }

        [[nodiscard]] WorkStatus statusFromString(const std::string_view value)
        {
            if (value == "running")
                return WorkStatus::Running;
            if (value == "paused")
                return WorkStatus::Paused;
            if (value == "waiting_for_space")
                return WorkStatus::WaitingForSpace;
            if (value == "waiting_for_network")
                return WorkStatus::WaitingForNetwork;
            if (value == "waiting_for_auth")
                return WorkStatus::WaitingForAuth;
            if (value == "failed")
                return WorkStatus::Failed;
            if (value == "complete")
                return WorkStatus::Complete;
            return WorkStatus::Queued;
        }

        [[nodiscard]] std::optional<std::string> optionalString(const QVariant& value)
        {
            return value.isNull() ? std::nullopt
                                  : std::optional<std::string>{value.toString().toStdString()};
        }

        [[nodiscard]] WorkRecord record(const QSqlQuery& query)
        {
            return {
                .jobId = query.value(0).toString().toStdString(),
                .parentJobId = optionalString(query.value(1)),
                .accountId = optionalString(query.value(2)),
                .kind = kindFromString(query.value(3).toString().toStdString()),
                .priority = static_cast<WorkPriority>(query.value(4).toInt()),
                .status = statusFromString(query.value(5).toString().toStdString()),
                .title = query.value(6).toString(),
                .progress = {.completedUnits = query.value(8).toULongLong(),
                             .totalUnits =
                                 query.value(9).isNull()
                                     ? std::nullopt
                                     : std::optional<std::uint64_t>{query.value(9).toULongLong()},
                             .completedBytes = query.value(10).toULongLong(),
                             .totalBytes =
                                 query.value(11).isNull()
                                     ? std::nullopt
                                     : std::optional<std::uint64_t>{query.value(11).toULongLong()},
                             .detail = query.value(7).toString()},
                .checkpointJson = query.value(12).toString(),
                .errorText = query.value(13).isNull()
                                 ? std::nullopt
                                 : std::optional<QString>{query.value(13).toString()},
                .pauseRequested = query.value(14).toBool(),
            };
        }

        [[nodiscard]] QString columns()
        {
            return QStringLiteral(
                "job_id,parent_job_id,account_id,kind,priority,status,title,detail,"
                "completed_units,total_units,completed_bytes,total_bytes,checkpoint_json,"
                "error_text,pause_requested");
        }
    } // namespace

    WorkScheduler::WorkScheduler(javelin::jmap::cache::DatabaseConnection& connection,
                                 QObject* parent, const std::chrono::milliseconds quietPeriod)
        : QObject(parent), m_connection(connection)
    {
        const javelin::jmap::cache::DatabaseWriteScope writeScope{m_connection};
        QSqlQuery recover{m_connection.database()};
        if (!recover.exec(QStringLiteral(
                "UPDATE background_jobs SET status=CASE WHEN pause_requested=1 THEN 'paused' ELSE "
                "'queued' END,updated_at=CURRENT_TIMESTAMP WHERE status='running'")))
        {
            qWarning().noquote() << "Background job recovery failed" << recover.lastError().text();
        }
        m_quietTimer.setSingleShot(true);
        m_quietTimer.setInterval(quietPeriod);
        connect(&m_quietTimer, &QTimer::timeout, this,
                &WorkScheduler::foregroundAvailabilityChanged);
        if (m_quietTimer.interval() > 0)
            m_quietTimer.start();
    }

    std::optional<javelin::jmap::cache::DatabaseError> WorkScheduler::ensure(const WorkSpec& spec)
    {
        const javelin::jmap::cache::DatabaseWriteScope writeScope{m_connection};
        QSqlQuery existing{m_connection.database()};
        existing.prepare(
            QStringLiteral("SELECT EXISTS(SELECT 1 FROM background_jobs WHERE job_id=:job_id)"));
        existing.bindValue(QStringLiteral(":job_id"), QString::fromStdString(spec.jobId));
        if (!existing.exec() || !existing.next())
            return queryError(i18n("Inspect background work queue"), existing);
        if (!existing.value(0).toBool())
        {
            QSqlQuery queued{m_connection.database()};
            if (!queued.exec(QStringLiteral(
                    "SELECT COUNT(*) FROM background_jobs WHERE status IN "
                    "('queued','waiting_for_space','waiting_for_network','waiting_for_auth')")))
                return queryError(i18n("Count background work queue"), queued);
            if (!queued.next())
                return queryError(i18n("Count background work queue"), queued);
            if (queued.value(0).toULongLong() >= maximumQueuedWork)
            {
                return javelin::jmap::cache::DatabaseError{
                    .code = javelin::jmap::cache::DatabaseErrorCode::QueryFailed,
                    .message = i18n("The background work queue is full."),
                };
            }
        }
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "INSERT INTO "
            "background_jobs(job_id,parent_job_id,account_id,kind,priority,status,title,"
            "checkpoint_json) VALUES(:id,:parent,:account,:kind,:priority,'queued',:title,"
            ":checkpoint) ON CONFLICT(job_id) DO UPDATE SET title=excluded.title,priority=excluded."
            "priority,account_id=excluded.account_id,parent_job_id=excluded.parent_job_id,"
            "status=CASE WHEN (background_jobs.status='failed' OR "
            "(:restart_completed=1 AND background_jobs.status='complete')) AND "
            "background_jobs.pause_requested=0 THEN 'queued' ELSE background_jobs.status END,"
            "error_text=CASE WHEN (background_jobs.status='failed' OR "
            "(:restart_completed=1 AND background_jobs.status='complete')) AND "
            "background_jobs.pause_requested=0 THEN NULL ELSE background_jobs.error_text END,"
            "updated_at=CURRENT_TIMESTAMP"));
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(spec.jobId));
        query.bindValue(QStringLiteral(":parent"),
                        spec.parentJobId ? QVariant{QString::fromStdString(*spec.parentJobId)}
                                         : QVariant{});
        query.bindValue(QStringLiteral(":account"),
                        spec.accountId ? QVariant{QString::fromStdString(*spec.accountId)}
                                       : QVariant{});
        query.bindValue(QStringLiteral(":kind"),
                        QString::fromStdString(std::string{toString(spec.kind)}));
        query.bindValue(QStringLiteral(":priority"), static_cast<int>(spec.priority));
        query.bindValue(QStringLiteral(":title"), spec.title);
        query.bindValue(QStringLiteral(":checkpoint"), spec.checkpointJson);
        query.bindValue(QStringLiteral(":restart_completed"), spec.restartCompleted ? 1 : 0);
        if (!query.exec())
            return queryError(i18n("Create background job"), query);
        m_firstQueuedAt.try_emplace(spec.jobId, std::chrono::steady_clock::now());
        Q_EMIT jobsChanged();
        return std::nullopt;
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    WorkScheduler::update(const std::string_view jobId, const WorkStatus status,
                          const WorkProgress& progress, QString checkpointJson,
                          std::optional<QString> errorText)
    {
        const javelin::jmap::cache::DatabaseWriteScope writeScope{m_connection};
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "UPDATE background_jobs SET status=:status,detail=:detail,completed_units=:units,"
            "total_units=:total_units,completed_bytes=:bytes,total_bytes=:total_bytes,"
            "checkpoint_json=:checkpoint,error_text=:error,updated_at=CURRENT_TIMESTAMP WHERE "
            "job_id=:id"));
        query.bindValue(QStringLiteral(":status"),
                        QString::fromStdString(std::string{toString(status)}));
        query.bindValue(QStringLiteral(":detail"), progress.detail);
        query.bindValue(QStringLiteral(":units"), static_cast<qulonglong>(progress.completedUnits));
        query.bindValue(QStringLiteral(":total_units"),
                        progress.totalUnits
                            ? QVariant{static_cast<qulonglong>(*progress.totalUnits)}
                            : QVariant{});
        query.bindValue(QStringLiteral(":bytes"), static_cast<qulonglong>(progress.completedBytes));
        query.bindValue(QStringLiteral(":total_bytes"),
                        progress.totalBytes
                            ? QVariant{static_cast<qulonglong>(*progress.totalBytes)}
                            : QVariant{});
        query.bindValue(QStringLiteral(":checkpoint"), checkpointJson);
        query.bindValue(QStringLiteral(":error"), errorText ? QVariant{*errorText} : QVariant{});
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{jobId}));
        if (!query.exec())
            return queryError(i18n("Update background job"), query);
        Q_EMIT jobsChanged();
        return std::nullopt;
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    WorkScheduler::pause(const std::string_view jobId)
    {
        return setControlStatus(jobId, WorkStatus::Paused, true);
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    WorkScheduler::resume(const std::string_view jobId)
    {
        return setControlStatus(jobId, WorkStatus::Queued, false);
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    WorkScheduler::retry(const std::string_view jobId)
    {
        return setControlStatus(jobId, WorkStatus::Queued, false);
    }

    std::variant<std::vector<WorkRecord>, javelin::jmap::cache::DatabaseError>
    WorkScheduler::list() const
    {
        QSqlQuery query{m_connection.database()};
        if (!query.exec(
                QStringLiteral("SELECT %1 FROM background_jobs ORDER BY CASE status WHEN "
                               "'running' THEN 0 WHEN 'queued' THEN 1 WHEN 'waiting_for_network' "
                               "THEN 2 WHEN 'waiting_for_space' THEN 3 WHEN 'waiting_for_auth' "
                               "THEN 4 WHEN 'paused' THEN 5 WHEN 'failed' THEN 6 ELSE 7 END,"
                               "priority DESC,created_at ASC,job_id")
                    .arg(columns())))
            return queryError(i18n("List background jobs"), query);
        std::vector<WorkRecord> records;
        while (query.next())
            records.push_back(record(query));
        return records;
    }

    std::variant<std::optional<WorkRecord>, javelin::jmap::cache::DatabaseError>
    WorkScheduler::find(const std::string_view jobId) const
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(
            QStringLiteral("SELECT %1 FROM background_jobs WHERE job_id=:id").arg(columns()));
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{jobId}));
        if (!query.exec())
            return queryError(i18n("Read background job"), query);
        if (!query.next())
            return std::optional<WorkRecord>{};
        return std::optional<WorkRecord>{record(query)};
    }

    std::optional<WorkAdmission> WorkScheduler::admit(const std::string_view jobId)
    {
        const auto found = find(jobId);
        const auto* recordValue = std::get_if<std::optional<WorkRecord>>(&found);
        if (recordValue == nullptr || !recordValue->has_value() ||
            (*recordValue)->status != WorkStatus::Queued || (*recordValue)->pauseRequested)
        {
            ++m_admissionMetrics.rejected;
            return std::nullopt;
        }

        const auto& job = **recordValue;
        if (m_admissions.contains(job.jobId))
        {
            ++m_admissionMetrics.rejected;
            return std::nullopt;
        }
        if (static_cast<int>(job.priority) < static_cast<int>(WorkPriority::Foreground) &&
            !mayStartBackgroundNetwork())
        {
            ++m_admissionMetrics.rejected;
            return std::nullopt;
        }
        if (m_admissions.size() >= m_maxConcurrentAdmissions)
        {
            ++m_admissionMetrics.rejected;
            return std::nullopt;
        }
        if (job.accountId.has_value() && m_activeAccounts.contains(*job.accountId))
        {
            ++m_admissionMetrics.rejected;
            return std::nullopt;
        }

        const auto records = list();
        const auto* queued = std::get_if<std::vector<WorkRecord>>(&records);
        if (queued == nullptr)
        {
            ++m_admissionMetrics.rejected;
            return std::nullopt;
        }
        for (const auto& candidate : *queued)
        {
            if (candidate.status != WorkStatus::Queued || candidate.pauseRequested)
                continue;
            if (static_cast<int>(candidate.priority) < static_cast<int>(WorkPriority::Foreground) &&
                !mayStartBackgroundNetwork())
                continue;
            if (candidate.accountId.has_value() && m_activeAccounts.contains(*candidate.accountId))
                continue;
            if (static_cast<int>(candidate.priority) > static_cast<int>(job.priority) ||
                (candidate.priority == job.priority && candidate.jobId != job.jobId))
            {
                ++m_admissionMetrics.rejected;
                return std::nullopt;
            }
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto firstQueued = m_firstQueuedAt.find(job.jobId);
        const auto queueWait =
            firstQueued == m_firstQueuedAt.end()
                ? std::chrono::microseconds{}
                : std::chrono::duration_cast<std::chrono::microseconds>(now - firstQueued->second);
        m_admissionMetrics.totalQueueWait += queueWait;
        m_admissionMetrics.maximumQueueWait =
            std::max(m_admissionMetrics.maximumQueueWait, queueWait);
        WorkAdmission admission{
            .jobId = job.jobId,
            .accountId = job.accountId,
            .priority = job.priority,
            .sequence = m_nextAdmissionSequence++,
            .admittedAt = now,
        };
        m_admissions.emplace(admission.jobId, admission);
        if (job.accountId.has_value())
            ++m_activeAccounts[*job.accountId];
        ++m_admissionMetrics.admitted;
        Q_EMIT foregroundAvailabilityChanged();
        return admission;
    }

    void WorkScheduler::release(const std::string_view jobId)
    {
        const auto found = m_admissions.find(std::string{jobId});
        if (found == m_admissions.end())
            return;
        if (found->second.accountId.has_value())
        {
            const auto account = m_activeAccounts.find(*found->second.accountId);
            if (account != m_activeAccounts.end())
            {
                if (account->second <= 1)
                    m_activeAccounts.erase(account);
                else
                    --account->second;
            }
        }
        m_admissions.erase(found);
        const auto current = find(jobId);
        const auto* currentRecord = std::get_if<std::optional<WorkRecord>>(&current);
        if (currentRecord == nullptr || !currentRecord->has_value() ||
            (*currentRecord)->status != WorkStatus::Queued)
            m_firstQueuedAt.erase(std::string{jobId});
        ++m_admissionMetrics.completed;
        Q_EMIT foregroundAvailabilityChanged();
    }

    void WorkScheduler::recordTransactionDuration(const std::chrono::microseconds duration)
    {
        m_admissionMetrics.totalTransactionTime += duration;
    }

    void WorkScheduler::recordForegroundAdmissionLatency(const std::chrono::microseconds duration)
    {
        m_admissionMetrics.totalForegroundAdmissionLatency += duration;
    }

    WorkAdmissionMetrics WorkScheduler::admissionMetrics() const
    {
        return m_admissionMetrics;
    }

    std::size_t WorkScheduler::activeAdmissions() const
    {
        return m_admissions.size();
    }

    void WorkScheduler::beginForegroundWork()
    {
        if (m_foregroundDepth == 0)
            m_foregroundStartedAt = std::chrono::steady_clock::now();
        ++m_foregroundDepth;
        m_quietTimer.stop();
        Q_EMIT foregroundAvailabilityChanged();
    }

    void WorkScheduler::endForegroundWork()
    {
        m_foregroundDepth = std::max(0, m_foregroundDepth - 1);
        if (m_foregroundDepth == 0)
        {
            if (m_foregroundStartedAt.has_value())
            {
                m_admissionMetrics.totalForegroundTime +=
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - *m_foregroundStartedAt);
                m_foregroundStartedAt.reset();
            }
            if (m_quietTimer.interval() == 0)
                m_quietTimer.stop();
            else
                m_quietTimer.start();
        }
        Q_EMIT foregroundAvailabilityChanged();
    }

    bool WorkScheduler::mayStartBackgroundNetwork() const
    {
        return m_foregroundDepth == 0 && !m_quietTimer.isActive();
    }

    QString WorkScheduler::summary() const
    {
        const auto records = list();
        const auto* values = std::get_if<std::vector<WorkRecord>>(&records);
        if (values == nullptr)
            return {};

        const auto countStatus = [values](const WorkStatus status)
        {
            return std::ranges::count_if(*values, [status](const WorkRecord& item)
                                         { return item.status == status; });
        };
        const auto failed = countStatus(WorkStatus::Failed);
        if (failed > 0)
            return i18np("%1 background task failed", "%1 background tasks failed", failed);

        const auto waiting = countStatus(WorkStatus::WaitingForSpace) +
                             countStatus(WorkStatus::WaitingForNetwork) +
                             countStatus(WorkStatus::WaitingForAuth);
        if (waiting > 0)
            return i18np("%1 background task waiting", "%1 background tasks waiting", waiting);

        const auto running = countStatus(WorkStatus::Running);
        const auto queued = countStatus(WorkStatus::Queued);
        const auto active = running + queued;
        if (active == 0)
            return {};
        if (active == 1 && running == 1)
        {
            const auto current =
                std::ranges::find(*values, WorkStatus::Running, &WorkRecord::status);
            if (current != values->end())
            {
                QString detail = current->progress.detail;
                if (current->progress.totalUnits.has_value() && *current->progress.totalUnits > 0)
                {
                    const auto progress = QStringLiteral("%1 / %2")
                                              .arg(current->progress.completedUnits)
                                              .arg(*current->progress.totalUnits);
                    detail = detail.isEmpty() ? progress
                                              : QStringLiteral("%1 — %2").arg(detail, progress);
                }
                return detail.isEmpty() ? current->title
                                        : QStringLiteral("%1 — %2").arg(current->title, detail);
            }
        }
        return i18np("%1 background task", "%1 background tasks", active);
    }

    QMetaObject::Connection WorkScheduler::connectChanged(QObject* context,
                                                          std::function<void()> callback)
    {
        return QObject::connect(this, &WorkScheduler::jobsChanged, context, std::move(callback));
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    WorkScheduler::setControlStatus(const std::string_view jobId, const WorkStatus status,
                                    const bool pauseRequested)
    {
        const javelin::jmap::cache::DatabaseWriteScope writeScope{m_connection};
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "UPDATE background_jobs SET status=:status,pause_requested=:pause,error_text=NULL,"
            "updated_at=CURRENT_TIMESTAMP WHERE job_id=:id"));
        query.bindValue(QStringLiteral(":status"),
                        QString::fromStdString(std::string{toString(status)}));
        query.bindValue(QStringLiteral(":pause"), pauseRequested ? 1 : 0);
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{jobId}));
        if (!query.exec())
            return queryError(i18n("Control background job"), query);
        Q_EMIT jobsChanged();
        return std::nullopt;
    }

    WorkClass classify(const WorkKind kind)
    {
        switch (kind)
        {
        case WorkKind::MessageDownload:
            return WorkClass::VisibleMaterialization;
        case WorkKind::SearchIndex:
            return WorkClass::Indexing;
        case WorkKind::FullMailSync:
            return WorkClass::OfflineSynchronization;
        case WorkKind::ContactRefresh:
        case WorkKind::CalendarRefresh:
            return WorkClass::Prefetch;
        case WorkKind::LegacyMigration:
        case WorkKind::VaultProjection:
        case WorkKind::TagDeletion:
        case WorkKind::Maintenance:
            return WorkClass::Maintenance;
        }
        return WorkClass::Maintenance;
    }

    WorkClass classify(const WorkKind kind, const WorkPriority priority)
    {
        if (static_cast<int>(priority) >= static_cast<int>(WorkPriority::Foreground))
            return WorkClass::ForegroundCommand;
        return classify(kind);
    }

} // namespace javelin::app
