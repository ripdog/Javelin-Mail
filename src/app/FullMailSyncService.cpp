#include "app/FullMailSyncService.h"

#include "app/MailIndexService.h"
#include "app/WorkScheduler.h"
#include "jmap/JmapCore.h"
#include "jmap/cache/Database.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailVault.h"
#include "jmap/cache/MailboxWindowRepository.h"
#include "jmap/cache/QueryService.h"
#include "jmap/cache/RawMessageSourceRepository.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/sync/MailboxQueryDescriptor.h"
#include "jmap/sync/MailboxRefreshExecutor.h"

#include <QCoroFuture>
#include <QCoroTask>
#include <QCoroTimer>

#include <KLocalizedString>

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QPointer>
#include <QSqlError>
#include <QSqlQuery>
#include <QStorageInfo>
#include <QTimer>
#include <QtConcurrentRun>

#include <algorithm>
#include <chrono>

namespace javelin::app
{
    Q_LOGGING_CATEGORY(logFullMailSync, "jmap.sync.full-mailbox")

    namespace
    {
        constexpr std::size_t canonicalWindowSize = 100;
        constexpr std::size_t maximumFullMailboxPageSize = 500;
        constexpr std::size_t offlineBodyBatchSize = 256;

        [[nodiscard]] std::string jobId(const std::string_view accountId,
                                        const std::string_view mailboxId)
        {
            const QByteArray key = QByteArray::fromStdString(std::string{accountId}) + '\0' +
                                   QByteArray::fromStdString(std::string{mailboxId});
            return "full-mailbox-" +
                   QCryptographicHash::hash(key, QCryptographicHash::Sha256).toHex().toStdString();
        }

        [[nodiscard]] javelin::jmap::LiveConnectionSettings
        liveSettings(const AccountConnectionSettings& settings)
        {
            return {.sessionUrl = settings.sessionUrl,
                    .loginEmail = settings.loginEmail,
                    .apiKey = settings.apiKey};
        }

        [[nodiscard]] QString checkpoint(const QString& phase, const std::uint64_t position,
                                         const std::uint64_t generation)
        {
            return QString::fromUtf8(QJsonDocument{
                QJsonObject{
                    {QStringLiteral("phase"), phase},
                    {QStringLiteral("position"), static_cast<qint64>(position)},
                    {QStringLiteral("generation"), static_cast<qint64>(generation)},
                }}.toJson(QJsonDocument::Compact));
        }

        void logDatabaseFailure(const QString& operation, const QSqlQuery& query)
        {
            qWarning().noquote() << operation << query.lastError().text();
        }

        struct FullMailboxPageCommit
        {
            FullMailboxPageCommit() = default;
            explicit FullMailboxPageCommit(QString errorValue) : error(std::move(errorValue))
            {
            }
            explicit FullMailboxPageCommit(const std::size_t count) : representativeCount(count)
            {
            }

            QString error;
            bool restartRequired = false;
            std::vector<std::size_t> windowOffsets;
            std::size_t representativeCount = 0;
        };

        [[nodiscard]] FullMailboxPageCommit commitFullMailboxPage(
            const QString& databasePath, const std::string& accountId, const std::string& mailboxId,
            const std::string& syncJobId, const std::uint64_t generation,
            const std::size_t position, std::vector<std::string> emailIds,
            std::vector<javelin::jmap::domain::Email> emails, std::string queryState,
            std::string emailState, const std::optional<std::size_t> total)
        {
            javelin::jmap::cache::ThreadConnectionFactory factory({
                .connectionNamePrefix = QStringLiteral("full-mail-page"),
                .databasePath = databasePath,
            });
            auto opened = factory.openForCurrentThread(accountId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&opened))
                return FullMailboxPageCommit{error->message};
            auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
            auto& database = connection.database();

            QSqlQuery baseline{database};
            baseline.prepare(QStringLiteral(
                "SELECT query_state FROM offline_mailbox_scopes WHERE "
                "account_id=:account AND mailbox_id=:mailbox AND generation=:generation"));
            baseline.bindValue(QStringLiteral(":account"), QString::fromStdString(accountId));
            baseline.bindValue(QStringLiteral(":mailbox"), QString::fromStdString(mailboxId));
            baseline.bindValue(QStringLiteral(":generation"), static_cast<qulonglong>(generation));
            if (!baseline.exec() || !baseline.next())
                return FullMailboxPageCommit{baseline.lastError().text().isEmpty()
                                                 ? i18n("Offline mailbox generation disappeared.")
                                                 : baseline.lastError().text()};
            const auto storedQueryState = baseline.value(0).toString().toStdString();
            baseline.finish();
            if (!storedQueryState.empty() && storedQueryState != queryState)
            {
                FullMailboxPageCommit result;
                result.restartRequired = true;
                return result;
            }

            QSqlQuery existingPreview{connection.database()};
            existingPreview.prepare(QStringLiteral(
                "SELECT preview FROM emails WHERE account_id=:account AND email_id=:email"));
            for (auto& email : emails)
            {
                if (email.preview.has_value())
                    continue;
                existingPreview.bindValue(QStringLiteral(":account"),
                                          QString::fromStdString(accountId));
                existingPreview.bindValue(QStringLiteral(":email"),
                                          QString::fromStdString(email.id));
                if (existingPreview.exec() && existingPreview.next() &&
                    !existingPreview.value(0).toString().isEmpty())
                {
                    email.preview = existingPreview.value(0).toString().toStdString();
                }
                existingPreview.finish();
            }

            javelin::jmap::cache::EmailRepository emailsRepository{connection};
            if (const auto error = emailsRepository.upsertMany(accountId, emails))
                return FullMailboxPageCommit{error->message};
            if (const auto error = javelin::jmap::sync::rebaseActiveEmailProjections(
                    connection, accountId, emailIds, emailState))
                return FullMailboxPageCommit{error->message};

            const auto queryKey = javelin::jmap::sync::mailboxQueryKey({
                .mailboxId = mailboxId,
                .sortProperty = "receivedAt",
                .isAscending = false,
                .collapseThreads = true,
            });

            {
                auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
                    connection, QStringLiteral("Commit full mailbox page"));
                if (const auto* error =
                        std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
                    return FullMailboxPageCommit{error->message};
                auto transaction = std::get<javelin::jmap::cache::DatabaseTransaction>(
                    std::move(transactionResult));
                QSqlQuery insert{database};
                insert.prepare(QStringLiteral(
                    "INSERT INTO offline_mailbox_membership(account_id,mailbox_id,email_id,"
                    "generation,position) VALUES(:account,:mailbox,:email,:generation,:position) "
                    "ON CONFLICT(account_id,mailbox_id,generation,email_id) DO UPDATE SET "
                    "position=excluded.position"));
                std::size_t pagePosition = position;
                for (const auto& emailId : emailIds)
                {
                    insert.bindValue(QStringLiteral(":account"), QString::fromStdString(accountId));
                    insert.bindValue(QStringLiteral(":mailbox"), QString::fromStdString(mailboxId));
                    insert.bindValue(QStringLiteral(":email"), QString::fromStdString(emailId));
                    insert.bindValue(QStringLiteral(":generation"),
                                     static_cast<qulonglong>(generation));
                    insert.bindValue(QStringLiteral(":position"),
                                     static_cast<qulonglong>(pagePosition++));
                    if (!insert.exec())
                    {
                        const QString error = insert.lastError().text();
                        transaction.rollback();
                        return FullMailboxPageCommit{error};
                    }
                }
                const auto completed = position + emailIds.size();
                QSqlQuery saveProgress{database};
                saveProgress.prepare(QStringLiteral(
                    "UPDATE offline_mailbox_scopes SET anchor_email_id=COALESCE(anchor_email_id,"
                    ":anchor),query_state=CASE WHEN query_state IS NULL OR query_state='' THEN "
                    ":query_state ELSE query_state END,email_state=CASE WHEN email_state IS NULL "
                    "OR email_state='' THEN :email_state ELSE email_state END,expected_total="
                    ":total,completed_total=MAX(completed_total,:completed),updated_at="
                    "CURRENT_TIMESTAMP WHERE account_id=:account AND mailbox_id=:mailbox AND "
                    "generation=:generation"));
                saveProgress.bindValue(QStringLiteral(":anchor"),
                                       position == 0 && !emailIds.empty()
                                           ? QVariant{QString::fromStdString(emailIds.front())}
                                           : QVariant{});
                saveProgress.bindValue(QStringLiteral(":query_state"),
                                       QString::fromStdString(queryState));
                saveProgress.bindValue(QStringLiteral(":email_state"),
                                       QString::fromStdString(emailState));
                saveProgress.bindValue(QStringLiteral(":total"),
                                       total.has_value() ? QVariant{static_cast<qulonglong>(*total)}
                                                         : QVariant{});
                saveProgress.bindValue(QStringLiteral(":completed"),
                                       static_cast<qulonglong>(completed));
                saveProgress.bindValue(QStringLiteral(":account"),
                                       QString::fromStdString(accountId));
                saveProgress.bindValue(QStringLiteral(":mailbox"),
                                       QString::fromStdString(mailboxId));
                saveProgress.bindValue(QStringLiteral(":generation"),
                                       static_cast<qulonglong>(generation));
                if (!saveProgress.exec())
                {
                    const QString error = saveProgress.lastError().text();
                    transaction.rollback();
                    return FullMailboxPageCommit{error};
                }
                QSqlQuery saveJob{database};
                saveJob.prepare(QStringLiteral(
                    "UPDATE background_jobs SET completed_units=:completed,total_units=:total,"
                    "detail='Reading mailbox contents',checkpoint_json=:checkpoint,"
                    "updated_at=CURRENT_TIMESTAMP WHERE job_id=:job"));
                saveJob.bindValue(QStringLiteral(":completed"), static_cast<qulonglong>(completed));
                saveJob.bindValue(QStringLiteral(":total"),
                                  total.has_value() ? QVariant{static_cast<qulonglong>(*total)}
                                                    : QVariant{});
                saveJob.bindValue(QStringLiteral(":checkpoint"),
                                  checkpoint(QStringLiteral("enumerating"), completed, generation));
                saveJob.bindValue(QStringLiteral(":job"), QString::fromStdString(syncJobId));
                if (!saveJob.exec())
                {
                    const QString error = saveJob.lastError().text();
                    transaction.rollback();
                    return FullMailboxPageCommit{error};
                }
                if (const auto error = transaction.commit())
                    return FullMailboxPageCommit{error->message};
            }

