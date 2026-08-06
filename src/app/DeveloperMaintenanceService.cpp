#include "app/DeveloperMaintenanceService.h"

#include "app/MailboxMaintenanceRegistry.h"
#include "jmap/cache/MailVault.h"
#include "jmap/cache/RawMessageSourceRepository.h"

#include <QCoroFuture>

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QtConcurrentRun>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace javelin::app
{
    namespace
    {
        using javelin::jmap::cache::DatabaseConnection;
        using javelin::jmap::cache::DatabaseError;
        using javelin::jmap::cache::DatabaseErrorCode;
        using javelin::jmap::cache::DatabaseTransaction;
        using javelin::jmap::cache::DatabaseWriteScope;
        using javelin::jmap::cache::MailVault;
        using javelin::jmap::cache::MailVaultObject;

        struct BodyObject
        {
            QString contentHash;
            QString relativePath;
            std::uint64_t size = 0;
        };

        [[nodiscard]] DatabaseError queryError(const QString& operation, const QSqlQuery& query)
        {
            return javelin::jmap::cache::databaseError(operation, query.lastError());
        }

        [[nodiscard]] DatabaseError
        maintenanceError(const QString& message,
                         const DatabaseErrorCode code = DatabaseErrorCode::QueryFailed)
        {
            return {.code = code, .message = message};
        }

        [[nodiscard]] std::variant<std::uint64_t, DatabaseError> count(QSqlDatabase& database,
                                                                       const QString& statement,
                                                                       const QString& accountId,
                                                                       const QString& mailboxId)
        {
            QSqlQuery query{database};
            query.prepare(statement);
            query.bindValue(QStringLiteral(":account"), accountId);
            query.bindValue(QStringLiteral(":mailbox"), mailboxId);
            if (!query.exec() || !query.next())
                return queryError(QStringLiteral("Measure mailbox maintenance rows"), query);
            return query.value(0).toULongLong();
        }

        [[nodiscard]] std::optional<DatabaseError>
        executeTarget(QSqlDatabase& database, const QString& operation, const QString& statement,
                      const QString& accountId, const QString& mailboxId)
        {
            QSqlQuery query{database};
            query.prepare(statement);
            query.bindValue(QStringLiteral(":account"), accountId);
            query.bindValue(QStringLiteral(":mailbox"), mailboxId);
            if (!query.exec())
                return queryError(operation, query);
            return std::nullopt;
        }

        [[nodiscard]] std::variant<std::uint64_t, DatabaseError>
        advanceEmailGeneration(QSqlDatabase& database, const QString& accountId)
        {
            QSqlQuery advance{database};
            advance.prepare(QStringLiteral(
                "INSERT INTO consistency_domains(account_id,data_type,mutation_generation) "
                "VALUES(:account,'Email',1) ON CONFLICT(account_id,data_type) DO UPDATE SET "
                "mutation_generation=mutation_generation+1,updated_at=CURRENT_TIMESTAMP"));
            advance.bindValue(QStringLiteral(":account"), accountId);
            if (!advance.exec())
                return queryError(QStringLiteral("Advance mailbox maintenance generation"),
                                  advance);

            QSqlQuery generation{database};
            generation.prepare(QStringLiteral(
                "SELECT mutation_generation FROM consistency_domains WHERE account_id=:account "
                "AND data_type='Email'"));
            generation.bindValue(QStringLiteral(":account"), accountId);
            if (!generation.exec() || !generation.next())
                return queryError(QStringLiteral("Read mailbox maintenance generation"),
                                  generation);
            return generation.value(0).toULongLong();
        }

        [[nodiscard]] std::optional<DatabaseError>
        resetOfflineScope(QSqlDatabase& database, const DeveloperMailboxClearCommand& command)
        {
            QSqlQuery reset{database};
            reset.prepare(QStringLiteral(
                "UPDATE offline_mailbox_scopes SET desired=CASE WHEN :disable=1 THEN 0 ELSE "
                "desired END,status=CASE WHEN :disable=1 OR desired=0 THEN 'paused' ELSE 'pending' "
                "END,query_state=NULL,email_state=NULL,anchor_email_id=NULL,expected_total=NULL,"
                "completed_total=0,completed_bytes=0,estimated_bytes=NULL,generation=generation+1,"
                "completed_generation=NULL,latest_error=NULL,updated_at=CURRENT_TIMESTAMP WHERE "
                "account_id=:account AND mailbox_id=:mailbox"));
            reset.bindValue(QStringLiteral(":disable"),
                            command.offlinePolicy == DeveloperOfflineClearPolicy::Disable ? 1 : 0);
            reset.bindValue(QStringLiteral(":account"), command.accountId);
            reset.bindValue(QStringLiteral(":mailbox"), command.mailboxId);
            if (!reset.exec())
                return queryError(QStringLiteral("Reset offline mailbox scope"), reset);
            return std::nullopt;
        }

        [[nodiscard]] std::optional<DatabaseError>
        validateMailbox(QSqlDatabase& database, const DeveloperMailboxClearCommand& command)
        {
            QSqlQuery mailbox{database};
            mailbox.prepare(QStringLiteral(
                "SELECT 1 FROM mailboxes WHERE account_id=:account AND mailbox_id=:mailbox"));
            mailbox.bindValue(QStringLiteral(":account"), command.accountId);
            mailbox.bindValue(QStringLiteral(":mailbox"), command.mailboxId);
            if (!mailbox.exec())
                return queryError(QStringLiteral("Validate mailbox maintenance target"), mailbox);
            if (!mailbox.next())
            {
                return maintenanceError(
                    QStringLiteral("The mailbox no longer exists in the local cache."));
            }
            return std::nullopt;
        }

        [[nodiscard]] DeveloperMailboxClearResult
        clearSqlite(DatabaseConnection& connection, const DeveloperMailboxClearCommand& command,
                    const std::uint64_t maintenanceGeneration)
        {
            const DatabaseWriteScope writeScope{connection};
            auto transactionResult = DatabaseTransaction::begin(
                connection, QStringLiteral("Clear mailbox SQLite cache"));
            if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
                return *error;
            auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));
            auto& database = connection.database();
            if (const auto error = validateMailbox(database, command))
                return *error;

            auto windowItems = count(
                database,
                QStringLiteral(
                    "SELECT COUNT(*) FROM mailbox_query_window_items i JOIN mailbox_query_windows "
                    "w ON w.account_id=i.account_id AND w.query_key=i.query_key AND "
                    "w.requested_offset=i.requested_offset AND "
                    "w.requested_limit=i.requested_limit WHERE w.account_id=:account AND "
                    "w.mailbox_id=:mailbox"),
                command.accountId, command.mailboxId);
            if (const auto* error = std::get_if<DatabaseError>(&windowItems))
                return *error;
            auto windows = count(database,
                                 QStringLiteral("SELECT COUNT(*) FROM mailbox_query_windows WHERE "
                                                "account_id=:account AND mailbox_id=:mailbox"),
                                 command.accountId, command.mailboxId);
            if (const auto* error = std::get_if<DatabaseError>(&windows))
                return *error;
            auto memberships = count(
                database,
                QStringLiteral("SELECT COUNT(*) FROM email_mailboxes em WHERE "
                               "em.account_id=:account AND em.mailbox_id=:mailbox AND NOT EXISTS("
                               "SELECT 1 FROM mutation_journal j WHERE j.account_id=em.account_id "
                               "AND j.data_type='Email' AND j.object_id=em.email_id AND j.status "
                               "IN ('pending','in_flight','accepted','unknown'))"),
                command.accountId, command.mailboxId);
            if (const auto* error = std::get_if<DatabaseError>(&memberships))
                return *error;
            auto offlineMemberships =
                count(database,
                      QStringLiteral("SELECT COUNT(*) FROM offline_mailbox_membership WHERE "
                                     "account_id=:account AND mailbox_id=:mailbox"),
                      command.accountId, command.mailboxId);
            if (const auto* error = std::get_if<DatabaseError>(&offlineMemberships))
                return *error;

            if (const auto error = executeTarget(
                    database, QStringLiteral("Delete mailbox query windows"),
                    QStringLiteral("DELETE FROM mailbox_query_windows WHERE account_id=:account "
                                   "AND mailbox_id=:mailbox"),
                    command.accountId, command.mailboxId))
                return *error;
            if (const auto error =
                    executeTarget(database, QStringLiteral("Delete offline mailbox membership"),
                                  QStringLiteral("DELETE FROM offline_mailbox_membership WHERE "
                                                 "account_id=:account AND mailbox_id=:mailbox"),
                                  command.accountId, command.mailboxId))
                return *error;
            if (const auto error = executeTarget(
                    database, QStringLiteral("Delete cached mailbox membership"),
                    QStringLiteral(
                        "DELETE FROM email_mailboxes AS em WHERE em.account_id=:account AND "
                        "em.mailbox_id=:mailbox AND NOT EXISTS(SELECT 1 FROM mutation_journal j "
                        "WHERE j.account_id=em.account_id AND j.data_type='Email' AND "
                        "j.object_id=em.email_id AND j.status IN "
                        "('pending','in_flight','accepted','unknown'))"),
                    command.accountId, command.mailboxId))
                return *error;
            if (const auto error = resetOfflineScope(database, command))
                return *error;
            const auto generation = advanceEmailGeneration(database, command.accountId);
            if (const auto* error = std::get_if<DatabaseError>(&generation))
                return *error;
            if (const auto error = transaction.commit())
                return *error;

            return DeveloperMailboxClearSummary{
                .accountId = command.accountId,
                .mailboxId = command.mailboxId,
                .kind = DeveloperMailboxCacheKind::Sqlite,
                .maintenanceGeneration = maintenanceGeneration,
                .rowsDiscarded = std::get<std::uint64_t>(windowItems) +
                                 std::get<std::uint64_t>(windows) +
                                 std::get<std::uint64_t>(memberships) +
                                 std::get<std::uint64_t>(offlineMemberships),
                .projectionsRemoved = 0,
                .logicalBytesReleased = 0,
                .reclaimedBytes = 0,
                .deferredBytes = 0,
                .offlineStorageDisabled = false,
            };
        }

        [[nodiscard]] std::variant<std::vector<BodyObject>, DatabaseError>
        bodyObjects(QSqlDatabase& database, const DeveloperMailboxClearCommand& command)
        {
            QSqlQuery query{database};
            query.prepare(QStringLiteral(
                "SELECT DISTINCT o.content_hash,o.relative_path,o.size FROM "
                "mail_vault_mailbox_refs mr JOIN mail_vault_email_refs r ON "
                "r.account_id=mr.account_id AND r.email_id=mr.email_id JOIN mail_vault_objects o "
                "ON o.content_hash=r.content_hash WHERE mr.account_id=:account AND "
                "mr.mailbox_id=:mailbox ORDER BY o.content_hash"));
            query.bindValue(QStringLiteral(":account"), command.accountId);
            query.bindValue(QStringLiteral(":mailbox"), command.mailboxId);
            if (!query.exec())
                return queryError(QStringLiteral("List mailbox body objects"), query);
            std::vector<BodyObject> result;
            while (query.next())
            {
                result.push_back({.contentHash = query.value(0).toString(),
                                  .relativePath = query.value(1).toString(),
                                  .size = query.value(2).toULongLong()});
            }
            return result;
        }

        [[nodiscard]] std::variant<std::vector<QString>, DatabaseError>
        mailboxBodyEmailIds(QSqlDatabase& database, const DeveloperMailboxClearCommand& command)
        {
            QSqlQuery query{database};
            query.prepare(QStringLiteral(
                "SELECT email_id FROM mail_vault_mailbox_refs WHERE account_id=:account AND "
                "mailbox_id=:mailbox ORDER BY email_id"));
            query.bindValue(QStringLiteral(":account"), command.accountId);
            query.bindValue(QStringLiteral(":mailbox"), command.mailboxId);
            if (!query.exec())
                return queryError(QStringLiteral("List mailbox body email references"), query);
            std::vector<QString> emailIds;
            while (query.next())
                emailIds.push_back(query.value(0).toString());
            return emailIds;
        }

        [[nodiscard]] std::optional<DatabaseError>
        replayTargetProjectionJobs(DatabaseConnection& connection,
                                   const DeveloperMailboxClearCommand& command)
        {
            javelin::jmap::cache::RawMessageSourceRepository sources{connection};
            for (;;)
            {
                QSqlQuery pending{connection.database()};
                pending.prepare(QStringLiteral(
                    "SELECT COUNT(*) FROM mail_vault_projection_jobs WHERE account_id=:account "
                    "AND mailbox_id=:mailbox AND status='pending'"));
                pending.bindValue(QStringLiteral(":account"), command.accountId);
                pending.bindValue(QStringLiteral(":mailbox"), command.mailboxId);
                if (!pending.exec() || !pending.next())
                    return queryError(QStringLiteral("Count mailbox projection jobs"), pending);
                if (pending.value(0).toULongLong() == 0)
                    return std::nullopt;
                if (const auto error = sources.replayProjectionJobs(250))
                    return error;
            }
        }

        [[nodiscard]] DeveloperMailboxClearResult
        clearBodies(DatabaseConnection& connection, const QString& vaultPath,
                    const DeveloperMailboxClearCommand& command,
                    const std::uint64_t maintenanceGeneration)
        {
            auto& database = connection.database();
            if (const auto error = validateMailbox(database, command))
                return *error;
            auto objectsResult = bodyObjects(database, command);
            if (const auto* error = std::get_if<DatabaseError>(&objectsResult))
                return *error;
            const auto objects = std::get<std::vector<BodyObject>>(std::move(objectsResult));
            auto emailIdsResult = mailboxBodyEmailIds(database, command);
            if (const auto* error = std::get_if<DatabaseError>(&emailIdsResult))
                return *error;
            const auto emailIds = std::get<std::vector<QString>>(std::move(emailIdsResult));
            const std::uint64_t referenceCount = emailIds.size();

            std::uint64_t removedEmailRefs = 0;
            {
                const DatabaseWriteScope writeScope{connection};
                auto transactionResult = DatabaseTransaction::begin(
                    connection, QStringLiteral("Clear mailbox body cache"));
                if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
                    return *error;
                auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));

                QSqlQuery unlink{database};
                unlink.prepare(QStringLiteral(
                    "INSERT INTO mail_vault_projection_jobs(account_id,email_id,mailbox_id,"
                    "content_hash,operation) SELECT mr.account_id,mr.email_id,mr.mailbox_id,"
                    "r.content_hash,'unlink' FROM mail_vault_mailbox_refs mr JOIN "
                    "mail_vault_email_refs r ON r.account_id=mr.account_id AND "
                    "r.email_id=mr.email_id WHERE mr.account_id=:account AND "
                    "mr.mailbox_id=:mailbox"));
                unlink.bindValue(QStringLiteral(":account"), command.accountId);
                unlink.bindValue(QStringLiteral(":mailbox"), command.mailboxId);
                if (!unlink.exec())
                {
                    return queryError(QStringLiteral("Queue mailbox body projection removal"),
                                      unlink);
                }

                if (const auto error = executeTarget(
                        database, QStringLiteral("Delete mail vault mailbox references"),
                        QStringLiteral("DELETE FROM mail_vault_mailbox_refs WHERE "
                                       "account_id=:account AND mailbox_id=:mailbox"),
                        command.accountId, command.mailboxId))
                    return *error;

                QSqlQuery removeUnownedRef{database};
                removeUnownedRef.prepare(QStringLiteral(
                    "DELETE FROM mail_vault_email_refs AS r WHERE r.account_id=:account AND "
                    "r.email_id=:email AND NOT EXISTS(SELECT 1 FROM mail_vault_mailbox_refs mr "
                    "WHERE mr.account_id=r.account_id AND mr.email_id=r.email_id)"));
                for (const auto& emailId : emailIds)
                {
                    removeUnownedRef.bindValue(QStringLiteral(":account"), command.accountId);
                    removeUnownedRef.bindValue(QStringLiteral(":email"), emailId);
                    if (!removeUnownedRef.exec())
                    {
                        return queryError(QStringLiteral("Release unowned mailbox body reference"),
                                          removeUnownedRef);
                    }
                    removedEmailRefs += static_cast<std::uint64_t>(
                        std::max<qint64>(0, removeUnownedRef.numRowsAffected()));
                    removeUnownedRef.finish();
                }

                QSqlQuery retention{database};
                if (!retention.exec(QStringLiteral(
                        "UPDATE mail_vault_email_refs AS r SET retention=CASE WHEN EXISTS(SELECT 1 "
                        "FROM mail_vault_mailbox_refs mr JOIN offline_mailbox_scopes s ON "
                        "s.account_id=mr.account_id AND s.mailbox_id=mr.mailbox_id AND "
                        "s.desired=1 WHERE mr.account_id=r.account_id AND "
                        "mr.email_id=r.email_id) THEN 'full_sync' ELSE 'evictable' END")))
                {
                    return queryError(QStringLiteral("Update body retention after mailbox clear"),
                                      retention);
                }
                if (const auto error = resetOfflineScope(database, command))
                    return *error;
                const auto generation = advanceEmailGeneration(database, command.accountId);
                if (const auto* error = std::get_if<DatabaseError>(&generation))
                    return *error;
                if (const auto error = transaction.commit())
                    return *error;
            }

            if (const auto error = replayTargetProjectionJobs(connection, command))
                return *error;

            const MailVault vault{vaultPath};
            std::uint64_t logicalBytes = 0;
            std::uint64_t reclaimedBytes = 0;
            std::uint64_t deferredBytes = 0;
            for (const auto& object : objects)
            {
                logicalBytes += object.size;
                QSqlQuery refs{database};
                refs.prepare(QStringLiteral(
                    "SELECT COUNT(*) FROM mail_vault_email_refs WHERE content_hash=:hash"));
                refs.bindValue(QStringLiteral(":hash"), object.contentHash);
                if (!refs.exec() || !refs.next())
                    return queryError(QStringLiteral("Inspect cleared body references"), refs);
                if (refs.value(0).toULongLong() != 0)
                    continue;

                const MailVaultObject vaultObject{.contentHash = object.contentHash.toStdString(),
                                                  .relativePath = object.relativePath,
                                                  .size = object.size};
                if (vault.isLeased(vaultObject))
                {
                    deferredBytes += object.size;
                    continue;
                }
                const bool existed =
                    QFileInfo::exists(QDir{vaultPath}.filePath(object.relativePath));
                if (const auto error = vault.evict(vaultObject))
                {
                    return maintenanceError(error->message);
                }
                const DatabaseWriteScope cleanupWriteScope{connection};
                auto cleanupResult = DatabaseTransaction::begin(
                    connection, QStringLiteral("Remove cleared mail vault object"));
                if (const auto* error = std::get_if<DatabaseError>(&cleanupResult))
                    return *error;
                auto cleanup = std::get<DatabaseTransaction>(std::move(cleanupResult));
                QSqlQuery jobs{database};
                jobs.prepare(QStringLiteral(
                    "DELETE FROM mail_vault_projection_jobs WHERE content_hash=:hash"));
                jobs.bindValue(QStringLiteral(":hash"), object.contentHash);
                if (!jobs.exec())
                    return queryError(QStringLiteral("Delete cleared body projection history"),
                                      jobs);
                QSqlQuery objectRow{database};
                objectRow.prepare(
                    QStringLiteral("DELETE FROM mail_vault_objects WHERE content_hash=:hash"));
                objectRow.bindValue(QStringLiteral(":hash"), object.contentHash);
                if (!objectRow.exec())
                    return queryError(QStringLiteral("Delete cleared mail vault object"),
                                      objectRow);
                if (const auto error = cleanup.commit())
                    return *error;
                if (existed)
                    reclaimedBytes += object.size;
            }

            return DeveloperMailboxClearSummary{
                .accountId = command.accountId,
                .mailboxId = command.mailboxId,
                .kind = DeveloperMailboxCacheKind::Bodies,
                .maintenanceGeneration = maintenanceGeneration,
                .rowsDiscarded = referenceCount + removedEmailRefs,
                .projectionsRemoved = referenceCount,
                .logicalBytesReleased = logicalBytes,
                .reclaimedBytes = reclaimedBytes,
                .deferredBytes = deferredBytes,
                .offlineStorageDisabled =
                    command.offlinePolicy == DeveloperOfflineClearPolicy::Disable,
            };
        }

        [[nodiscard]] DeveloperMailboxClearResult
        clearMailbox(QString databasePath, QString vaultPath, DeveloperMailboxClearCommand command,
                     const std::uint64_t maintenanceGeneration)
        {
            auto opened = DatabaseConnection::open({
                .connectionName =
                    QStringLiteral("developer-maintenance-%1")
                        .arg(javelin::jmap::cache::ThreadConnectionFactory::currentThreadTag()),
                .databasePath = std::move(databasePath),
                .busyTimeout = std::chrono::seconds{30},
            });
            if (const auto* error = std::get_if<DatabaseError>(&opened))
                return *error;
            auto connection = std::get<DatabaseConnection>(std::move(opened));
            if (command.kind == DeveloperMailboxCacheKind::Sqlite)
                return clearSqlite(connection, command, maintenanceGeneration);
            return clearBodies(connection, vaultPath, command, maintenanceGeneration);
        }
    } // namespace

    DeveloperMaintenanceService::DeveloperMaintenanceService(
        QString databasePath, QString vaultPath, MailboxMaintenanceRegistry& registry,
        MailCacheChangePublisher& cacheChangePublisher,
        std::function<void(std::string_view, std::string_view)> requestOfflineResync)
        : m_databasePath(std::move(databasePath)), m_vaultPath(std::move(vaultPath)),
          m_registry(registry), m_cacheChangePublisher(cacheChangePublisher),
          m_requestOfflineResync(std::move(requestOfflineResync))
    {
        m_workerPool.setMaxThreadCount(1);
        m_workerPool.setExpiryTimeout(-1);
        m_workerPool.setThreadPriority(QThread::LowPriority);
    }

    DeveloperMaintenanceService::~DeveloperMaintenanceService()
    {
        m_workerPool.waitForDone();
    }

    QCoro::Task<DeveloperMailboxClearResult>
    DeveloperMaintenanceService::clearMailboxCache(DeveloperMailboxClearCommand command)
    {
        auto lease = m_registry.tryBegin(command.accountId, command.mailboxId);
        if (!lease.has_value())
        {
            co_return maintenanceError(
                QStringLiteral("Mailbox cache maintenance is already active."),
                DatabaseErrorCode::TransientContention);
        }
        const std::uint64_t generation = lease->generation();
        auto future = QtConcurrent::run(&m_workerPool, clearMailbox, m_databasePath, m_vaultPath,
                                        command, generation);
        auto result = co_await qCoro(future).takeResult();
        if (const auto* summary = std::get_if<DeveloperMailboxClearSummary>(&result))
        {
            m_cacheChangePublisher.publishCacheChange(MailCacheChange{
                .accountId = summary->accountId,
                .mailboxIds = {summary->mailboxId},
                .queryWindows = {},
                .searchWindows = {},
                .mailboxTreeChanged = false,
                .hasNewMail = false,
                .optimisticProjection = false,
                .contactsChanged = false,
            });
            if (command.offlinePolicy == DeveloperOfflineClearPolicy::Preserve &&
                m_requestOfflineResync)
            {
                m_requestOfflineResync(command.accountId.toStdString(),
                                       command.mailboxId.toStdString());
            }
        }
        co_return result;
    }
} // namespace javelin::app
