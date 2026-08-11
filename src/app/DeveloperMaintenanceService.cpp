#include "app/DeveloperMaintenanceService.h"

#include "app/MailboxMaintenanceRegistry.h"
#include "app/WorkScheduler.h"
#include "jmap/cache/MailVault.h"
#include "jmap/cache/RawMessageSourceRepository.h"

#include <QCoroFuture>
#include <QCoroTask>

#include <KLocalizedString>

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QTimer>
#include <QUuid>
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

        constexpr std::string_view mailboxCleanupJobPrefix = "mailbox-cache-cleanup:";

        [[nodiscard]] QString cacheKindName(const DeveloperMailboxCacheKind kind)
        {
            switch (kind)
            {
            case DeveloperMailboxCacheKind::Sqlite:
                return QStringLiteral("sqlite");
            case DeveloperMailboxCacheKind::Bodies:
                return QStringLiteral("bodies");
            case DeveloperMailboxCacheKind::SqliteAndBodies:
                return QStringLiteral("sqlite_and_bodies");
            }
            return QStringLiteral("sqlite");
        }

        [[nodiscard]] std::optional<DeveloperMailboxCacheKind> cacheKind(const QString& name)
        {
            if (name == QStringLiteral("sqlite"))
                return DeveloperMailboxCacheKind::Sqlite;
            if (name == QStringLiteral("bodies"))
                return DeveloperMailboxCacheKind::Bodies;
            if (name == QStringLiteral("sqlite_and_bodies"))
                return DeveloperMailboxCacheKind::SqliteAndBodies;
            return std::nullopt;
        }

        [[nodiscard]] QString offlinePolicyName(const DeveloperOfflineClearPolicy policy)
        {
            return policy == DeveloperOfflineClearPolicy::Disable ? QStringLiteral("disable")
                                                                  : QStringLiteral("preserve");
        }

        [[nodiscard]] std::optional<DeveloperOfflineClearPolicy> offlinePolicy(const QString& name)
        {
            if (name == QStringLiteral("preserve"))
                return DeveloperOfflineClearPolicy::Preserve;
            if (name == QStringLiteral("disable"))
                return DeveloperOfflineClearPolicy::Disable;
            return std::nullopt;
        }

        [[nodiscard]] QString cleanupCheckpoint(const DeveloperMailboxClearCommand& command)
        {
            const QJsonObject object{
                {QStringLiteral("type"), QStringLiteral("mailbox_cache_cleanup")},
                {QStringLiteral("version"), 1},
                {QStringLiteral("accountId"), command.accountId},
                {QStringLiteral("mailboxId"), command.mailboxId},
                {QStringLiteral("kind"), cacheKindName(command.kind)},
                {QStringLiteral("offlinePolicy"), offlinePolicyName(command.offlinePolicy)},
            };
            return QString::fromUtf8(QJsonDocument{object}.toJson(QJsonDocument::Compact));
        }

        [[nodiscard]] std::variant<DeveloperMailboxClearCommand, DatabaseError>
        cleanupCommand(const QString& checkpoint)
        {
            QJsonParseError parseError;
            const auto document = QJsonDocument::fromJson(checkpoint.toUtf8(), &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject())
                return maintenanceError(i18n("The mailbox cache cleanup checkpoint is invalid."));
            const auto object = document.object();
            if (object.value(QStringLiteral("type")).toString() !=
                    QStringLiteral("mailbox_cache_cleanup") ||
                object.value(QStringLiteral("version")).toInt() != 1)
                return maintenanceError(
                    i18n("The mailbox cache cleanup checkpoint is unsupported."));

            const QString accountId = object.value(QStringLiteral("accountId")).toString();
            const QString mailboxId = object.value(QStringLiteral("mailboxId")).toString();
            const auto kind = cacheKind(object.value(QStringLiteral("kind")).toString());
            const auto policy =
                offlinePolicy(object.value(QStringLiteral("offlinePolicy")).toString());
            if (accountId.isEmpty() || mailboxId.isEmpty() || !kind.has_value() ||
                !policy.has_value())
                return maintenanceError(
                    i18n("The mailbox cache cleanup checkpoint is incomplete."));

            return DeveloperMailboxClearCommand{
                .accountId = accountId,
                .mailboxId = mailboxId,
                .kind = *kind,
                .offlinePolicy = *policy,
            };
        }

        [[nodiscard]] bool isMailboxCleanupJob(const WorkRecord& job)
        {
            return job.kind == WorkKind::Maintenance &&
                   job.jobId.starts_with(mailboxCleanupJobPrefix);
        }

        [[nodiscard]] QString cleanupTitle(const DeveloperMailboxClearCommand& command)
        {
            if (command.kind == DeveloperMailboxCacheKind::Bodies)
                return i18n("Clear cached message bodies for %1", command.mailboxId);
            return i18n("Clear cached mailbox data for %1", command.mailboxId);
        }

        [[nodiscard]] QString cleanupRunningDetail(const DeveloperMailboxClearCommand& command)
        {
            if (command.kind == DeveloperMailboxCacheKind::Bodies)
                return i18n("Removing cached message bodies from %1", command.mailboxId);
            if (command.kind == DeveloperMailboxCacheKind::SqliteAndBodies)
                return i18n("Removing cached message bodies and mailbox data from %1",
                            command.mailboxId);
            return i18n("Removing cached mailbox data from %1", command.mailboxId);
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

        [[nodiscard]] std::optional<DatabaseError> executeAccount(QSqlDatabase& database,
                                                                  const QString& operation,
                                                                  const QString& statement,
                                                                  const QString& accountId)
        {
            QSqlQuery query{database};
            query.prepare(statement);
            query.bindValue(QStringLiteral(":account"), accountId);
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

        [[nodiscard]] DeveloperMailboxClearExecutionResult
        clearSqlite(DatabaseConnection& connection, const DeveloperMailboxClearCommand& command,
                    const std::uint64_t maintenanceGeneration, const bool resetOffline = true)
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

            QSqlQuery createThreadTargets{database};
            if (!createThreadTargets.exec(
                    QStringLiteral("CREATE TEMP TABLE mailbox_cache_clear_threads("
                                   "thread_id TEXT PRIMARY KEY) WITHOUT ROWID")))
            {
                return queryError(QStringLiteral("Create mailbox Thread cleanup targets"),
                                  createThreadTargets);
            }

            QSqlQuery targetThreads{database};
            targetThreads.prepare(QStringLiteral(
                "INSERT INTO mailbox_cache_clear_threads(thread_id) "
                "WITH candidate_emails(email_id) AS ("
                "SELECT em.email_id FROM email_mailboxes em WHERE em.account_id=:account AND "
                "em.mailbox_id=:mailbox UNION SELECT i.email_id FROM "
                "mailbox_query_window_items i JOIN mailbox_query_windows w ON "
                "w.account_id=i.account_id AND w.query_key=i.query_key AND "
                "w.requested_offset=i.requested_offset AND "
                "w.requested_limit=i.requested_limit WHERE w.account_id=:account AND "
                "w.mailbox_id=:mailbox UNION SELECT offline.email_id FROM "
                "offline_mailbox_membership offline WHERE offline.account_id=:account AND "
                "offline.mailbox_id=:mailbox) "
                "SELECT DISTINCT e.thread_id FROM candidate_emails candidate JOIN emails e ON "
                "e.account_id=:account AND e.email_id=candidate.email_id WHERE "
                "e.thread_id IS NOT NULL AND NOT EXISTS(SELECT 1 FROM emails active_email JOIN "
                "mutation_journal mutation ON mutation.account_id=active_email.account_id AND "
                "mutation.data_type='Email' AND mutation.object_id=active_email.email_id AND "
                "mutation.status IN ('pending','in_flight','accepted','unknown') WHERE "
                "active_email.account_id=e.account_id AND active_email.thread_id=e.thread_id)"));
            targetThreads.bindValue(QStringLiteral(":account"), command.accountId);
            targetThreads.bindValue(QStringLiteral(":mailbox"), command.mailboxId);
            if (!targetThreads.exec())
                return queryError(QStringLiteral("Resolve mailbox Thread cleanup targets"),
                                  targetThreads);

            QSqlQuery threadRows{database};
            threadRows.prepare(QStringLiteral(
                "SELECT COUNT(*) FROM threads t JOIN mailbox_cache_clear_threads target ON "
                "target.thread_id=t.thread_id WHERE t.account_id=:account"));
            threadRows.bindValue(QStringLiteral(":account"), command.accountId);
            if (!threadRows.exec() || !threadRows.next())
            {
                return queryError(QStringLiteral("Measure mailbox Thread cache rows"), threadRows);
            }
            const std::uint64_t cachedThreads = threadRows.value(0).toULongLong();

            QSqlQuery threadMemberRows{database};
            threadMemberRows.prepare(QStringLiteral(
                "SELECT COUNT(*) FROM thread_email_members member JOIN "
                "mailbox_cache_clear_threads target ON target.thread_id=member.thread_id WHERE "
                "member.account_id=:account"));
            threadMemberRows.bindValue(QStringLiteral(":account"), command.accountId);
            if (!threadMemberRows.exec() || !threadMemberRows.next())
            {
                return queryError(QStringLiteral("Measure mailbox Thread member cache rows"),
                                  threadMemberRows);
            }
            const std::uint64_t cachedThreadMembers = threadMemberRows.value(0).toULongLong();

            QStringList invalidatedMailboxIds{command.mailboxId};
            QSqlQuery affectedMailboxes{database};
            affectedMailboxes.prepare(QStringLiteral(
                "SELECT DISTINCT membership.mailbox_id FROM email_mailboxes membership JOIN "
                "emails e ON e.account_id=membership.account_id AND "
                "e.email_id=membership.email_id JOIN mailbox_cache_clear_threads target ON "
                "target.thread_id=e.thread_id WHERE membership.account_id=:account ORDER BY "
                "membership.mailbox_id"));
            affectedMailboxes.bindValue(QStringLiteral(":account"), command.accountId);
            if (!affectedMailboxes.exec())
            {
                return queryError(QStringLiteral("Resolve mailbox Thread invalidations"),
                                  affectedMailboxes);
            }
            while (affectedMailboxes.next())
            {
                const QString mailboxId = affectedMailboxes.value(0).toString();
                if (!invalidatedMailboxIds.contains(mailboxId))
                    invalidatedMailboxIds.push_back(mailboxId);
            }

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
            if (const auto error = executeAccount(
                    database, QStringLiteral("Delete mailbox Thread membership cache"),
                    QStringLiteral("DELETE FROM thread_email_members AS member WHERE "
                                   "member.account_id=:account AND EXISTS(SELECT 1 FROM "
                                   "mailbox_cache_clear_threads target WHERE "
                                   "target.thread_id=member.thread_id)"),
                    command.accountId))
                return *error;
            if (const auto error = executeAccount(
                    database, QStringLiteral("Invalidate mailbox Thread cache"),
                    QStringLiteral("UPDATE threads AS t SET membership_freshness='stale',"
                                   "member_count=0,state=NULL WHERE t.account_id=:account AND "
                                   "EXISTS(SELECT 1 FROM mailbox_cache_clear_threads target WHERE "
                                   "target.thread_id=t.thread_id)"),
                    command.accountId))
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
            if (resetOffline)
            {
                if (const auto error = resetOfflineScope(database, command))
                    return *error;
            }
            const auto generation = advanceEmailGeneration(database, command.accountId);
            if (const auto* error = std::get_if<DatabaseError>(&generation))
                return *error;
            if (const auto error = transaction.commit())
                return *error;

            return DeveloperMailboxClearSummary{
                .accountId = command.accountId,
                .mailboxId = command.mailboxId,
                .invalidatedMailboxIds = std::move(invalidatedMailboxIds),
                .kind = DeveloperMailboxCacheKind::Sqlite,
                .maintenanceGeneration = maintenanceGeneration,
                .rowsDiscarded = std::get<std::uint64_t>(windowItems) +
                                 std::get<std::uint64_t>(windows) +
                                 std::get<std::uint64_t>(memberships) +
                                 std::get<std::uint64_t>(offlineMemberships) + cachedThreads +
                                 cachedThreadMembers,
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
                "SELECT DISTINCT o.content_hash,o.relative_path,o.size FROM mail_vault_objects o "
                "WHERE EXISTS(SELECT 1 FROM mail_vault_mailbox_refs mr JOIN "
                "mail_vault_email_refs r ON r.account_id=mr.account_id AND "
                "r.email_id=mr.email_id WHERE r.content_hash=o.content_hash AND "
                "mr.account_id=:account AND mr.mailbox_id=:mailbox) OR EXISTS(SELECT 1 FROM "
                "mail_vault_projection_jobs j WHERE j.content_hash=o.content_hash AND "
                "j.account_id=:account AND j.mailbox_id=:mailbox AND j.operation='unlink') ORDER "
                "BY "
                "o.content_hash"));
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
                if (const auto error = sources.replayProjectionJobsForMailbox(
                        command.accountId.toStdString(), command.mailboxId.toStdString(), 250))
                    return error;
            }
        }

        [[nodiscard]] DeveloperMailboxClearExecutionResult
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
            constexpr std::size_t cleanupBatchSize = 128;
            std::uint64_t logicalBytes = 0;
            std::uint64_t reclaimedBytes = 0;
            std::uint64_t deferredBytes = 0;
            std::vector<BodyObject> evictedObjects;
            evictedObjects.reserve(cleanupBatchSize);
            const auto commitEvictedObjects = [&]() -> std::optional<DatabaseError>
            {
                if (evictedObjects.empty())
                    return std::nullopt;
                const DatabaseWriteScope cleanupWriteScope{connection};
                auto cleanupResult = DatabaseTransaction::begin(
                    connection, QStringLiteral("Remove cleared mail vault objects"));
                if (const auto* error = std::get_if<DatabaseError>(&cleanupResult))
                    return *error;
                auto cleanup = std::get<DatabaseTransaction>(std::move(cleanupResult));
                QSqlQuery jobs{database};
                jobs.prepare(QStringLiteral(
                    "DELETE FROM mail_vault_projection_jobs WHERE content_hash=:hash"));
                QSqlQuery objectRow{database};
                objectRow.prepare(
                    QStringLiteral("DELETE FROM mail_vault_objects WHERE content_hash=:hash"));
                for (const auto& object : evictedObjects)
                {
                    jobs.bindValue(QStringLiteral(":hash"), object.contentHash);
                    if (!jobs.exec())
                    {
                        return queryError(QStringLiteral("Delete cleared body projection history"),
                                          jobs);
                    }
                    jobs.finish();
                    objectRow.bindValue(QStringLiteral(":hash"), object.contentHash);
                    if (!objectRow.exec())
                    {
                        return queryError(QStringLiteral("Delete cleared mail vault object"),
                                          objectRow);
                    }
                    objectRow.finish();
                }
                if (const auto error = cleanup.commit())
                    return error;
                evictedObjects.clear();
                return std::nullopt;
            };

            QSqlQuery refs{database};
            refs.prepare(QStringLiteral(
                "SELECT COUNT(*) FROM mail_vault_email_refs WHERE content_hash=:hash"));
            for (const auto& object : objects)
            {
                logicalBytes += object.size;
                refs.bindValue(QStringLiteral(":hash"), object.contentHash);
                if (!refs.exec() || !refs.next())
                    return queryError(QStringLiteral("Inspect cleared body references"), refs);
                const bool isUnreferenced = refs.value(0).toULongLong() == 0;
                refs.finish();
                if (!isUnreferenced)
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
                    return maintenanceError(error->message);
                evictedObjects.push_back(object);
                if (existed)
                    reclaimedBytes += object.size;
                if (evictedObjects.size() >= cleanupBatchSize)
                {
                    if (const auto error = commitEvictedObjects())
                        return *error;
                }
            }
            if (const auto error = commitEvictedObjects())
                return *error;

            return DeveloperMailboxClearSummary{
                .accountId = command.accountId,
                .mailboxId = command.mailboxId,
                .invalidatedMailboxIds = {command.mailboxId},
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

        [[nodiscard]] DeveloperMailboxClearExecutionResult
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
            if (command.kind == DeveloperMailboxCacheKind::Bodies)
                return clearBodies(connection, vaultPath, command, maintenanceGeneration);

            auto bodies = clearBodies(connection, vaultPath, command, maintenanceGeneration);
            if (const auto* error = std::get_if<DatabaseError>(&bodies))
                return *error;
            auto sqlite = clearSqlite(connection, command, maintenanceGeneration, false);
            if (const auto* error = std::get_if<DatabaseError>(&sqlite))
                return *error;

            const auto& bodySummary = std::get<DeveloperMailboxClearSummary>(bodies);
            const auto& sqliteSummary = std::get<DeveloperMailboxClearSummary>(sqlite);
            return DeveloperMailboxClearSummary{
                .accountId = command.accountId,
                .mailboxId = command.mailboxId,
                .invalidatedMailboxIds = sqliteSummary.invalidatedMailboxIds,
                .kind = DeveloperMailboxCacheKind::SqliteAndBodies,
                .maintenanceGeneration = maintenanceGeneration,
                .rowsDiscarded = bodySummary.rowsDiscarded + sqliteSummary.rowsDiscarded,
                .projectionsRemoved = bodySummary.projectionsRemoved,
                .logicalBytesReleased = bodySummary.logicalBytesReleased,
                .reclaimedBytes = bodySummary.reclaimedBytes,
                .deferredBytes = bodySummary.deferredBytes,
                .offlineStorageDisabled = bodySummary.offlineStorageDisabled,
            };
        }
    } // namespace

    DeveloperMaintenanceService::DeveloperMaintenanceService(
        QString databasePath, QString vaultPath, MailboxMaintenanceRegistry& registry,
        MailCacheChangePublisher& cacheChangePublisher, WorkScheduler& workScheduler,
        std::function<void(std::string_view, std::string_view)> requestOfflineResync,
        QObject* parent)
        : QObject(parent), m_databasePath(std::move(databasePath)),
          m_vaultPath(std::move(vaultPath)), m_registry(registry),
          m_cacheChangePublisher(cacheChangePublisher), m_workScheduler(workScheduler),
          m_requestOfflineResync(std::move(requestOfflineResync))
    {
        m_workerPool.setMaxThreadCount(1);
        m_workerPool.setExpiryTimeout(-1);
        m_workerPool.setThreadPriority(QThread::LowPriority);
        connect(&m_workScheduler, &WorkScheduler::jobsChanged, this, [this]() { schedulePump(); });
        connect(&m_workScheduler, &WorkScheduler::foregroundAvailabilityChanged, this,
                [this]() { schedulePump(); });
        schedulePump();
    }

    DeveloperMaintenanceService::~DeveloperMaintenanceService()
    {
        m_workerPool.waitForDone();
    }

    QCoro::Task<DeveloperMailboxClearResult>
    DeveloperMaintenanceService::clearMailboxCache(DeveloperMailboxClearCommand command)
    {
        if (command.accountId.isEmpty() || command.mailboxId.isEmpty())
            co_return maintenanceError(i18n("The mailbox cache cleanup target is incomplete."));

        const std::string jobId = QStringLiteral("mailbox-cache-cleanup:%1")
                                      .arg(QUuid::createUuid().toString(QUuid::WithoutBraces))
                                      .toStdString();
        if (const auto error = m_workScheduler.ensure({
                .jobId = jobId,
                .parentJobId = std::nullopt,
                .accountId = command.accountId.toStdString(),
                .kind = WorkKind::Maintenance,
                .priority = WorkPriority::Bulk,
                .title = cleanupTitle(command),
                .checkpointJson = cleanupCheckpoint(command),
            }))
            co_return *error;

        schedulePump();
        co_return DeveloperMailboxClearQueued{.jobId = QString::fromStdString(jobId)};
    }

    void DeveloperMaintenanceService::schedulePump()
    {
        if (m_pumpScheduled)
            return;
        m_pumpScheduled = true;
        QTimer::singleShot(0, this,
                           [this]
                           {
                               m_pumpScheduled = false;
                               pump();
                           });
    }

    void DeveloperMaintenanceService::pump()
    {
        if (m_running || !m_workScheduler.mayStartBackgroundNetwork())
            return;

        const auto listed = m_workScheduler.list();
        const auto* jobs = std::get_if<std::vector<WorkRecord>>(&listed);
        if (jobs == nullptr)
            return;

        for (const auto& job : *jobs)
        {
            if (!isMailboxCleanupJob(job) || job.status != WorkStatus::Queued)
                continue;

            const auto decoded = cleanupCommand(job.checkpointJson);
            const auto* command = std::get_if<DeveloperMailboxClearCommand>(&decoded);
            if (command == nullptr)
            {
                const auto& error = std::get<DatabaseError>(decoded);
                static_cast<void>(m_workScheduler.update(
                    job.jobId, WorkStatus::Failed,
                    WorkProgress{.completedUnits = 0,
                                 .totalUnits = std::nullopt,
                                 .completedBytes = 0,
                                 .totalBytes = std::nullopt,
                                 .detail = i18n("Could not resume mailbox cache cleanup")},
                    job.checkpointJson, error.message));
                continue;
            }

            if (!m_workScheduler.admit(job.jobId).has_value())
                continue;

            m_running = true;
            auto task = runJob(job.jobId, *command);
            QCoro::connect(std::move(task), this,
                           [this, jobId = job.jobId]
                           {
                               m_workScheduler.release(jobId);
                               m_running = false;
                               schedulePump();
                           });
            return;
        }
    }

    QCoro::Task<void> DeveloperMaintenanceService::runJob(std::string jobId,
                                                          DeveloperMailboxClearCommand command)
    {
        const QString checkpoint = cleanupCheckpoint(command);
        WorkProgress progress{
            .completedUnits = 0,
            .totalUnits = 1,
            .completedBytes = 0,
            .totalBytes = std::nullopt,
            .detail = cleanupRunningDetail(command),
        };

        auto lease = m_registry.tryBegin(command.accountId, command.mailboxId);
        if (!lease.has_value())
        {
            const auto error =
                maintenanceError(i18n("Mailbox cache maintenance is already active."),
                                 DatabaseErrorCode::TransientContention);
            static_cast<void>(m_workScheduler.update(jobId, WorkStatus::Failed, progress,
                                                     checkpoint, error.message));
            co_return;
        }

        if (const auto error =
                m_workScheduler.update(jobId, WorkStatus::Running, progress, checkpoint))
            co_return;

        const std::uint64_t generation = lease->generation();
        auto future = QtConcurrent::run(&m_workerPool, clearMailbox, m_databasePath, m_vaultPath,
                                        command, generation);
        auto result = co_await qCoro(future).takeResult();
        if (const auto* error = std::get_if<DatabaseError>(&result))
        {
            static_cast<void>(m_workScheduler.update(jobId, WorkStatus::Failed, progress,
                                                     checkpoint, error->message));
            co_return;
        }

        const auto& summary = std::get<DeveloperMailboxClearSummary>(result);
        m_cacheChangePublisher.publishCacheChange(MailCacheChange{
            .accountId = summary.accountId,
            .mailboxIds = summary.invalidatedMailboxIds,
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

        progress.completedUnits = 1;
        progress.completedBytes = summary.reclaimedBytes;
        if (summary.deferredBytes > 0)
        {
            progress.detail =
                i18n("Reclaimed %1; %2 remains in use",
                     QLocale{}.formattedDataSize(static_cast<qint64>(summary.reclaimedBytes)),
                     QLocale{}.formattedDataSize(static_cast<qint64>(summary.deferredBytes)));
        }
        else if (summary.reclaimedBytes > 0)
        {
            progress.detail =
                i18n("Reclaimed %1",
                     QLocale{}.formattedDataSize(static_cast<qint64>(summary.reclaimedBytes)));
        }
        else
        {
            progress.detail = i18n("Cache cleanup complete");
        }
        static_cast<void>(
            m_workScheduler.update(jobId, WorkStatus::Complete, progress, checkpoint));
    }
} // namespace javelin::app