            javelin::jmap::cache::QueryService queries{connection};
            const auto coverageResult = queries.offlineMailboxCoverage(accountId, mailboxId);
            const auto* coverage =
                std::get_if<std::optional<javelin::jmap::cache::OfflineMailboxCoverage>>(
                    &coverageResult);
            if (coverage == nullptr)
                return FullMailboxPageCommit{
                    std::get<javelin::jmap::cache::DatabaseError>(coverageResult).message};
            if (!coverage->has_value() || (*coverage)->representativeCount == 0)
                return {};

            QSqlQuery canonicalState{database};
            canonicalState.prepare(
                QStringLiteral("SELECT state_token FROM sync_state WHERE account_id=:account AND "
                               "object_type='EmailQuery' AND query_key=:query_key"));
            canonicalState.bindValue(QStringLiteral(":account"), QString::fromStdString(accountId));
            canonicalState.bindValue(QStringLiteral(":query_key"),
                                     QString::fromStdString(queryKey));
            if (!canonicalState.exec())
                return FullMailboxPageCommit{canonicalState.lastError().text()};
            if (!canonicalState.next() || canonicalState.value(0).toString().isEmpty())
            {
                canonicalState.finish();
                return FullMailboxPageCommit{(*coverage)->representativeCount};
            }
            const std::string progressiveState = canonicalState.value(0).toString().toStdString();
            canonicalState.finish();
            std::size_t firstOffset = 0;
            QSqlQuery lastWindow{database};
            lastWindow.prepare(QStringLiteral(
                "SELECT MAX(requested_offset) FROM mailbox_query_windows WHERE "
                "account_id=:account AND mailbox_id=:mailbox AND query_key=:query_key AND "
                "query_state=:query_state"));
            lastWindow.bindValue(QStringLiteral(":account"), QString::fromStdString(accountId));
            lastWindow.bindValue(QStringLiteral(":mailbox"), QString::fromStdString(mailboxId));
            lastWindow.bindValue(QStringLiteral(":query_key"), QString::fromStdString(queryKey));
            lastWindow.bindValue(QStringLiteral(":query_state"),
                                 QString::fromStdString(progressiveState));
            if (!lastWindow.exec())
                return FullMailboxPageCommit{lastWindow.lastError().text()};
            if (lastWindow.next() && !lastWindow.value(0).isNull())
                firstOffset = lastWindow.value(0).toULongLong();
            lastWindow.finish();
            const auto lastOffset =
                ((*coverage)->representativeCount - 1) / canonicalWindowSize * canonicalWindowSize;
            const auto itemsResult = queries.listOfflineMailboxRepresentativeIds(
                accountId, mailboxId, generation, (*coverage)->representativeCount - firstOffset,
                firstOffset);
            const auto* items = std::get_if<std::vector<std::string>>(&itemsResult);
            if (items == nullptr)
                return FullMailboxPageCommit{
                    std::get<javelin::jmap::cache::DatabaseError>(itemsResult).message};

            javelin::jmap::cache::MailboxWindowRepository windows{connection};
            FullMailboxPageCommit result{(*coverage)->representativeCount};
            for (std::size_t offset = firstOffset; offset <= lastOffset;
                 offset += canonicalWindowSize)
            {
                const auto itemOffset = offset - firstOffset;
                const auto count = itemOffset < items->size()
                                       ? std::min(canonicalWindowSize, items->size() - itemOffset)
                                       : 0;
                std::vector<std::string> representativeIds;
                representativeIds.reserve(count);
                for (std::size_t index = 0; index < count; ++index)
                    representativeIds.push_back((*items)[itemOffset + index]);
                if (const auto error = windows.replace({
                        .accountId = accountId,
                        .mailboxId = mailboxId,
                        .queryKey = queryKey,
                        .requestedOffset = offset,
                        .requestedLimit = canonicalWindowSize,
                        .position = offset,
                        .returnedLimit = canonicalWindowSize,
                        .total = std::nullopt,
                        .queryState = progressiveState,
                        .coverage = javelin::jmap::cache::QueryWindowCoverage::Server,
                        .emailIds = std::move(representativeIds),
                    }))
                    return FullMailboxPageCommit{error->message};
                result.windowOffsets.push_back(offset);
            }
            return result;
        }

        [[nodiscard]] std::size_t
        fullMailboxPageSize(javelin::jmap::cache::DatabaseConnection& connection,
                            const std::string_view accountId)
        {
            javelin::jmap::cache::SessionRepository sessions{connection};
            const auto loaded = sessions.load(accountId);
            const auto* session = std::get_if<std::optional<javelin::jmap::api::Session>>(&loaded);
            if (session == nullptr || !session->has_value())
                return canonicalWindowSize;
            const auto limits = javelin::jmap::api::coreRequestLimits(**session);
            if (!limits.has_value())
                return canonicalWindowSize;
            return std::clamp<std::size_t>(static_cast<std::size_t>(limits->maxObjectsInGet), 1,
                                           maximumFullMailboxPageSize);
        }

        struct OfflineBodyWork
        {
            QString error;
            std::uint64_t totalUnits = 0;
            std::uint64_t totalBytes = 0;
            std::uint64_t missingUnits = 0;
            std::uint64_t missingBytes = 0;
            std::vector<std::pair<std::string, std::uint64_t>> downloads;
        };

        [[nodiscard]] QString
        reconcileOfflineMailboxMembership(javelin::jmap::cache::DatabaseConnection& connection,
                                          const std::string_view accountId,
                                          const std::string_view mailboxId)
        {
            auto& database = connection.database();
            QSqlQuery addMembership{database};
            addMembership.prepare(QStringLiteral(
                "INSERT INTO offline_mailbox_membership(account_id,mailbox_id,email_id,generation,"
                "position) SELECT s.account_id,s.mailbox_id,em.email_id,s.generation,"
                "COALESCE((SELECT MAX(existing.position)+1 FROM offline_mailbox_membership "
                "existing WHERE existing.account_id=s.account_id AND "
                "existing.mailbox_id=s.mailbox_id AND existing.generation=s.generation),0)+"
                "ROW_NUMBER() OVER (ORDER BY em.email_id)-1 FROM offline_mailbox_scopes s INNER "
                "JOIN email_mailboxes em ON em.account_id=s.account_id AND "
                "em.mailbox_id=s.mailbox_id WHERE s.account_id=:account AND "
                "s.mailbox_id=:mailbox AND s.desired=1 AND s.status IN ('fetching','complete') "
                "AND NOT EXISTS(SELECT 1 FROM offline_mailbox_membership existing WHERE "
                "existing.account_id=s.account_id AND existing.mailbox_id=s.mailbox_id AND "
                "existing.generation=s.generation AND existing.email_id=em.email_id)"));
            addMembership.bindValue(QStringLiteral(":account"),
                                    QString::fromStdString(std::string{accountId}));
            addMembership.bindValue(QStringLiteral(":mailbox"),
                                    QString::fromStdString(std::string{mailboxId}));
            if (!addMembership.exec())
                return QStringLiteral("Extend offline mailbox membership: ") +
                       addMembership.lastError().text();

            QSqlQuery removeMembership{database};
            removeMembership.prepare(QStringLiteral(
                "DELETE FROM offline_mailbox_membership AS om WHERE om.account_id=:account AND "
                "om.mailbox_id=:mailbox AND om.generation=(SELECT generation FROM "
                "offline_mailbox_scopes WHERE account_id=:account AND mailbox_id=:mailbox AND "
                "desired=1 AND status IN ('fetching','complete')) AND NOT EXISTS(SELECT 1 FROM "
                "email_mailboxes em WHERE em.account_id=om.account_id AND "
                "em.mailbox_id=om.mailbox_id AND em.email_id=om.email_id)"));
            removeMembership.bindValue(QStringLiteral(":account"),
                                       QString::fromStdString(std::string{accountId}));
            removeMembership.bindValue(QStringLiteral(":mailbox"),
                                       QString::fromStdString(std::string{mailboxId}));
            if (!removeMembership.exec())
                return QStringLiteral("Prune offline mailbox membership: ") +
                       removeMembership.lastError().text();

            QSqlQuery retain{database};
            retain.prepare(QStringLiteral(
                "UPDATE mail_vault_email_refs SET retention='full_sync' WHERE account_id=:account "
                "AND email_id IN (SELECT email_id FROM email_mailboxes WHERE account_id=:account "
                "AND mailbox_id=:mailbox)"));
            retain.bindValue(QStringLiteral(":account"),
                             QString::fromStdString(std::string{accountId}));
            retain.bindValue(QStringLiteral(":mailbox"),
                             QString::fromStdString(std::string{mailboxId}));
            if (!retain.exec())
                return QStringLiteral("Retain synchronized mailbox changes: ") +
                       retain.lastError().text();

            QSqlQuery advanceScope{database};
            advanceScope.prepare(QStringLiteral(
                "UPDATE offline_mailbox_scopes SET email_state=COALESCE((SELECT state_token FROM "
                "sync_state WHERE account_id=:account AND object_type='Email' AND query_key=''),"
                "email_state),expected_total=(SELECT COUNT(*) FROM email_mailboxes WHERE "
                "account_id=:account AND mailbox_id=:mailbox),completed_total=(SELECT COUNT(*) "
                "FROM email_mailboxes WHERE account_id=:account AND mailbox_id=:mailbox),"
                "estimated_bytes=(SELECT COALESCE(SUM(e.size),0) FROM email_mailboxes m JOIN "
                "emails e ON e.account_id=m.account_id AND e.email_id=m.email_id WHERE "
                "m.account_id=:account AND m.mailbox_id=:mailbox),completed_bytes=(SELECT "
                "COALESCE(SUM(e.size),0) FROM email_mailboxes m JOIN emails e ON "
                "e.account_id=m.account_id AND e.email_id=m.email_id JOIN mail_vault_email_refs r "
                "ON r.account_id=e.account_id AND r.email_id=e.email_id AND r.blob_id=e.blob_id "
                "WHERE m.account_id=:account AND m.mailbox_id=:mailbox),updated_at="
                "CURRENT_TIMESTAMP WHERE account_id=:account AND mailbox_id=:mailbox AND desired=1 "
                "AND status IN ('fetching','complete')"));
            advanceScope.bindValue(QStringLiteral(":account"),
                                   QString::fromStdString(std::string{accountId}));
            advanceScope.bindValue(QStringLiteral(":mailbox"),
                                   QString::fromStdString(std::string{mailboxId}));
            if (!advanceScope.exec())
                return QStringLiteral("Advance offline mailbox baseline: ") +
                       advanceScope.lastError().text();
            return {};
        }

