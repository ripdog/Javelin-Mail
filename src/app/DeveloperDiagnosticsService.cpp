#include "app/DeveloperDiagnosticsService.h"

#include "jmap/cache/MailVault.h"
#include "jmap/cache/MailboxRepository.h"

#include <QCoroFuture>

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QtConcurrentRun>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace javelin::app
{
    namespace
    {
        using javelin::jmap::cache::DatabaseError;
        using javelin::jmap::cache::ReadOnlyDatabaseConnection;

        [[nodiscard]] DatabaseError queryError(const QString& operation, const QSqlQuery& query)
        {
            return javelin::jmap::cache::databaseError(operation, query.lastError());
        }

        [[nodiscard]] std::optional<QString> optionalString(const QVariant& value)
        {
            if (value.isNull())
                return std::nullopt;
            return value.toString();
        }

        [[nodiscard]] std::optional<std::uint64_t> optionalUnsigned(const QVariant& value)
        {
            if (value.isNull())
                return std::nullopt;
            return value.toULongLong();
        }

        [[nodiscard]] std::uint64_t allocatedBytes(const QString& path)
        {
#ifdef Q_OS_UNIX
            struct stat details{};
            if (::stat(path.toLocal8Bit().constData(), &details) == 0)
                return static_cast<std::uint64_t>(details.st_blocks) * 512ULL;
#endif
            return static_cast<std::uint64_t>(std::max<qint64>(0, QFileInfo{path}.size()));
        }

        struct FileIdentity
        {
            std::uint64_t device = 0;
            std::uint64_t inode = 0;

            friend bool operator<(const FileIdentity& left, const FileIdentity& right)
            {
                return std::tie(left.device, left.inode) < std::tie(right.device, right.inode);
            }
        };

        [[nodiscard]] std::optional<FileIdentity> fileIdentity(const QString& path)
        {
#ifdef Q_OS_UNIX
            struct stat details{};
            if (::stat(path.toLocal8Bit().constData(), &details) == 0)
            {
                return FileIdentity{.device = static_cast<std::uint64_t>(details.st_dev),
                                    .inode = static_cast<std::uint64_t>(details.st_ino)};
            }
#endif
            return std::nullopt;
        }

        [[nodiscard]] std::variant<DeveloperMailboxUsage, DatabaseError>
        measureMailbox(const QSqlDatabase& database, const QString& vaultPath,
                       const QString& accountId, const QString& mailboxId)
        {
            DeveloperMailboxUsage usage;

            QSqlQuery sqliteEstimate{database};
            sqliteEstimate.prepare(QStringLiteral(
                "SELECT "
                "COALESCE((SELECT SUM(length(em.email_id)+length(em.mailbox_id)+48) "
                "FROM email_mailboxes em WHERE em.account_id=:account AND "
                "em.mailbox_id=:mailbox),0)+"
                "COALESCE((SELECT SUM(length(w.query_key)+160) FROM mailbox_query_windows w "
                "WHERE w.account_id=:account AND w.mailbox_id=:mailbox),0)+"
                "COALESCE((SELECT SUM(length(i.email_id)+48) FROM mailbox_query_window_items i "
                "JOIN mailbox_query_windows w ON w.account_id=i.account_id AND "
                "w.query_key=i.query_key AND w.requested_offset=i.requested_offset AND "
                "w.requested_limit=i.requested_limit WHERE w.account_id=:account AND "
                "w.mailbox_id=:mailbox),0)+"
                "COALESCE((SELECT SUM(length(o.email_id)+64) FROM offline_mailbox_membership o "
                "WHERE o.account_id=:account AND o.mailbox_id=:mailbox),0)"));
            sqliteEstimate.bindValue(QStringLiteral(":account"), accountId);
            sqliteEstimate.bindValue(QStringLiteral(":mailbox"), mailboxId);
            if (!sqliteEstimate.exec() || !sqliteEstimate.next())
                return queryError(QStringLiteral("Measure mailbox SQLite usage"), sqliteEstimate);
            usage.sqliteEstimatedBytes = sqliteEstimate.value(0).toULongLong();

            QSqlQuery objects{database};
            objects.prepare(QStringLiteral(
                "SELECT DISTINCT o.content_hash,o.relative_path,o.size,EXISTS("
                "SELECT 1 FROM mail_vault_email_refs other_ref "
                "LEFT JOIN email_mailboxes other_membership ON "
                "other_membership.account_id=other_ref.account_id AND "
                "other_membership.email_id=other_ref.email_id "
                "WHERE other_ref.content_hash=o.content_hash AND "
                "(other_membership.account_id IS NULL OR "
                "other_membership.account_id<>:account OR "
                "other_membership.mailbox_id<>:mailbox)) "
                "FROM mail_vault_email_refs r "
                "JOIN mail_vault_objects o ON o.content_hash=r.content_hash "
                "JOIN email_mailboxes em ON em.account_id=r.account_id AND em.email_id=r.email_id "
                "WHERE em.account_id=:account AND em.mailbox_id=:mailbox"));
            objects.bindValue(QStringLiteral(":account"), accountId);
            objects.bindValue(QStringLiteral(":mailbox"), mailboxId);
            if (!objects.exec())
                return queryError(QStringLiteral("Measure mailbox body usage"), objects);

            const javelin::jmap::cache::MailVault vault{vaultPath};
            std::set<FileIdentity> allocatedFiles;
            while (objects.next())
            {
                const QString contentHash = objects.value(0).toString();
                const QString relativePath = objects.value(1).toString();
                const std::uint64_t size = objects.value(2).toULongLong();
                usage.logicalBodyBytes += size;

                if (objects.value(3).toBool())
                    usage.sharedBodyBytes += size;
                else
                    usage.reclaimableBodyBytes += size;

                const QString absolutePath = QDir{vaultPath}.filePath(relativePath);
                if (!QFileInfo::exists(absolutePath))
                {
                    ++usage.missingBodyObjects;
                    continue;
                }

                const auto identity = fileIdentity(absolutePath);
                if (!identity.has_value() || allocatedFiles.insert(*identity).second)
                    usage.allocatedBodyBytes += allocatedBytes(absolutePath);

                const javelin::jmap::cache::MailVaultObject object{
                    .contentHash = contentHash.toStdString(),
                    .relativePath = relativePath,
                    .size = size,
                };
                if (vault.isLeased(object))
                    ++usage.activeBodyLeases;
            }

            return usage;
        }

        [[nodiscard]] DeveloperDiagnosticsResult loadSnapshot(QString databasePath,
                                                              QString vaultPath)
        {
            auto opened = ReadOnlyDatabaseConnection::open({
                .connectionName =
                    QStringLiteral("developer-diagnostics-%1")
                        .arg(javelin::jmap::cache::ThreadConnectionFactory::currentThreadTag()),
                .databasePath = databasePath,
                .busyTimeout = std::chrono::seconds{30},
            });
            if (const auto* error = std::get_if<DatabaseError>(&opened))
                return *error;
            auto connection = std::get<ReadOnlyDatabaseConnection>(std::move(opened));
            if (const auto error = connection.validate())
                return *error;

            DeveloperDiagnosticsSnapshot snapshot{
                .databasePath = std::move(databasePath),
                .vaultPath = std::move(vaultPath),
                .databaseDataVersion = 0,
                .mailboxes = {},
            };
            const auto dataVersion = connection.dataVersion();
            if (const auto* error = std::get_if<DatabaseError>(&dataVersion))
                return *error;
            snapshot.databaseDataVersion = std::get<std::uint64_t>(dataVersion);

            QSqlQuery mailboxes{connection.database()};
            if (!mailboxes.exec(QStringLiteral(
                    "SELECT a.account_id,CASE WHEN a.name='' THEN a.email_address ELSE a.name END,"
                    "a.email_address,m.mailbox_id,m.name,m.parent_mailbox_id,parent.name,m.role,"
                    "m.sort_order,m.total_emails,m.unread_emails,m.total_threads,m.unread_threads,"
                    "m.is_subscribed,m.state,m.rights_json,"
                    "(SELECT state_token FROM sync_state s WHERE s.account_id=m.account_id AND "
                    "s.object_type='Mailbox' AND s.query_key=''),"
                    "(SELECT state_token FROM sync_state s WHERE s.account_id=m.account_id AND "
                    "s.object_type='Email' AND s.query_key=''),"
                    "offline.desired,offline.status,offline.query_state,offline.email_state,"
                    "offline.expected_total,offline.completed_total,offline.completed_bytes,"
                    "offline.estimated_bytes,offline.generation,offline.completed_generation,"
                    "offline.anchor_email_id,offline.latest_error,offline.updated_at "
                    "FROM mailboxes m JOIN accounts a ON a.account_id=m.account_id "
                    "LEFT JOIN mailboxes parent ON parent.account_id=m.account_id AND "
                    "parent.mailbox_id=m.parent_mailbox_id "
                    "LEFT JOIN offline_mailbox_scopes offline ON offline.account_id=m.account_id "
                    "AND offline.mailbox_id=m.mailbox_id "
                    "ORDER BY a.email_address COLLATE NOCASE,m.sort_order,m.name COLLATE NOCASE")))
            {
                return queryError(QStringLiteral("Load developer mailbox records"), mailboxes);
            }

            const QString measuredAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
            while (mailboxes.next())
            {
                DeveloperMailboxRecord record{
                    .accountId = mailboxes.value(0).toString(),
                    .accountName = mailboxes.value(1).toString(),
                    .accountEmailAddress = mailboxes.value(2).toString(),
                    .mailboxId = mailboxes.value(3).toString(),
                    .mailboxName = mailboxes.value(4).toString(),
                    .parentMailboxId = optionalString(mailboxes.value(5)),
                    .parentMailboxName = optionalString(mailboxes.value(6)),
                    .role = optionalString(mailboxes.value(7)),
                    .sortOrder = mailboxes.value(8).toULongLong(),
                    .totalEmails = mailboxes.value(9).toULongLong(),
                    .unreadEmails = mailboxes.value(10).toULongLong(),
                    .totalThreads = mailboxes.value(11).toULongLong(),
                    .unreadThreads = mailboxes.value(12).toULongLong(),
                    .isSubscribed = mailboxes.value(13).toBool(),
                    .mailboxState = optionalString(mailboxes.value(14)),
                    .rawRightsJson = mailboxes.value(15).toString(),
                    .accountMailboxState = optionalString(mailboxes.value(16)),
                    .accountEmailState = optionalString(mailboxes.value(17)),
                    .queryCoverageSummary = {},
                    .queryMaterializationSummary = {},
                    .oldestCachedMessage = std::nullopt,
                    .newestCachedMessage = std::nullopt,
                    .offlineDesired = mailboxes.value(18).toBool(),
                    .offlineStatus = optionalString(mailboxes.value(19)),
                    .offlineQueryState = optionalString(mailboxes.value(20)),
                    .offlineEmailState = optionalString(mailboxes.value(21)),
                    .offlineExpectedTotal = optionalUnsigned(mailboxes.value(22)),
                    .offlineCompletedTotal = mailboxes.value(23).toULongLong(),
                    .offlineCompletedBytes = mailboxes.value(24).toULongLong(),
                    .offlineEstimatedBytes = optionalUnsigned(mailboxes.value(25)),
                    .offlineGeneration = mailboxes.value(26).toULongLong(),
                    .offlineCompletedGeneration = optionalUnsigned(mailboxes.value(27)),
                    .offlineAnchorEmailId = optionalString(mailboxes.value(28)),
                    .offlineLatestError = optionalString(mailboxes.value(29)),
                    .offlineUpdatedAt = optionalString(mailboxes.value(30)),
                    .usage = {},
                    .measuredAt = measuredAt,
                };

                const auto rights =
                    javelin::jmap::cache::deserializeMailboxRights(record.rawRightsJson);
                record.mayReadItems = rights.mayReadItems;
                record.mayAddItems = rights.mayAddItems;
                record.mayRemoveItems = rights.mayRemoveItems;
                record.maySetSeen = rights.maySetSeen;
                record.maySetKeywords = rights.maySetKeywords;
                record.mayCreateChild = rights.mayCreateChild;
                record.mayRename = rights.mayRename;
                record.mayDelete = rights.mayDelete;
                record.maySubmit = rights.maySubmit;

                QSqlQuery membership{connection.database()};
                membership.prepare(QStringLiteral(
                    "SELECT COUNT(*),MIN(e.received_at),MAX(e.received_at) "
                    "FROM email_mailboxes em LEFT JOIN emails e ON e.account_id=em.account_id AND "
                    "e.email_id=em.email_id WHERE em.account_id=:account AND "
                    "em.mailbox_id=:mailbox"));
                membership.bindValue(QStringLiteral(":account"), record.accountId);
                membership.bindValue(QStringLiteral(":mailbox"), record.mailboxId);
                if (!membership.exec() || !membership.next())
                    return queryError(QStringLiteral("Load mailbox membership diagnostics"),
                                      membership);
                record.cachedMembershipCount = membership.value(0).toULongLong();
                record.oldestCachedMessage = optionalString(membership.value(1));
                record.newestCachedMessage = optionalString(membership.value(2));

                QSqlQuery windows{connection.database()};
                windows.prepare(QStringLiteral(
                    "SELECT COUNT(*),COALESCE(SUM((SELECT COUNT(*) FROM "
                    "mailbox_query_window_items i WHERE i.account_id=w.account_id AND "
                    "i.query_key=w.query_key AND i.requested_offset=w.requested_offset AND "
                    "i.requested_limit=w.requested_limit)),0),"
                    "COALESCE(GROUP_CONCAT(DISTINCT w.coverage),''),"
                    "COALESCE(GROUP_CONCAT(DISTINCT w.materialization),'') "
                    "FROM mailbox_query_windows w WHERE w.account_id=:account AND "
                    "w.mailbox_id=:mailbox"));
                windows.bindValue(QStringLiteral(":account"), record.accountId);
                windows.bindValue(QStringLiteral(":mailbox"), record.mailboxId);
                if (!windows.exec() || !windows.next())
                    return queryError(QStringLiteral("Load mailbox window diagnostics"), windows);
                record.queryWindowCount = windows.value(0).toULongLong();
                record.queryWindowItemCount = windows.value(1).toULongLong();
                record.queryCoverageSummary = windows.value(2).toString();
                record.queryMaterializationSummary = windows.value(3).toString();

                QSqlQuery vaultCounts{connection.database()};
                vaultCounts.prepare(QStringLiteral(
                    "SELECT "
                    "(SELECT COUNT(*) FROM mail_vault_email_refs r JOIN email_mailboxes em ON "
                    "em.account_id=r.account_id AND em.email_id=r.email_id WHERE "
                    "em.account_id=:account AND em.mailbox_id=:mailbox),"
                    "(SELECT COUNT(*) FROM mail_vault_projection_jobs j WHERE "
                    "j.account_id=:account AND j.mailbox_id=:mailbox AND j.status='pending'),"
                    "(SELECT COUNT(*) FROM mail_vault_projection_jobs j WHERE "
                    "j.account_id=:account AND j.mailbox_id=:mailbox AND j.status='failed'),"
                    "(SELECT COUNT(*) FROM mail_vault_projection_jobs j WHERE "
                    "j.account_id=:account AND j.mailbox_id=:mailbox AND j.status='complete'),"
                    "(SELECT COUNT(*) FROM mutation_journal j WHERE j.account_id=:account AND "
                    "j.status IN ('pending','in_flight','unknown') AND "
                    "instr(j.payload_json,:mailbox_needle)>0)"));
                vaultCounts.bindValue(QStringLiteral(":account"), record.accountId);
                vaultCounts.bindValue(QStringLiteral(":mailbox"), record.mailboxId);
                const QString mailboxNeedle =
                    QStringLiteral("\"") + record.mailboxId + QStringLiteral("\"");
                vaultCounts.bindValue(QStringLiteral(":mailbox_needle"), mailboxNeedle);
                if (!vaultCounts.exec() || !vaultCounts.next())
                    return queryError(QStringLiteral("Load mailbox vault diagnostics"),
                                      vaultCounts);
                record.vaultReferenceCount = vaultCounts.value(0).toULongLong();
                record.pendingVaultProjectionCount = vaultCounts.value(1).toULongLong();
                record.failedVaultProjectionCount = vaultCounts.value(2).toULongLong();
                record.completeVaultProjectionCount = vaultCounts.value(3).toULongLong();
                record.activeMutationCount = vaultCounts.value(4).toULongLong();

                auto usage = measureMailbox(connection.database(), snapshot.vaultPath,
                                            record.accountId, record.mailboxId);
                if (const auto* error = std::get_if<DatabaseError>(&usage))
                    return *error;
                record.usage = std::get<DeveloperMailboxUsage>(std::move(usage));
                snapshot.mailboxes.push_back(std::move(record));
            }

            return snapshot;
        }
    } // namespace

    DeveloperDiagnosticsService::DeveloperDiagnosticsService(QString databasePath,
                                                             QString vaultPath, QObject* parent)
        : QObject(parent), m_databasePath(std::move(databasePath)),
          m_vaultPath(std::move(vaultPath))
    {
        m_scanPool.setMaxThreadCount(1);
        m_scanPool.setExpiryTimeout(-1);
        m_scanPool.setThreadPriority(QThread::LowPriority);
    }

    DeveloperDiagnosticsService::~DeveloperDiagnosticsService()
    {
        m_scanPool.waitForDone();
    }

    QCoro::Task<DeveloperDiagnosticsResult> DeveloperDiagnosticsService::snapshot()
    {
        auto future = QtConcurrent::run(&m_scanPool, loadSnapshot, m_databasePath, m_vaultPath);
        co_return co_await qCoro(future).takeResult();
    }

} // namespace javelin::app
