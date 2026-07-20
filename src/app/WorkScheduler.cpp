#include "app/WorkScheduler.h"

#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>

#include <algorithm>

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
    }

    std::optional<javelin::jmap::cache::DatabaseError> WorkScheduler::ensure(const WorkSpec& spec)
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "INSERT INTO "
            "background_jobs(job_id,parent_job_id,account_id,kind,priority,status,title,"
            "checkpoint_json) VALUES(:id,:parent,:account,:kind,:priority,'queued',:title,"
            ":checkpoint) ON CONFLICT(job_id) DO UPDATE SET title=excluded.title,priority=excluded."
            "priority,account_id=excluded.account_id,parent_job_id=excluded.parent_job_id"));
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
        if (!query.exec())
            return queryError(QStringLiteral("Create background job"), query);
        Q_EMIT jobsChanged();
        return std::nullopt;
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    WorkScheduler::update(const std::string_view jobId, const WorkStatus status,
                          const WorkProgress& progress, QString checkpointJson,
                          std::optional<QString> errorText)
    {
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
            return queryError(QStringLiteral("Update background job"), query);
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
            return queryError(QStringLiteral("List background jobs"), query);
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
            return queryError(QStringLiteral("Read background job"), query);
        if (!query.next())
            return std::optional<WorkRecord>{};
        return std::optional<WorkRecord>{record(query)};
    }

    void WorkScheduler::beginForegroundWork()
    {
        ++m_foregroundDepth;
        m_quietTimer.stop();
        Q_EMIT foregroundAvailabilityChanged();
    }

    void WorkScheduler::endForegroundWork()
    {
        m_foregroundDepth = std::max(0, m_foregroundDepth - 1);
        if (m_foregroundDepth == 0)
        {
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
        const auto active = std::ranges::count_if(
            *values, [](const WorkRecord& item)
            { return item.status == WorkStatus::Running || item.status == WorkStatus::Queued; });
        return active == 0 ? QString{}
                           : QStringLiteral("%1 background task%2")
                                 .arg(active)
                                 .arg(active == 1 ? QString{} : QStringLiteral("s"));
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    WorkScheduler::setControlStatus(const std::string_view jobId, const WorkStatus status,
                                    const bool pauseRequested)
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "UPDATE background_jobs SET status=:status,pause_requested=:pause,error_text=NULL,"
            "updated_at=CURRENT_TIMESTAMP WHERE job_id=:id"));
        query.bindValue(QStringLiteral(":status"),
                        QString::fromStdString(std::string{toString(status)}));
        query.bindValue(QStringLiteral(":pause"), pauseRequested ? 1 : 0);
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{jobId}));
        if (!query.exec())
            return queryError(QStringLiteral("Control background job"), query);
        Q_EMIT jobsChanged();
        return std::nullopt;
    }

    WorkTaskModel::WorkTaskModel(WorkScheduler& scheduler, QObject* parent)
        : QAbstractTableModel(parent), m_scheduler(scheduler)
    {
        connect(&m_scheduler, &WorkScheduler::jobsChanged, this, &WorkTaskModel::reload);
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
        const auto result = m_scheduler.list();
        const auto* records = std::get_if<std::vector<WorkRecord>>(&result);
        if (records == nullptr)
            return;
        beginResetModel();
        m_records = *records;
        endResetModel();
    }

    std::string_view toString(const WorkKind kind)
    {
        switch (kind)
        {
        case WorkKind::FullMailSync:
            return "full_mail_sync";
        case WorkKind::MessageDownload:
            return "message_download";
        case WorkKind::SearchIndex:
            return "search_index";
        case WorkKind::LegacyMigration:
            return "legacy_migration";
        case WorkKind::VaultProjection:
            return "vault_projection";
        case WorkKind::ContactRefresh:
            return "contact_refresh";
        case WorkKind::CalendarRefresh:
            return "calendar_refresh";
        case WorkKind::Maintenance:
            return "maintenance";
        }
        return "maintenance";
    }

    std::string_view toString(const WorkStatus status)
    {
        switch (status)
        {
        case WorkStatus::Queued:
            return "queued";
        case WorkStatus::Running:
            return "running";
        case WorkStatus::Paused:
            return "paused";
        case WorkStatus::WaitingForSpace:
            return "waiting_for_space";
        case WorkStatus::WaitingForNetwork:
            return "waiting_for_network";
        case WorkStatus::WaitingForAuth:
            return "waiting_for_auth";
        case WorkStatus::Failed:
            return "failed";
        case WorkStatus::Complete:
            return "complete";
        }
        return "queued";
    }

} // namespace javelin::app