        [[nodiscard]] OfflineBodyWork
        offlineBodyWork(javelin::jmap::cache::DatabaseConnection& connection,
                        const std::string_view accountId, const std::string_view mailboxId)
        {
            OfflineBodyWork work;
            QSqlQuery totals{connection.database()};
            totals.prepare(QStringLiteral(
                "SELECT COUNT(*),COALESCE(SUM(e.size),0) FROM email_mailboxes m JOIN emails e ON "
                "e.account_id=m.account_id AND e.email_id=m.email_id WHERE m.account_id=:account "
                "AND m.mailbox_id=:mailbox"));
            totals.bindValue(QStringLiteral(":account"),
                             QString::fromStdString(std::string{accountId}));
            totals.bindValue(QStringLiteral(":mailbox"),
                             QString::fromStdString(std::string{mailboxId}));
            if (!totals.exec() || !totals.next())
            {
                work.error = QStringLiteral("Read offline mailbox body totals: ") +
                             totals.lastError().text();
                return work;
            }
            work.totalUnits = totals.value(0).toULongLong();
            work.totalBytes = totals.value(1).toULongLong();
            totals.finish();

            QSqlQuery missingTotals{connection.database()};
            missingTotals.prepare(QStringLiteral(
                "SELECT COUNT(*),COALESCE(SUM(e.size),0) FROM email_mailboxes m JOIN emails e ON "
                "e.account_id=m.account_id AND e.email_id=m.email_id LEFT JOIN "
                "mail_vault_email_refs r ON r.account_id=e.account_id AND r.email_id=e.email_id "
                "AND r.blob_id=e.blob_id WHERE m.account_id=:account AND m.mailbox_id=:mailbox AND "
                "r.email_id IS NULL"));
            missingTotals.bindValue(QStringLiteral(":account"),
                                    QString::fromStdString(std::string{accountId}));
            missingTotals.bindValue(QStringLiteral(":mailbox"),
                                    QString::fromStdString(std::string{mailboxId}));
            if (!missingTotals.exec() || !missingTotals.next())
            {
                work.error = QStringLiteral("Read missing offline mailbox body totals: ") +
                             missingTotals.lastError().text();
                return work;
            }
            work.missingUnits = missingTotals.value(0).toULongLong();
            work.missingBytes = missingTotals.value(1).toULongLong();
            missingTotals.finish();
            if (work.missingUnits == 0)
                return work;

            QSqlQuery missing{connection.database()};
            missing.prepare(QStringLiteral(
                "SELECT e.email_id,e.size FROM email_mailboxes m JOIN emails e ON "
                "e.account_id=m.account_id AND e.email_id=m.email_id LEFT JOIN "
                "mail_vault_email_refs r ON r.account_id=e.account_id AND r.email_id=e.email_id "
                "AND r.blob_id=e.blob_id WHERE m.account_id=:account AND m.mailbox_id=:mailbox AND "
                "r.email_id IS NULL ORDER BY e.received_at DESC,e.email_id LIMIT :limit"));
            missing.bindValue(QStringLiteral(":account"),
                              QString::fromStdString(std::string{accountId}));
            missing.bindValue(QStringLiteral(":mailbox"),
                              QString::fromStdString(std::string{mailboxId}));
            missing.bindValue(QStringLiteral(":limit"),
                              static_cast<qulonglong>(offlineBodyBatchSize));
            if (!missing.exec())
            {
                work.error = QStringLiteral("Read missing offline mailbox body batch: ") +
                             missing.lastError().text();
                return work;
            }
            work.downloads.reserve(static_cast<std::size_t>(
                std::min<std::uint64_t>(work.missingUnits, offlineBodyBatchSize)));
            while (missing.next())
                work.downloads.emplace_back(missing.value(0).toString().toStdString(),
                                            missing.value(1).toULongLong());
            return work;
        }

        struct OfflineCompletionResult
        {
            QString error;
            bool complete = false;
            std::uint64_t totalUnits = 0;
            std::uint64_t totalBytes = 0;
        };

