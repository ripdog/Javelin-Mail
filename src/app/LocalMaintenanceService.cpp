#include "app/LocalMaintenanceService.h"

#include "app/WorkScheduler.h"
#include "jmap/cache/Database.h"
#include "jmap/cache/RawMessageSourceRepository.h"

#include <QCoroFuture>
#include <QCoroTask>

#include <KLocalizedString>

#include <QSqlError>
#include <QSqlQuery>
#include <QTimer>
#include <QtConcurrentRun>

namespace javelin::app
{
    namespace
    {
        struct MaintenanceResult
        {
            std::size_t migrated = 0;
            std::size_t evicted = 0;
            bool migrationComplete = false;
            std::size_t pendingProjections = 0;
            QString error;
        };

        [[nodiscard]] MaintenanceResult performMaintenance(QString databasePath)
        {
            auto opened = javelin::jmap::cache::DatabaseConnection::open({
                .connectionName =
                    QStringLiteral("local-maintenance-%1")
                        .arg(javelin::jmap::cache::ThreadConnectionFactory::currentThreadTag()),
                .databasePath = std::move(databasePath),
            });
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&opened))
                return {.error = error->message};
            auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
            javelin::jmap::cache::RawMessageSourceRepository sources{connection};
            const auto migrated = sources.migrateLegacySources(4);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&migrated))
                return {.error = error->message};
            if (const auto error = sources.replayProjectionJobs(100))
                return {.error = error->message};
            const auto evicted = sources.evictUnretained(25);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&evicted))
                return {.error = error->message};

            QSqlQuery state{connection.database()};
            if (!state.exec(QStringLiteral(
                    "SELECT status='complete' FROM local_data_migrations WHERE migration_key="
                    "'raw_message_sources_to_vault'")) ||
                !state.next())
                return {.error = state.lastError().text()};
            QSqlQuery projections{connection.database()};
            if (!projections.exec(QStringLiteral(
                    "SELECT COUNT(*) FROM mail_vault_projection_jobs WHERE status='pending'")) ||
                !projections.next())
                return {.error = projections.lastError().text()};
            return {.migrated = std::get<std::size_t>(migrated),
                    .evicted = std::get<std::size_t>(evicted),
                    .migrationComplete = state.value(0).toBool(),
                    .pendingProjections =
                        static_cast<std::size_t>(projections.value(0).toULongLong()),
                    .error = {}};
        }
    } // namespace

    LocalMaintenanceService::LocalMaintenanceService(
        javelin::jmap::cache::DatabaseConnection& connection, WorkScheduler& scheduler,
        QObject* parent)
        : QObject(parent), m_connection(connection), m_scheduler(scheduler)
    {
        const javelin::jmap::cache::DatabaseWriteScope writeScope{m_connection};
        QSqlQuery retryProjections{m_connection.database()};
        static_cast<void>(retryProjections.exec(QStringLiteral(
            "UPDATE mail_vault_projection_jobs SET status='pending' WHERE status='failed'")));
        static_cast<void>(m_scheduler.ensure({
            .jobId = "legacy-mail-vault-migration",
            .parentJobId = std::nullopt,
            .accountId = std::nullopt,
            .kind = WorkKind::LegacyMigration,
            .priority = WorkPriority::Maintenance,
            .title = i18n("Move saved messages into mail vault"),
            .checkpointJson = QStringLiteral("{}"),
        }));
        static_cast<void>(m_scheduler.ensure({
            .jobId = "mail-vault-projections",
            .parentJobId = std::nullopt,
            .accountId = std::nullopt,
            .kind = WorkKind::VaultProjection,
            .priority = WorkPriority::Maintenance,
            .title = i18n("Update mail vault folders"),
            .checkpointJson = QStringLiteral("{}"),
        }));
        connect(&m_scheduler, &WorkScheduler::foregroundAvailabilityChanged, this,
                [this]() { schedule(); });
        schedule();
    }

    void LocalMaintenanceService::requestReplay()
    {
        m_replayRequested = true;
        if (!m_running && !hasPendingMaintenance())
        {
            m_replayRequested = false;
            m_complete = true;
            return;
        }
        m_complete = false;
        schedule();
    }

    bool LocalMaintenanceService::hasPendingMaintenance() const
    {
        QSqlQuery query{m_connection.database()};
        if (!query.exec(QStringLiteral(
                "SELECT EXISTS(SELECT 1 FROM mail_vault_projection_jobs WHERE status='pending') "
                "OR EXISTS(SELECT 1 FROM local_data_migrations WHERE status<>'complete')")) ||
            !query.next())
            return true;
        return query.value(0).toBool();
    }

    void LocalMaintenanceService::schedule()
    {
        if (m_scheduled || m_running || m_complete)
            return;
        m_scheduled = true;
        QTimer::singleShot(0, this,
                           [this]()
                           {
                               m_scheduled = false;
                               if (!m_scheduler.mayStartBackgroundNetwork())
                                   return;
                               std::optional<std::string> admissionJob;
                               if (m_scheduler.admit("legacy-mail-vault-migration").has_value())
                                   admissionJob = "legacy-mail-vault-migration";
                               else if (m_scheduler.admit("mail-vault-projections").has_value())
                                   admissionJob = "mail-vault-projections";
                               if (!admissionJob.has_value())
                                   return;
                               m_running = true;
                               m_replayRequested = false;
                               auto task = run();
                               QCoro::connect(std::move(task), this,
                                              [this, admissionJob = std::move(admissionJob)]()
                                              {
                                                  m_scheduler.release(*admissionJob);
                                                  m_running = false;
                                                  if (m_replayRequested)
                                                  {
                                                      m_replayRequested = false;
                                                      requestReplay();
                                                  }
                                                  else
                                                  {
                                                      schedule();
                                                  }
                                              });
                           });
    }

    QCoro::Task<void> LocalMaintenanceService::run()
    {
        WorkProgress migrationProgress{.completedUnits = m_migrated,
                                       .totalUnits = std::nullopt,
                                       .completedBytes = 0,
                                       .totalBytes = std::nullopt,
                                       .detail = i18n("Migrating saved messages")};
        static_cast<void>(m_scheduler.update("legacy-mail-vault-migration", WorkStatus::Running,
                                             migrationProgress));
        auto future = QtConcurrent::run(performMaintenance, m_connection.database().databaseName());
        const auto result = co_await qCoro(future).takeResult();
        if (!result.error.isEmpty())
        {
            static_cast<void>(m_scheduler.update("legacy-mail-vault-migration", WorkStatus::Failed,
                                                 migrationProgress, QStringLiteral("{}"),
                                                 result.error));
            co_return;
        }
        m_migrated += result.migrated;
        migrationProgress.completedUnits = m_migrated;
        migrationProgress.detail = result.migrationComplete
                                       ? i18n("Saved message migration complete")
                                       : i18n("Migrating saved messages");
        if (result.evicted > 0)
            migrationProgress.detail =
                i18np("Evicted %1 unretained vault object",
                      "Evicted %1 unretained vault objects", result.evicted);
        static_cast<void>(
            m_scheduler.update("legacy-mail-vault-migration",
                               result.migrationComplete ? WorkStatus::Complete : WorkStatus::Queued,
                               migrationProgress));

        WorkProgress projectionProgress{.completedUnits = 0,
                                        .totalUnits = result.pendingProjections,
                                        .completedBytes = 0,
                                        .totalBytes = std::nullopt,
                                        .detail = result.pendingProjections == 0
                                                      ? i18n("Mail folders are current")
                                                      : i18n("Updating mail folders")};
        static_cast<void>(m_scheduler.update("mail-vault-projections",
                                             result.pendingProjections == 0 ? WorkStatus::Complete
                                                                            : WorkStatus::Queued,
                                             projectionProgress));
        m_complete = result.migrationComplete && result.pendingProjections == 0;
    }
} // namespace javelin::app