        [[nodiscard]] OfflineCompletionResult
        tryCompleteOfflineMailbox(javelin::jmap::cache::DatabaseConnection& connection,
                                  const std::string_view accountId,
                                  const std::string_view mailboxId, const std::uint64_t generation)
        {
            auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
                connection, QStringLiteral("Complete offline mailbox hydration"));
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
                return {.error = error->message};
            auto transaction =
                std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(transactionResult));

            if (const auto error =
                    reconcileOfflineMailboxMembership(connection, accountId, mailboxId);
                !error.isEmpty())
            {
                transaction.rollback();
                return {.error = error};
            }

            QSqlQuery missing{connection.database()};
            missing.prepare(QStringLiteral(
                "SELECT EXISTS(SELECT 1 FROM email_mailboxes m JOIN emails e ON "
                "e.account_id=m.account_id AND e.email_id=m.email_id LEFT JOIN "
                "mail_vault_email_refs r ON r.account_id=e.account_id AND r.email_id=e.email_id "
                "AND r.blob_id=e.blob_id WHERE m.account_id=:account AND m.mailbox_id=:mailbox AND "
                "r.email_id IS NULL)"));
            missing.bindValue(QStringLiteral(":account"),
                              QString::fromStdString(std::string{accountId}));
            missing.bindValue(QStringLiteral(":mailbox"),
                              QString::fromStdString(std::string{mailboxId}));
            if (!missing.exec() || !missing.next())
            {
                const auto error = QStringLiteral("Verify offline mailbox hydration: ") +
                                   missing.lastError().text();
                transaction.rollback();
                return {.error = error};
            }
            if (missing.value(0).toBool())
            {
                if (const auto error = transaction.commit())
                    return {.error = error->message};
                return {};
            }

            QSqlQuery totals{connection.database()};
            totals.prepare(QStringLiteral(
                "SELECT COALESCE(expected_total,0),COALESCE(estimated_bytes,0) FROM "
                "offline_mailbox_scopes WHERE account_id=:account AND mailbox_id=:mailbox AND "
                "desired=1 AND generation=:generation"));
            totals.bindValue(QStringLiteral(":account"),
                             QString::fromStdString(std::string{accountId}));
            totals.bindValue(QStringLiteral(":mailbox"),
                             QString::fromStdString(std::string{mailboxId}));
            totals.bindValue(QStringLiteral(":generation"), static_cast<qulonglong>(generation));
            if (!totals.exec() || !totals.next())
            {
                const auto error = totals.lastError().text().isEmpty()
                                       ? QStringLiteral("Offline mailbox totals are unavailable.")
                                       : totals.lastError().text();
                transaction.rollback();
                return {.error = error};
            }
            const auto totalUnits = totals.value(0).toULongLong();
            const auto totalBytes = totals.value(1).toULongLong();

            QSqlQuery complete{connection.database()};
            complete.prepare(QStringLiteral(
                "UPDATE offline_mailbox_scopes SET status='complete',completed_generation="
                ":generation,completed_bytes=COALESCE(estimated_bytes,completed_bytes),"
                "latest_error=NULL,updated_at=CURRENT_TIMESTAMP WHERE account_id=:account AND "
                "mailbox_id=:mailbox AND desired=1 AND generation=:generation AND status IN "
                "('fetching','complete')"));
            complete.bindValue(QStringLiteral(":generation"), static_cast<qulonglong>(generation));
            complete.bindValue(QStringLiteral(":account"),
                               QString::fromStdString(std::string{accountId}));
            complete.bindValue(QStringLiteral(":mailbox"),
                               QString::fromStdString(std::string{mailboxId}));
            if (!complete.exec() || complete.numRowsAffected() != 1)
            {
                const auto error = complete.lastError().text().isEmpty()
                                       ? QStringLiteral("Offline mailbox generation changed before "
                                                        "hydration completed.")
                                       : complete.lastError().text();
                transaction.rollback();
                return {.error = error};
            }

            QSqlQuery retention{connection.database()};
            retention.prepare(QStringLiteral(
                "UPDATE mail_vault_email_refs SET retention='full_sync' WHERE account_id=:account "
                "AND email_id IN (SELECT email_id FROM email_mailboxes WHERE account_id=:account "
                "AND mailbox_id=:mailbox)"));
            retention.bindValue(QStringLiteral(":account"),
                                QString::fromStdString(std::string{accountId}));
            retention.bindValue(QStringLiteral(":mailbox"),
                                QString::fromStdString(std::string{mailboxId}));
            if (!retention.exec())
            {
                const auto error =
                    QStringLiteral("Retain synchronized mail: ") + retention.lastError().text();
                transaction.rollback();
                return {.error = error};
            }
            if (const auto error = transaction.commit())
                return {.error = error->message};
            return {
                .error = {}, .complete = true, .totalUnits = totalUnits, .totalBytes = totalBytes};
        }

        [[nodiscard]] QString
        mailboxDisplayName(javelin::jmap::cache::DatabaseConnection& connection,
                           const std::string_view accountId, const std::string_view mailboxId)
        {
            QSqlQuery query{connection.database()};
            query.prepare(QStringLiteral(
                "SELECT name FROM mailboxes WHERE account_id=:account AND mailbox_id=:mailbox"));
            query.bindValue(QStringLiteral(":account"),
                            QString::fromStdString(std::string{accountId}));
            query.bindValue(QStringLiteral(":mailbox"),
                            QString::fromStdString(std::string{mailboxId}));
            if (query.exec() && query.next() && !query.value(0).toString().isEmpty())
                return query.value(0).toString();
            return QString::fromStdString(std::string{mailboxId});
        }
    } // namespace

    FullMailSyncService::FullMailSyncService(javelin::jmap::cache::DatabaseConnection& connection,
                                             javelin::jmap::JmapCore& core,
                                             WorkScheduler& scheduler,
                                             MailIndexService& indexService, QObject* parent)
        : QObject(parent), m_connection(connection), m_core(core), m_scheduler(scheduler),
          m_indexService(indexService)
    {
        connect(&m_scheduler, &WorkScheduler::jobsChanged, this, [this]() { schedulePump(); });
        connect(&m_scheduler, &WorkScheduler::foregroundAvailabilityChanged, this,
                [this]() { schedulePump(); });
    }

    void
    FullMailSyncService::applySettings(std::vector<FullSyncAccountConfiguration> configurations)
    {
        const javelin::jmap::cache::DatabaseWriteScope writeScope{m_connection};
        std::unordered_set<std::string> desiredKeys;
        m_settings.clear();
        m_scopes.clear();
        for (auto& configuration : configurations)
        {
            m_settings.insert_or_assign(configuration.accountId, configuration.settings);
            QSqlQuery accountMetadata{m_connection.database()};
            accountMetadata.prepare(QStringLiteral(
                "INSERT INTO mail_vault_projection_jobs(account_id,email_id,operation) "
                "SELECT :account,'','metadata' WHERE EXISTS(SELECT 1 FROM accounts WHERE "
                "account_id=:account) AND NOT EXISTS(SELECT 1 FROM mail_vault_projection_jobs "
                "WHERE account_id=:account AND email_id='' AND mailbox_id IS NULL AND "
                "operation='metadata' AND status='pending')"));
            accountMetadata.bindValue(QStringLiteral(":account"),
                                      QString::fromStdString(configuration.accountId));
            if (!accountMetadata.exec())
                logDatabaseFailure(QStringLiteral("Queue mail account metadata"), accountMetadata);
            for (const auto& mailboxId : configuration.mailboxIds)
            {
                const auto id = jobId(configuration.accountId, mailboxId);
                Scope scope{
                    .accountId = configuration.accountId, .mailboxId = mailboxId, .jobId = id};
                m_scopes.insert_or_assign(id, scope);
                if (mailboxSubscribed(configuration.accountId, mailboxId) != std::optional{true})
                    continue;
                desiredKeys.insert(configuration.accountId + "\n" + mailboxId);

                bool wasDisabled = false;
                QSqlQuery previous{m_connection.database()};
                previous.prepare(QStringLiteral(
                    "SELECT desired FROM offline_mailbox_scopes WHERE account_id=:account AND "
                    "mailbox_id=:mailbox"));
                previous.bindValue(QStringLiteral(":account"),
                                   QString::fromStdString(configuration.accountId));
                previous.bindValue(QStringLiteral(":mailbox"), QString::fromStdString(mailboxId));
                if (!previous.exec())
                    logDatabaseFailure(QStringLiteral("Inspect full mailbox scope"), previous);
                else if (previous.next())
                    wasDisabled = !previous.value(0).toBool();
                previous.finish();

                QSqlQuery upsert{m_connection.database()};
                upsert.prepare(QStringLiteral(
                    "INSERT INTO offline_mailbox_scopes(account_id,mailbox_id,desired,status) "
                    "VALUES(:account,:mailbox,1,'pending') ON CONFLICT(account_id,mailbox_id) DO "
                    "UPDATE SET desired=1,status=CASE WHEN offline_mailbox_scopes.desired=0 AND "
                    "offline_mailbox_scopes.completed_generation IS NOT NULL THEN 'complete' WHEN "
                    "offline_mailbox_scopes.desired=0 THEN 'pending' ELSE "
                    "offline_mailbox_scopes.status END,updated_at=CURRENT_TIMESTAMP"));
                upsert.bindValue(QStringLiteral(":account"),
                                 QString::fromStdString(configuration.accountId));
                upsert.bindValue(QStringLiteral(":mailbox"), QString::fromStdString(mailboxId));
                if (!upsert.exec())
                {
                    logDatabaseFailure(QStringLiteral("Configure full mailbox scope"), upsert);
                    continue;
                }
                QSqlQuery mailboxMetadata{m_connection.database()};
                mailboxMetadata.prepare(QStringLiteral(
                    "INSERT INTO mail_vault_projection_jobs(account_id,email_id,mailbox_id,"
                    "operation) SELECT :account,'',:mailbox,'metadata' WHERE EXISTS(SELECT 1 FROM "
                    "mailboxes WHERE account_id=:account AND mailbox_id=:mailbox) AND NOT "
                    "EXISTS(SELECT 1 FROM mail_vault_projection_jobs WHERE account_id=:account "
                    "AND email_id='' AND mailbox_id=:mailbox AND operation='metadata' AND "
                    "status='pending')"));
                mailboxMetadata.bindValue(QStringLiteral(":account"),
                                          QString::fromStdString(configuration.accountId));
                mailboxMetadata.bindValue(QStringLiteral(":mailbox"),
                                          QString::fromStdString(mailboxId));
                if (!mailboxMetadata.exec())
                    logDatabaseFailure(QStringLiteral("Queue mail mailbox metadata"),
                                       mailboxMetadata);
                static_cast<void>(m_scheduler.ensure({
                    .jobId = id,
                    .parentJobId = std::nullopt,
                    .accountId = configuration.accountId,
                    .kind = WorkKind::FullMailSync,
                    .priority = WorkPriority::Bulk,
                    .title =
                        i18n("Download all mail in %1",
                             mailboxDisplayName(m_connection, configuration.accountId, mailboxId)),
                    .checkpointJson = QStringLiteral("{}"),
                }));
                if (wasDisabled)
                    static_cast<void>(m_scheduler.resume(id));
            }
        }

        std::vector<std::pair<std::string, std::string>> existingScopes;
        QSqlQuery existing{m_connection.database()};
        if (existing.exec(QStringLiteral(
                "SELECT account_id,mailbox_id FROM offline_mailbox_scopes WHERE desired=1")))
        {
            while (existing.next())
                existingScopes.emplace_back(existing.value(0).toString().toStdString(),
                                            existing.value(1).toString().toStdString());
            existing.finish();
        }
        for (const auto& [accountId, mailboxId] : existingScopes)
        {
            if (desiredKeys.contains(accountId + "\n" + mailboxId))
                continue;
            QSqlQuery disable{m_connection.database()};
            disable.prepare(QStringLiteral(
                "UPDATE offline_mailbox_scopes SET desired=0,status='paused',updated_at="
                "CURRENT_TIMESTAMP WHERE account_id=:account AND mailbox_id=:mailbox"));
            disable.bindValue(QStringLiteral(":account"), QString::fromStdString(accountId));
            disable.bindValue(QStringLiteral(":mailbox"), QString::fromStdString(mailboxId));
            if (!disable.exec())
                logDatabaseFailure(QStringLiteral("Disable full mailbox scope"), disable);
            static_cast<void>(m_scheduler.pause(jobId(accountId, mailboxId)));
        }

        QSqlQuery retention{m_connection.database()};
        if (!retention.exec(QStringLiteral(
                "UPDATE mail_vault_email_refs AS r SET retention=CASE WHEN EXISTS(SELECT 1 FROM "
                "mail_vault_mailbox_refs mr JOIN offline_mailbox_scopes s ON "
                "s.account_id=mr.account_id AND s.mailbox_id=mr.mailbox_id AND s.desired=1 WHERE "
                "mr.account_id=r.account_id AND mr.email_id=r.email_id) THEN 'full_sync' ELSE "
                "'evictable' END")))
            logDatabaseFailure(QStringLiteral("Update mail retention"), retention);
        schedulePump();
    }

    void FullMailSyncService::refreshMailboxVisibility(const std::string_view accountId)
    {
        if (!m_settings.contains(std::string{accountId}))
            return;

        std::vector<FullSyncAccountConfiguration> configurations;
        configurations.reserve(m_settings.size());
        for (const auto& [configuredAccountId, settings] : m_settings)
        {
            FullSyncAccountConfiguration configuration{
                .settings = settings, .accountId = configuredAccountId, .mailboxIds = {}};
            for (const auto& [id, scope] : m_scopes)
            {
                Q_UNUSED(id);
                if (scope.accountId == configuredAccountId)
                    configuration.mailboxIds.push_back(scope.mailboxId);
            }
            configurations.push_back(std::move(configuration));
        }
        applySettings(std::move(configurations));
    }

    void FullMailSyncService::requestCatchUp(const std::string_view accountId)
    {
        if (m_runningAccounts.contains(std::string{accountId}))
        {
            m_dirtyAccounts.insert(std::string{accountId});
            return;
        }

        for (const auto& [id, scope] : m_scopes)
        {
            if (scope.accountId != accountId)
                continue;

            auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
                m_connection, QStringLiteral("Reconcile offline mailbox catch-up"));
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
            {
                qWarning().noquote() << "Reconcile offline mailbox catch-up" << error->message;
                continue;
            }
            auto transaction =
                std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(transactionResult));

            if (const auto error = reconcileOfflineMailboxMembership(m_connection, scope.accountId,
                                                                     scope.mailboxId);
                !error.isEmpty())
            {
                qWarning().noquote() << error;
                transaction.rollback();
                continue;
            }

            QSqlQuery missing{m_connection.database()};
            missing.prepare(QStringLiteral(
                "SELECT EXISTS(SELECT 1 FROM email_mailboxes m JOIN emails e ON "
                "e.account_id=m.account_id AND e.email_id=m.email_id LEFT JOIN "
                "mail_vault_email_refs r ON r.account_id=e.account_id AND r.email_id=e.email_id "
                "AND r.blob_id=e.blob_id WHERE m.account_id=:account AND m.mailbox_id=:mailbox "
                "AND r.email_id IS NULL)"));
            missing.bindValue(QStringLiteral(":account"), QString::fromStdString(scope.accountId));
            missing.bindValue(QStringLiteral(":mailbox"), QString::fromStdString(scope.mailboxId));
            if (!missing.exec() || !missing.next())
            {
                logDatabaseFailure(QStringLiteral("Inspect offline mailbox catch-up"), missing);
                transaction.rollback();
                continue;
            }
            const bool hasMissingSource = missing.value(0).toBool();
            missing.finish();

            if (hasMissingSource)
            {
                QSqlQuery fetching{m_connection.database()};
                fetching.prepare(QStringLiteral(
                    "UPDATE offline_mailbox_scopes SET status='fetching',updated_at="
                    "CURRENT_TIMESTAMP WHERE account_id=:account AND mailbox_id=:mailbox AND "
                    "desired=1 AND status='complete'"));
                fetching.bindValue(QStringLiteral(":account"),
                                   QString::fromStdString(scope.accountId));
                fetching.bindValue(QStringLiteral(":mailbox"),
                                   QString::fromStdString(scope.mailboxId));
                if (!fetching.exec())
                {
                    logDatabaseFailure(QStringLiteral("Resume offline mailbox hydration"),
                                       fetching);
                    transaction.rollback();
                    continue;
                }

                QSqlQuery queue{m_connection.database()};
                queue.prepare(QStringLiteral(
                    "UPDATE background_jobs SET status='queued',updated_at=CURRENT_TIMESTAMP WHERE "
                    "job_id=:id AND status IN ('complete','failed') AND pause_requested=0"));
                queue.bindValue(QStringLiteral(":id"), QString::fromStdString(id));
                if (!queue.exec())
                {
                    logDatabaseFailure(QStringLiteral("Queue full mailbox catch-up"), queue);
                    transaction.rollback();
                    continue;
                }
            }

            if (const auto error = transaction.commit())
                qWarning().noquote() << "Commit offline mailbox catch-up" << error->message;
        }
        schedulePump();
    }

    void FullMailSyncService::requestMailboxResync(const std::string_view accountId,
                                                   const std::string_view mailboxId)
    {
        const auto id = jobId(accountId, mailboxId);
        const auto scope = m_scopes.find(id);
        if (scope == m_scopes.end())
            return;

        const javelin::jmap::cache::DatabaseWriteScope writeScope{m_connection};
        QSqlQuery desired{m_connection.database()};
        desired.prepare(QStringLiteral(
            "SELECT desired FROM offline_mailbox_scopes WHERE account_id=:account AND "
            "mailbox_id=:mailbox"));
        desired.bindValue(QStringLiteral(":account"),
                          QString::fromStdString(std::string{accountId}));
        desired.bindValue(QStringLiteral(":mailbox"),
                          QString::fromStdString(std::string{mailboxId}));
        if (!desired.exec() || !desired.next() || !desired.value(0).toBool())
            return;

        if (m_runningAccounts.contains(std::string{accountId}))
        {
            m_dirtyAccounts.insert(std::string{accountId});
            return;
        }

        QSqlQuery queue{m_connection.database()};
        queue.prepare(QStringLiteral(
            "UPDATE background_jobs SET status='queued',pause_requested=0,error_text=NULL,"
            "updated_at=CURRENT_TIMESTAMP WHERE job_id=:id AND status<>'running'"));
        queue.bindValue(QStringLiteral(":id"), QString::fromStdString(id));
        if (!queue.exec())
        {
            logDatabaseFailure(QStringLiteral("Queue full mailbox resynchronization"), queue);
            return;
        }
        schedulePump();
    }

    void FullMailSyncService::schedulePump()
    {
        if (m_pumpScheduled)
            return;
        m_pumpScheduled = true;
        QTimer::singleShot(0, this,
                           [this]()
                           {
                               m_pumpScheduled = false;
                               pump();
                           });
    }

    void FullMailSyncService::pump()
    {
        if (!m_scheduler.mayStartBackgroundNetwork())
            return;
        const auto records = m_scheduler.list();
        const auto* jobs = std::get_if<std::vector<WorkRecord>>(&records);
        if (jobs == nullptr)
            return;
        for (const auto& job : *jobs)
        {
            if (job.kind != WorkKind::FullMailSync || job.status != WorkStatus::Queued)
                continue;
            const auto scope = m_scopes.find(job.jobId);
            if (scope == m_scopes.end() || m_runningAccounts.contains(scope->second.accountId))
                continue;
            if (!m_scheduler.admit(job.jobId).has_value())
                continue;
            m_runningAccounts.insert(scope->second.accountId);
            auto task = run(scope->second);
            QCoro::connect(std::move(task), this,
                           [this, accountId = scope->second.accountId, jobId = job.jobId]()
                           {
                               m_scheduler.release(jobId);
                               m_runningAccounts.erase(accountId);
                               if (m_dirtyAccounts.erase(accountId) != 0)
                                   requestCatchUp(accountId);
                               schedulePump();
                           });
        }
    }

    QCoro::Task<bool> FullMailSyncService::waitForBackgroundNetwork(std::string id)
    {
        const QPointer<FullMailSyncService> self{this};
        while (!m_scheduler.mayStartBackgroundNetwork())
        {
            const auto job = m_scheduler.find(id);
            const auto* value = std::get_if<std::optional<WorkRecord>>(&job);
            if (value == nullptr || !value->has_value() || (*value)->pauseRequested)
                co_return false;
            QTimer timer;
            timer.setSingleShot(true);
            timer.start(std::chrono::milliseconds{100});
            co_await qCoro(timer).waitForTimeout();
            if (!self)
                co_return false;
        }
        const auto job = m_scheduler.find(id);
        const auto* value = std::get_if<std::optional<WorkRecord>>(&job);
        co_return value != nullptr && value->has_value() && !(*value)->pauseRequested;
    }

    QCoro::Task<void> FullMailSyncService::run(Scope scope)
    {
        const QPointer<FullMailSyncService> self{this};
        const auto accountSettings = settingsFor(scope.accountId);
        if (!accountSettings)
            co_return;
        if (mailboxSubscribed(scope.accountId, scope.mailboxId) != std::optional{true})
        {
            static_cast<void>(m_scheduler.pause(scope.jobId));
            co_return;
        }
        WorkProgress progress;

        QSqlQuery state{m_connection.database()};
        state.prepare(QStringLiteral(
            "SELECT status,generation,completed_generation,expected_total,completed_total FROM "
            "offline_mailbox_scopes WHERE account_id=:account AND mailbox_id=:mailbox AND "
            "desired=1"));
        state.bindValue(QStringLiteral(":account"), QString::fromStdString(scope.accountId));
        state.bindValue(QStringLiteral(":mailbox"), QString::fromStdString(scope.mailboxId));
        if (!state.exec() || !state.next())
        {
            static_cast<void>(
                m_scheduler.update(scope.jobId, WorkStatus::Failed, progress, QStringLiteral("{}"),
                                   state.lastError().text().isEmpty()
                                       ? std::optional<QString>{QStringLiteral(
                                             "Offline mailbox scope is unavailable.")}
                                       : std::optional<QString>{state.lastError().text()}));
            co_return;
        }

        const QString scopeStatus = state.value(0).toString();
        const bool hasCompletedGeneration = !state.value(2).isNull();
        std::uint64_t generation = state.value(1).toULongLong();
        const bool resumeBodyHydration = scopeStatus == QStringLiteral("fetching");
        const bool completedBaseline =
            scopeStatus == QStringLiteral("complete") && hasCompletedGeneration;
        if ((resumeBodyHydration && generation == 0) ||
            (scopeStatus == QStringLiteral("complete") && !hasCompletedGeneration))
        {
            state.finish();
            static_cast<void>(
                m_scheduler.update(scope.jobId, WorkStatus::Failed, progress, QStringLiteral("{}"),
                                   i18n("Offline mailbox synchronization state is inconsistent.")));
            co_return;
        }

        const bool needsEnumeration = !resumeBodyHydration && !completedBaseline;
        if (needsEnumeration)
        {
            progress.completedUnits = state.value(4).toULongLong();
            if (!state.value(3).isNull())
                progress.totalUnits = state.value(3).toULongLong();
            progress.detail = i18n("Reading mailbox contents");
        }
        else
        {
            progress.detail = i18n("Downloading complete messages");
        }
        state.finish();
        static_cast<void>(
            m_scheduler.update(scope.jobId, WorkStatus::Running, progress,
                               checkpoint(scopeStatus, progress.completedUnits, generation)));
        if (needsEnumeration)
        {
            const bool resumeEnumeration =
                scopeStatus == QStringLiteral("enumerating") && generation != 0;
            if (!resumeEnumeration)
            {
                const javelin::jmap::cache::DatabaseWriteScope writeScope{m_connection};
                ++generation;
                QSqlQuery begin{m_connection.database()};
                begin.prepare(QStringLiteral(
                    "UPDATE offline_mailbox_scopes SET status='enumerating',generation=:generation,"
                    "anchor_email_id=NULL,expected_total=NULL,completed_total=0,completed_bytes=0,"
                    "latest_error=NULL,updated_at=CURRENT_TIMESTAMP WHERE account_id=:account AND "
                    "mailbox_id=:mailbox"));
                begin.bindValue(QStringLiteral(":generation"), static_cast<qulonglong>(generation));
                begin.bindValue(QStringLiteral(":account"),
                                QString::fromStdString(scope.accountId));
                begin.bindValue(QStringLiteral(":mailbox"),
                                QString::fromStdString(scope.mailboxId));
                if (!begin.exec())
                {
                    static_cast<void>(m_scheduler.update(scope.jobId, WorkStatus::Failed, progress,
                                                         QStringLiteral("{}"),
                                                         begin.lastError().text()));
                    co_return;
                }
                QSqlQuery clear{m_connection.database()};
                clear.prepare(QStringLiteral(
                    "DELETE FROM offline_mailbox_membership WHERE account_id=:account AND "
                    "mailbox_id=:mailbox AND generation=:generation"));
                clear.bindValue(QStringLiteral(":account"),
                                QString::fromStdString(scope.accountId));
                clear.bindValue(QStringLiteral(":mailbox"),
                                QString::fromStdString(scope.mailboxId));
                clear.bindValue(QStringLiteral(":generation"), static_cast<qulonglong>(generation));
                if (!clear.exec())
                    logDatabaseFailure(QStringLiteral("Clear mailbox staging generation"), clear);
            }

            std::size_t position = 0;
            std::optional<std::size_t> total;
            std::optional<std::string> anchor;
            const auto pageSize = fullMailboxPageSize(m_connection, scope.accountId);
            if (resumeEnumeration)
            {
                QSqlQuery resume{m_connection.database()};
                resume.prepare(QStringLiteral(
                    "SELECT anchor_email_id,completed_total,expected_total FROM "
                    "offline_mailbox_scopes WHERE account_id=:account AND mailbox_id=:mailbox"));
                resume.bindValue(QStringLiteral(":account"),
                                 QString::fromStdString(scope.accountId));
                resume.bindValue(QStringLiteral(":mailbox"),
                                 QString::fromStdString(scope.mailboxId));
                if (!resume.exec() || !resume.next())
                {
                    static_cast<void>(m_scheduler.update(scope.jobId, WorkStatus::Failed, progress,
                                                         QStringLiteral("{}"),
                                                         resume.lastError().text()));
                    co_return;
                }
                if (!resume.value(0).isNull())
                    anchor = resume.value(0).toString().toStdString();
                position = static_cast<std::size_t>(resume.value(1).toULongLong());
                if (!resume.value(2).isNull())
                    total = static_cast<std::size_t>(resume.value(2).toULongLong());
                resume.finish();
            }
            while (true)
            {
                if (mailboxSubscribed(scope.accountId, scope.mailboxId) != std::optional{true})
                {
                    static_cast<void>(m_scheduler.pause(scope.jobId));
                    co_return;
                }
                const bool mayContinue = co_await waitForBackgroundNetwork(scope.jobId);
                if (!self)
                    co_return;
                if (!mayContinue)
                {
                    static_cast<void>(m_scheduler.pause(scope.jobId));
                    co_return;
                }
                qCDebug(logFullMailSync).noquote()
                    << "materialize page" << QString::fromStdString(scope.accountId)
                    << QString::fromStdString(scope.mailboxId) << "offset"
                    << static_cast<qulonglong>(position) << "limit"
                    << static_cast<qulonglong>(pageSize) << "expected"
                    << (total.has_value() ? QString::number(*total) : QStringLiteral("unknown"));
                auto pageResult = co_await m_core.materializeFullMailboxPage(
                    liveSettings(*accountSettings), scope.accountId, scope.mailboxId, position,
                    pageSize, anchor);
                if (!self)
                    co_return;
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&pageResult))
                {
                    if (anchor.has_value() &&
                        error->protocolType == std::optional<std::string>{"anchorNotFound"})
                    {
                        const javelin::jmap::cache::DatabaseWriteScope writeScope{m_connection};
                        QSqlQuery restart{m_connection.database()};
                        restart.prepare(QStringLiteral(
                            "UPDATE offline_mailbox_scopes SET status='pending',anchor_email_id="
                            "NULL WHERE account_id=:account AND mailbox_id=:mailbox"));
                        restart.bindValue(QStringLiteral(":account"),
                                          QString::fromStdString(scope.accountId));
                        restart.bindValue(QStringLiteral(":mailbox"),
                                          QString::fromStdString(scope.mailboxId));
                        static_cast<void>(restart.exec());
                    }
                    static_cast<void>(m_scheduler.update(
                        scope.jobId, WorkStatus::Failed, progress,
                        checkpoint(QStringLiteral("enumerating"), position, generation),
                        error->message));
                    co_return;
                }
                auto page = std::get<javelin::jmap::FullMailboxPage>(std::move(pageResult));
                const auto pagePosition = page.position;
                const auto pageCount = page.emailIds.size();
                auto commitFuture =
                    QtConcurrent::run(commitFullMailboxPage, m_connection.database().databaseName(),
                                      scope.accountId, scope.mailboxId, scope.jobId, generation,
                                      pagePosition, page.emailIds, std::move(page.emails),
                                      page.queryState, std::move(page.emailState), page.total);
                const auto commit = co_await qCoro(commitFuture).takeResult();
                if (!self)
                    co_return;
                if (commit.restartRequired)
                {
                    const javelin::jmap::cache::DatabaseWriteScope writeScope{m_connection};
                    QSqlQuery restart{m_connection.database()};
                    restart.prepare(QStringLiteral(
                        "UPDATE offline_mailbox_scopes SET status='pending',anchor_email_id=NULL,"
                        "query_state=NULL,email_state=NULL,expected_total=NULL,completed_total=0,"
                        "updated_at=CURRENT_TIMESTAMP WHERE account_id=:account AND "
                        "mailbox_id=:mailbox AND generation=:generation"));
                    restart.bindValue(QStringLiteral(":account"),
                                      QString::fromStdString(scope.accountId));
                    restart.bindValue(QStringLiteral(":mailbox"),
                                      QString::fromStdString(scope.mailboxId));
                    restart.bindValue(QStringLiteral(":generation"),
                                      static_cast<qulonglong>(generation));
                    if (!restart.exec())
                    {
                        static_cast<void>(m_scheduler.update(
                            scope.jobId, WorkStatus::Failed, progress,
                            checkpoint(QStringLiteral("enumerating"), position, generation),
                            restart.lastError().text()));
                        co_return;
                    }
                    static_cast<void>(
                        m_scheduler.update(scope.jobId, WorkStatus::Queued, {},
                                           checkpoint(QStringLiteral("pending"), 0, generation)));
                    co_return;
                }
                if (!commit.error.isEmpty())
                {
                    static_cast<void>(m_scheduler.update(
                        scope.jobId, WorkStatus::Failed, progress,
                        checkpoint(QStringLiteral("enumerating"), position, generation),
                        commit.error));
                    co_return;
                }
                for (const auto offset : commit.windowOffsets)
                    Q_EMIT mailboxWindowCommitted(QString::fromStdString(scope.accountId),
                                                  QString::fromStdString(scope.mailboxId), offset,
                                                  canonicalWindowSize);
                if (pagePosition == 0)
                {
                    if (!page.emailIds.empty())
                        anchor = page.emailIds.front();
                }
                total = page.total;
                position = pagePosition + pageCount;
                progress.completedUnits = position;
                progress.totalUnits = total;
                progress.detail = i18n("Reading mailbox contents");
                static_cast<void>(m_scheduler.update(
                    scope.jobId, WorkStatus::Running, progress,
                    checkpoint(QStringLiteral("enumerating"), position, generation)));
                if (pageCount == 0 || (total.has_value() && position >= *total) ||
                    (!total.has_value() && pageCount < pageSize))
                    break;
            }
            const auto coverageResult =
                javelin::jmap::cache::QueryService{m_connection}.offlineMailboxCoverage(
                    scope.accountId, scope.mailboxId);
            const auto* coverage =
                std::get_if<std::optional<javelin::jmap::cache::OfflineMailboxCoverage>>(
                    &coverageResult);
            if (coverage == nullptr || !coverage->has_value())
            {
                const QString error =
                    coverage == nullptr
                        ? std::get<javelin::jmap::cache::DatabaseError>(coverageResult).message
                        : i18n("Offline mailbox coverage disappeared.");
                static_cast<void>(m_scheduler.update(
                    scope.jobId, WorkStatus::Failed, progress,
                    checkpoint(QStringLiteral("enumerating"), position, generation), error));
                co_return;
            }
            QSqlQuery currentScopeState{m_connection.database()};
            currentScopeState.prepare(QStringLiteral(
                "SELECT query_state FROM offline_mailbox_scopes WHERE account_id=:account AND "
                "mailbox_id=:mailbox AND generation=:generation"));
            currentScopeState.bindValue(QStringLiteral(":account"),
                                        QString::fromStdString(scope.accountId));
            currentScopeState.bindValue(QStringLiteral(":mailbox"),
                                        QString::fromStdString(scope.mailboxId));
            currentScopeState.bindValue(QStringLiteral(":generation"),
                                        static_cast<qulonglong>(generation));
            if (!currentScopeState.exec() || !currentScopeState.next() ||
                currentScopeState.value(0).toString().isEmpty())
            {
                static_cast<void>(m_scheduler.update(
                    scope.jobId, WorkStatus::Failed, progress,
                    checkpoint(QStringLiteral("enumerating"), position, generation),
                    currentScopeState.lastError().text().isEmpty()
                        ? i18n("Offline mailbox query state is unavailable.")
                        : currentScopeState.lastError().text()));
                co_return;
            }
            const std::string completedQueryState =
                currentScopeState.value(0).toString().toStdString();
            const javelin::jmap::cache::DatabaseWriteScope writeScope{m_connection};
            auto& database = m_connection.database();
            if (!database.transaction())
            {
                static_cast<void>(m_scheduler.update(
                    scope.jobId, WorkStatus::Failed, progress,
                    checkpoint(QStringLiteral("enumerating"), position, generation),
                    database.lastError().text()));
                co_return;
            }
            QSqlQuery enumerated{database};
            enumerated.prepare(QStringLiteral(
                "UPDATE offline_mailbox_scopes SET status='fetching',query_state=:query_state,"
                "expected_total=:total,completed_total=:total,updated_at=CURRENT_TIMESTAMP WHERE "
                "account_id=:account AND mailbox_id=:mailbox"));
            enumerated.bindValue(QStringLiteral(":query_state"),
                                 QString::fromStdString(completedQueryState));
            enumerated.bindValue(QStringLiteral(":total"), static_cast<qulonglong>(position));
            enumerated.bindValue(QStringLiteral(":account"),
                                 QString::fromStdString(scope.accountId));
            enumerated.bindValue(QStringLiteral(":mailbox"),
                                 QString::fromStdString(scope.mailboxId));
            if (!enumerated.exec())
            {
                const QString error = enumerated.lastError().text();
                database.rollback();
                static_cast<void>(m_scheduler.update(
                    scope.jobId, WorkStatus::Failed, progress,
                    checkpoint(QStringLiteral("enumerating"), position, generation), error));
                co_return;
            }
            QSqlQuery promoteWindows{database};
            promoteWindows.prepare(QStringLiteral(
                "UPDATE mailbox_query_windows SET total=:total,updated_at=CURRENT_TIMESTAMP "
                "WHERE account_id=:account AND mailbox_id=:mailbox"));
            promoteWindows.bindValue(QStringLiteral(":total"),
                                     static_cast<qulonglong>((*coverage)->representativeCount));
            promoteWindows.bindValue(QStringLiteral(":account"),
                                     QString::fromStdString(scope.accountId));
            promoteWindows.bindValue(QStringLiteral(":mailbox"),
                                     QString::fromStdString(scope.mailboxId));
            if (!promoteWindows.exec() || !database.commit())
            {
                const QString error = promoteWindows.lastError().text().isEmpty()
                                          ? database.lastError().text()
                                          : promoteWindows.lastError().text();
                database.rollback();
                static_cast<void>(m_scheduler.update(
                    scope.jobId, WorkStatus::Failed, progress,
                    checkpoint(QStringLiteral("enumerating"), position, generation), error));
                co_return;
            }
        }

        QElapsedTimer progressPersistenceTimer;
        progressPersistenceTimer.start();
        std::uint64_t completedTotalUnits = 0;
        std::uint64_t completedTotalBytes = 0;
        while (true)
        {
            if (mailboxSubscribed(scope.accountId, scope.mailboxId) != std::optional{true})
            {
                static_cast<void>(m_scheduler.pause(scope.jobId));
                co_return;
            }
            OfflineBodyWork work;
            auto prepareResult = javelin::jmap::cache::DatabaseTransaction::begin(
                m_connection, QStringLiteral("Prepare offline mailbox hydration"));
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&prepareResult))
            {
                static_cast<void>(m_scheduler.update(scope.jobId, WorkStatus::Failed, progress,
                                                     QStringLiteral("{}"), error->message));
                co_return;
            }
            auto prepare =
                std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(prepareResult));
            if (const auto error = reconcileOfflineMailboxMembership(m_connection, scope.accountId,
                                                                     scope.mailboxId);
                !error.isEmpty())
            {
                prepare.rollback();
                static_cast<void>(m_scheduler.update(scope.jobId, WorkStatus::Failed, progress,
                                                     QStringLiteral("{}"), error));
                co_return;
            }
            work = offlineBodyWork(m_connection, scope.accountId, scope.mailboxId);
            if (!work.error.isEmpty())
            {
                prepare.rollback();
                static_cast<void>(m_scheduler.update(scope.jobId, WorkStatus::Failed, progress,
                                                     QStringLiteral("{}"), work.error));
                co_return;
            }
            if (!work.downloads.empty())
            {
                QSqlQuery fetching{m_connection.database()};
                fetching.prepare(QStringLiteral(
                    "UPDATE offline_mailbox_scopes SET status='fetching',updated_at="
                    "CURRENT_TIMESTAMP WHERE account_id=:account AND mailbox_id=:mailbox AND "
                    "desired=1 AND generation=:generation AND status='complete'"));
                fetching.bindValue(QStringLiteral(":account"),
                                   QString::fromStdString(scope.accountId));
                fetching.bindValue(QStringLiteral(":mailbox"),
                                   QString::fromStdString(scope.mailboxId));
                fetching.bindValue(QStringLiteral(":generation"),
                                   static_cast<qulonglong>(generation));
                if (!fetching.exec())
                {
                    const auto error = fetching.lastError().text();
                    prepare.rollback();
                    static_cast<void>(m_scheduler.update(scope.jobId, WorkStatus::Failed, progress,
                                                         QStringLiteral("{}"), error));
                    co_return;
                }
            }
            if (const auto error = prepare.commit())
            {
                static_cast<void>(m_scheduler.update(scope.jobId, WorkStatus::Failed, progress,
                                                     QStringLiteral("{}"), error->message));
                co_return;
            }

            const auto missingUnits = work.missingUnits;
            progress = {
                .completedUnits =
                    work.totalUnits >= missingUnits ? work.totalUnits - missingUnits : 0,
                .totalUnits = work.totalUnits,
                .completedBytes =
                    work.totalBytes >= work.missingBytes ? work.totalBytes - work.missingBytes : 0,
                .totalBytes = work.totalBytes,
                .detail = i18n("Downloading complete messages"),
            };
            static_cast<void>(m_scheduler.update(
                scope.jobId, WorkStatus::Running, progress,
                checkpoint(QStringLiteral("fetching"), progress.completedUnits, generation)));

            if (work.downloads.empty())
            {
                const auto completion = tryCompleteOfflineMailbox(m_connection, scope.accountId,
                                                                  scope.mailboxId, generation);
                if (!completion.error.isEmpty())
                {
                    static_cast<void>(m_scheduler.update(
                        scope.jobId, WorkStatus::Failed, progress,
                        checkpoint(QStringLiteral("fetching"), progress.completedUnits, generation),
                        completion.error));
                    co_return;
                }
                if (completion.complete)
                {
                    completedTotalUnits = completion.totalUnits;
                    completedTotalBytes = completion.totalBytes;
                    break;
                }
                continue;
            }

            if (!hasDiskSpace(scope.accountId, scope.mailboxId, work.missingBytes))
            {
                progress.detail = i18n("Waiting for free disk space");
                static_cast<void>(
                    m_scheduler.update(scope.jobId, WorkStatus::WaitingForSpace, progress));
                QTimer::singleShot(std::chrono::minutes{1}, this,
                                   [this, id = scope.jobId]()
                                   {
                                       const auto current = m_scheduler.find(id);
                                       const auto* record =
                                           std::get_if<std::optional<WorkRecord>>(&current);
                                       if (record != nullptr && record->has_value() &&
                                           (*record)->status == WorkStatus::WaitingForSpace &&
                                           !(*record)->pauseRequested)
                                       {
                                           static_cast<void>(m_scheduler.update(
                                               id, WorkStatus::Queued, (*record)->progress,
                                               (*record)->checkpointJson));
                                       }
                                   });
                co_return;
            }

            for (const auto& [emailId, size] : work.downloads)
            {
                const bool mayContinue = co_await waitForBackgroundNetwork(scope.jobId);
                if (!self)
                    co_return;
                if (!mayContinue)
                {
                    static_cast<void>(m_scheduler.pause(scope.jobId));
                    co_return;
                }
                const auto result = co_await m_core.refreshMessageContent(
                    liveSettings(*accountSettings), scope.accountId, emailId);
                if (!self)
                    co_return;
                if (const auto* unavailable =
                        std::get_if<javelin::jmap::MessageContentUnavailable>(&result))
                {
                    static_cast<void>(m_scheduler.update(
                        scope.jobId, WorkStatus::Failed, progress,
                        checkpoint(QStringLiteral("fetching"), progress.completedUnits, generation),
                        unavailable->message));
                    co_return;
                }
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    static_cast<void>(m_scheduler.update(
                        scope.jobId, WorkStatus::Failed, progress,
                        checkpoint(QStringLiteral("fetching"), progress.completedUnits, generation),
                        error->message));
                    co_return;
                }
                const auto& summary = std::get<javelin::jmap::MessageContentRefreshSummary>(result);
                if (!summary.usedCachedContent)
                {
                    Q_EMIT messageContentCommitted(QString::fromStdString(summary.accountId),
                                                   QString::fromStdString(summary.emailId));
                }
                ++progress.completedUnits;
                progress.completedBytes += size;
                if (progressPersistenceTimer.elapsed() >= 1000 ||
                    (progress.totalUnits.has_value() &&
                     progress.completedUnits == *progress.totalUnits))
                {
                    auto persistResult = javelin::jmap::cache::DatabaseTransaction::begin(
                        m_connection, QStringLiteral("Persist offline mailbox body progress"));
                    if (const auto* error =
                            std::get_if<javelin::jmap::cache::DatabaseError>(&persistResult))
                    {
                        static_cast<void>(
                            m_scheduler.update(scope.jobId, WorkStatus::Failed, progress,
                                               checkpoint(QStringLiteral("fetching"),
                                                          progress.completedUnits, generation),
                                               error->message));
                        co_return;
                    }
                    auto persist = std::get<javelin::jmap::cache::DatabaseTransaction>(
                        std::move(persistResult));
                    QSqlQuery saveScope{m_connection.database()};
                    saveScope.prepare(QStringLiteral(
                        "UPDATE offline_mailbox_scopes SET completed_bytes=:completed,"
                        "estimated_bytes=:total,updated_at=CURRENT_TIMESTAMP WHERE "
                        "account_id=:account AND mailbox_id=:mailbox AND desired=1 AND "
                        "generation=:generation AND status='fetching'"));
                    saveScope.bindValue(QStringLiteral(":completed"),
                                        static_cast<qulonglong>(progress.completedBytes));
                    saveScope.bindValue(QStringLiteral(":total"),
                                        static_cast<qulonglong>(work.totalBytes));
                    saveScope.bindValue(QStringLiteral(":account"),
                                        QString::fromStdString(scope.accountId));
                    saveScope.bindValue(QStringLiteral(":mailbox"),
                                        QString::fromStdString(scope.mailboxId));
                    saveScope.bindValue(QStringLiteral(":generation"),
                                        static_cast<qulonglong>(generation));
                    if (!saveScope.exec())
                    {
                        const auto error = saveScope.lastError().text();
                        persist.rollback();
                        static_cast<void>(
                            m_scheduler.update(scope.jobId, WorkStatus::Failed, progress,
                                               checkpoint(QStringLiteral("fetching"),
                                                          progress.completedUnits, generation),
                                               error));
                        co_return;
                    }
                    if (const auto error = persist.commit())
                    {
                        static_cast<void>(
                            m_scheduler.update(scope.jobId, WorkStatus::Failed, progress,
                                               checkpoint(QStringLiteral("fetching"),
                                                          progress.completedUnits, generation),
                                               error->message));
                        co_return;
                    }
                    static_cast<void>(
                        m_scheduler.update(scope.jobId, WorkStatus::Running, progress,
                                           checkpoint(QStringLiteral("fetching"),
                                                      progress.completedUnits, generation)));
                    progressPersistenceTimer.restart();
                }
            }

            const auto completion = tryCompleteOfflineMailbox(m_connection, scope.accountId,
                                                              scope.mailboxId, generation);
            if (!completion.error.isEmpty())
            {
                static_cast<void>(m_scheduler.update(
                    scope.jobId, WorkStatus::Failed, progress,
                    checkpoint(QStringLiteral("fetching"), progress.completedUnits, generation),
                    completion.error));
                co_return;
            }
            if (completion.complete)
            {
                completedTotalUnits = completion.totalUnits;
                completedTotalBytes = completion.totalBytes;
                break;
            }
        }

        progress = {.completedUnits = completedTotalUnits,
                    .totalUnits = completedTotalUnits,
                    .completedBytes = completedTotalBytes,
                    .totalBytes = completedTotalBytes,
                    .detail = i18n("Available offline")};
        static_cast<void>(m_scheduler.update(
            scope.jobId, WorkStatus::Complete, progress,
            checkpoint(QStringLiteral("complete"), progress.completedUnits, generation)));
        m_indexService.requestIndex(scope.accountId);
    }

    bool FullMailSyncService::hasDiskSpace(const std::string_view accountId,
                                           const std::string_view mailboxId,
                                           const std::uint64_t remainingBytes) const
    {
        Q_UNUSED(accountId);
        Q_UNUSED(mailboxId);
        const auto vault = javelin::jmap::cache::MailVault::forDatabase(m_connection);
        if (!QDir{}.mkpath(vault.rootPath()))
            return false;
        QStorageInfo storage{vault.rootPath()};
        if (!storage.isValid() || !storage.isReady())
            return false;
        const auto capacity = static_cast<std::uint64_t>(std::max<qint64>(0, storage.bytesTotal()));
        const auto available =
            static_cast<std::uint64_t>(std::max<qint64>(0, storage.bytesAvailable()));
        const std::uint64_t reserve =
            std::max<std::uint64_t>(2ULL * 1024ULL * 1024ULL * 1024ULL, capacity / 20ULL);
        return available >= remainingBytes && available - remainingBytes >= reserve;
    }

    std::optional<bool>
    FullMailSyncService::mailboxSubscribed(const std::string_view accountId,
                                           const std::string_view mailboxId) const
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral("SELECT is_subscribed FROM mailboxes WHERE "
                                     "account_id=:account AND mailbox_id=:mailbox"));
        query.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":mailbox"), QString::fromStdString(std::string{mailboxId}));
        if (!query.exec() || !query.next())
            return std::nullopt;
        return query.value(0).toBool();
    }

    std::optional<AccountConnectionSettings>
    FullMailSyncService::settingsFor(const std::string_view accountId) const
    {
        const auto found = m_settings.find(std::string{accountId});
        return found == m_settings.end() ? std::nullopt
                                         : std::optional<AccountConnectionSettings>{found->second};
    }
} // namespace javelin::app
