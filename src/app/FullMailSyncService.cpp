#include "app/FullMailSyncService.h"

#include "app/MailIndexService.h"
#include "app/WorkScheduler.h"
#include "jmap/JmapCore.h"
#include "jmap/cache/Database.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailVault.h"
#include "jmap/cache/RawMessageSourceRepository.h"
#include "jmap/sync/MailboxRefreshExecutor.h"

#include <QCoroFuture>
#include <QCoroTask>
#include <QCoroTimer>

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QStorageInfo>
#include <QTimer>
#include <QtConcurrentRun>

#include <algorithm>
#include <chrono>

namespace javelin::app
{
    namespace
    {
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

        [[nodiscard]] QString
        commitFullMailboxPage(const QString& databasePath, const std::string& accountId,
                              const std::string& mailboxId, const std::uint64_t generation,
                              const std::size_t position, std::vector<std::string> emailIds,
                              std::vector<javelin::jmap::domain::Email> emails,
                              std::string emailState)
        {
            javelin::jmap::cache::ThreadConnectionFactory factory({
                .connectionNamePrefix = QStringLiteral("full-mail-page"),
                .databasePath = databasePath,
            });
            auto opened = factory.openForCurrentThread(accountId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&opened))
                return error->message;
            auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));

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
                return error->message;
            if (const auto error = javelin::jmap::sync::rebaseActiveEmailProjections(
                    connection, accountId, emailIds, emailState))
                return error->message;

            auto& database = connection.database();
            if (!database.transaction())
                return database.lastError().text();
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
                    database.rollback();
                    return error;
                }
            }
            if (!database.commit())
            {
                const QString error = database.lastError().text();
                database.rollback();
                return error;
            }
            return {};
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
        std::unordered_set<std::string> desiredKeys;
        m_settings.clear();
        m_scopes.clear();
        for (auto& configuration : configurations)
        {
            m_settings.insert_or_assign(configuration.accountId, configuration.settings);
            QSqlQuery accountMetadata{m_connection.database()};
            accountMetadata.prepare(QStringLiteral(
                "INSERT INTO mail_vault_projection_jobs(account_id,email_id,operation) "
                "VALUES(:account,'','metadata')"));
            accountMetadata.bindValue(QStringLiteral(":account"),
                                      QString::fromStdString(configuration.accountId));
            if (!accountMetadata.exec())
                logDatabaseFailure(QStringLiteral("Queue mail account metadata"), accountMetadata);
            for (const auto& mailboxId : configuration.mailboxIds)
            {
                const auto id = jobId(configuration.accountId, mailboxId);
                desiredKeys.insert(configuration.accountId + "\n" + mailboxId);
                Scope scope{
                    .accountId = configuration.accountId, .mailboxId = mailboxId, .jobId = id};
                m_scopes.insert_or_assign(id, scope);

                QSqlQuery upsert{m_connection.database()};
                upsert.prepare(QStringLiteral(
                    "INSERT INTO offline_mailbox_scopes(account_id,mailbox_id,desired,status) "
                    "VALUES(:account,:mailbox,1,'pending') ON CONFLICT(account_id,mailbox_id) DO "
                    "UPDATE SET desired=1,status=CASE WHEN status='paused' AND "
                    "completed_generation "
                    "IS NULL THEN 'paused' ELSE status END,updated_at=CURRENT_TIMESTAMP"));
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
                    "operation) VALUES(:account,'',:mailbox,'metadata')"));
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
                    .title = QStringLiteral("Download all mail in %1")
                                 .arg(mailboxDisplayName(m_connection, configuration.accountId,
                                                         mailboxId)),
                    .checkpointJson = QStringLiteral("{}"),
                }));
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
                "email_mailboxes em JOIN offline_mailbox_scopes s ON s.account_id=em.account_id "
                "AND s.mailbox_id=em.mailbox_id AND s.desired=1 WHERE em.account_id=r.account_id "
                "AND em.email_id=r.email_id) THEN 'full_sync' ELSE 'evictable' END")))
            logDatabaseFailure(QStringLiteral("Update mail retention"), retention);
        schedulePump();
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
            QSqlQuery retain{m_connection.database()};
            retain.prepare(QStringLiteral(
                "UPDATE mail_vault_email_refs SET retention='full_sync' WHERE "
                "account_id=:account AND email_id IN (SELECT email_id FROM email_mailboxes WHERE "
                "account_id=:account AND mailbox_id=:mailbox)"));
            retain.bindValue(QStringLiteral(":account"), QString::fromStdString(scope.accountId));
            retain.bindValue(QStringLiteral(":mailbox"), QString::fromStdString(scope.mailboxId));
            if (!retain.exec())
                logDatabaseFailure(QStringLiteral("Retain synchronized mailbox changes"), retain);
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
                continue;
            const bool hasMissingSource = missing.value(0).toBool();
            missing.finish();
            if (!hasMissingSource)
                continue;
            QSqlQuery query{m_connection.database()};
            query.prepare(QStringLiteral(
                "UPDATE background_jobs SET status='queued',updated_at=CURRENT_TIMESTAMP WHERE "
                "job_id=:id AND status='complete'"));
            query.bindValue(QStringLiteral(":id"), QString::fromStdString(id));
            if (!query.exec())
                logDatabaseFailure(QStringLiteral("Queue full mailbox catch-up"), query);
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
            m_runningAccounts.insert(scope->second.accountId);
            auto task = run(scope->second);
            QCoro::connect(std::move(task), this,
                           [this, accountId = scope->second.accountId]()
                           {
                               m_runningAccounts.erase(accountId);
                               if (m_dirtyAccounts.erase(accountId) != 0)
                                   requestCatchUp(accountId);
                               schedulePump();
                           });
        }
    }

    QCoro::Task<bool> FullMailSyncService::waitForBackgroundNetwork(std::string id)
    {
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
        }
        const auto job = m_scheduler.find(id);
        const auto* value = std::get_if<std::optional<WorkRecord>>(&job);
        co_return value != nullptr && value->has_value() && !(*value)->pauseRequested;
    }

    QCoro::Task<void> FullMailSyncService::run(Scope scope)
    {
        const auto accountSettings = settingsFor(scope.accountId);
        if (!accountSettings)
            co_return;
        WorkProgress progress;
        static_cast<void>(m_scheduler.update(scope.jobId, WorkStatus::Running, progress));

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

        const bool hasCompletedBaseline = !state.value(2).isNull();
        const QString scopeStatus = state.value(0).toString();
        std::uint64_t generation = state.value(1).toULongLong();
        state.finish();
        if (!hasCompletedBaseline)
        {
            const bool resumeEnumeration =
                scopeStatus == QStringLiteral("enumerating") && generation != 0;
            if (!resumeEnumeration)
            {
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
            std::string initialQueryState;
            if (resumeEnumeration)
            {
                QSqlQuery resume{m_connection.database()};
                resume.prepare(QStringLiteral(
                    "SELECT s.anchor_email_id,s.query_state,COUNT(m.email_id),s.expected_total "
                    "FROM offline_mailbox_scopes s LEFT JOIN offline_mailbox_membership m ON "
                    "m.account_id=s.account_id AND m.mailbox_id=s.mailbox_id AND "
                    "m.generation=s.generation WHERE s.account_id=:account AND "
                    "s.mailbox_id=:mailbox GROUP BY s.account_id,s.mailbox_id"));
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
                initialQueryState = resume.value(1).toString().toStdString();
                position = static_cast<std::size_t>(resume.value(2).toULongLong());
                if (!resume.value(3).isNull())
                    total = static_cast<std::size_t>(resume.value(3).toULongLong());
                resume.finish();
            }
            while (true)
            {
                if (!co_await waitForBackgroundNetwork(scope.jobId))
                {
                    static_cast<void>(m_scheduler.pause(scope.jobId));
                    co_return;
                }
                auto pageResult = co_await m_core.materializeFullMailboxPage(
                    liveSettings(*accountSettings), scope.accountId, scope.mailboxId, position, 250,
                    anchor);
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&pageResult))
                {
                    if (anchor.has_value())
                    {
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
                auto commitFuture = QtConcurrent::run(
                    commitFullMailboxPage, m_connection.database().databaseName(), scope.accountId,
                    scope.mailboxId, generation, position, page.emailIds, std::move(page.emails),
                    std::move(page.emailState));
                const QString commitError = co_await qCoro(commitFuture).takeResult();
                if (!commitError.isEmpty())
                {
                    static_cast<void>(m_scheduler.update(
                        scope.jobId, WorkStatus::Failed, progress,
                        checkpoint(QStringLiteral("enumerating"), position, generation),
                        commitError));
                    co_return;
                }
                if (position == 0)
                {
                    initialQueryState = page.queryState;
                    if (!page.emailIds.empty())
                    {
                        anchor = page.emailIds.front();
                        QSqlQuery saveAnchor{m_connection.database()};
                        saveAnchor.prepare(QStringLiteral(
                            "UPDATE offline_mailbox_scopes SET anchor_email_id=:anchor,"
                            "query_state=:state WHERE account_id=:account AND "
                            "mailbox_id=:mailbox"));
                        saveAnchor.bindValue(QStringLiteral(":anchor"),
                                             QString::fromStdString(*anchor));
                        saveAnchor.bindValue(QStringLiteral(":state"),
                                             QString::fromStdString(initialQueryState));
                        saveAnchor.bindValue(QStringLiteral(":account"),
                                             QString::fromStdString(scope.accountId));
                        saveAnchor.bindValue(QStringLiteral(":mailbox"),
                                             QString::fromStdString(scope.mailboxId));
                        if (!saveAnchor.exec())
                            logDatabaseFailure(QStringLiteral("Save mailbox crawl anchor"),
                                               saveAnchor);
                    }
                }
                total = page.total;
                position += page.emailIds.size();
                progress.completedUnits = position;
                progress.totalUnits = total;
                progress.detail = QStringLiteral("Reading mailbox contents");
                static_cast<void>(m_scheduler.update(
                    scope.jobId, WorkStatus::Running, progress,
                    checkpoint(QStringLiteral("enumerating"), position, generation)));
                if (page.emailIds.size() < 250)
                    break;
            }
            QSqlQuery enumerated{m_connection.database()};
            enumerated.prepare(QStringLiteral(
                "UPDATE offline_mailbox_scopes SET status='fetching',query_state=:query_state,"
                "expected_total=:total,completed_total=:total,updated_at=CURRENT_TIMESTAMP WHERE "
                "account_id=:account AND mailbox_id=:mailbox"));
            enumerated.bindValue(QStringLiteral(":query_state"),
                                 QString::fromStdString(initialQueryState));
            enumerated.bindValue(QStringLiteral(":total"), static_cast<qulonglong>(position));
            enumerated.bindValue(QStringLiteral(":account"),
                                 QString::fromStdString(scope.accountId));
            enumerated.bindValue(QStringLiteral(":mailbox"),
                                 QString::fromStdString(scope.mailboxId));
            if (!enumerated.exec())
                logDatabaseFailure(QStringLiteral("Complete mailbox enumeration"), enumerated);
        }

        QSqlQuery totals{m_connection.database()};
        totals.prepare(QStringLiteral(
            "SELECT COALESCE(SUM(e.size),0),COUNT(*) FROM email_mailboxes m JOIN emails "
            "e ON e.account_id=m.account_id AND e.email_id=m.email_id LEFT JOIN "
            "mail_vault_email_refs r ON r.account_id=e.account_id AND r.email_id=e.email_id AND "
            "r.blob_id=e.blob_id WHERE m.account_id=:account AND m.mailbox_id=:mailbox AND "
            "r.email_id IS NULL"));
        totals.bindValue(QStringLiteral(":account"), QString::fromStdString(scope.accountId));
        totals.bindValue(QStringLiteral(":mailbox"), QString::fromStdString(scope.mailboxId));
        if (!totals.exec() || !totals.next())
        {
            static_cast<void>(m_scheduler.update(scope.jobId, WorkStatus::Failed, progress,
                                                 QStringLiteral("{}"), totals.lastError().text()));
            co_return;
        }
        const std::uint64_t remainingBytes = totals.value(0).toULongLong();
        const std::uint64_t missingCount = totals.value(1).toULongLong();
        totals.finish();
        if (!hasDiskSpace(scope.accountId, scope.mailboxId, remainingBytes))
        {
            progress.totalBytes = remainingBytes;
            progress.totalUnits = missingCount;
            progress.detail = QStringLiteral("Waiting for free disk space");
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

        progress = {.completedUnits = 0,
                    .totalUnits = missingCount,
                    .completedBytes = 0,
                    .totalBytes = remainingBytes,
                    .detail = QStringLiteral("Downloading complete messages")};
        QSqlQuery missing{m_connection.database()};
        missing.prepare(QStringLiteral(
            "SELECT e.email_id,e.size FROM email_mailboxes m JOIN emails e ON "
            "e.account_id=m.account_id AND e.email_id=m.email_id LEFT JOIN mail_vault_email_refs r "
            "ON r.account_id=e.account_id AND r.email_id=e.email_id AND r.blob_id=e.blob_id WHERE "
            "m.account_id=:account AND m.mailbox_id=:mailbox AND "
            "r.email_id IS NULL ORDER BY e.received_at DESC,e.email_id"));
        missing.bindValue(QStringLiteral(":account"), QString::fromStdString(scope.accountId));
        missing.bindValue(QStringLiteral(":mailbox"), QString::fromStdString(scope.mailboxId));
        if (!missing.exec())
        {
            static_cast<void>(m_scheduler.update(scope.jobId, WorkStatus::Failed, progress,
                                                 QStringLiteral("{}"), missing.lastError().text()));
            co_return;
        }
        std::vector<std::pair<std::string, std::uint64_t>> downloads;
        while (missing.next())
            downloads.emplace_back(missing.value(0).toString().toStdString(),
                                   missing.value(1).toULongLong());
        missing.finish();

        for (const auto& [emailId, size] : downloads)
        {
            if (!co_await waitForBackgroundNetwork(scope.jobId))
            {
                static_cast<void>(m_scheduler.pause(scope.jobId));
                co_return;
            }
            const auto result = co_await m_core.refreshMessageContent(
                liveSettings(*accountSettings), scope.accountId, emailId);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
            {
                static_cast<void>(m_scheduler.update(
                    scope.jobId, WorkStatus::Failed, progress,
                    checkpoint(QStringLiteral("fetching"), progress.completedUnits, generation),
                    error->message));
                co_return;
            }
            ++progress.completedUnits;
            progress.completedBytes += size;
            static_cast<void>(m_scheduler.update(
                scope.jobId, WorkStatus::Running, progress,
                checkpoint(QStringLiteral("fetching"), progress.completedUnits, generation)));
        }

        QSqlQuery complete{m_connection.database()};
        complete.prepare(QStringLiteral(
            "UPDATE offline_mailbox_scopes SET status='complete',completed_generation=:generation,"
            "completed_bytes=completed_bytes+:bytes,latest_error=NULL,updated_at=CURRENT_TIMESTAMP "
            "WHERE account_id=:account AND mailbox_id=:mailbox"));
        complete.bindValue(QStringLiteral(":generation"), static_cast<qulonglong>(generation));
        complete.bindValue(QStringLiteral(":bytes"),
                           static_cast<qulonglong>(progress.completedBytes));
        complete.bindValue(QStringLiteral(":account"), QString::fromStdString(scope.accountId));
        complete.bindValue(QStringLiteral(":mailbox"), QString::fromStdString(scope.mailboxId));
        if (!complete.exec())
        {
            static_cast<void>(m_scheduler.update(scope.jobId, WorkStatus::Failed, progress,
                                                 QStringLiteral("{}"),
                                                 complete.lastError().text()));
            co_return;
        }
        QSqlQuery retention{m_connection.database()};
        retention.prepare(QStringLiteral(
            "UPDATE mail_vault_email_refs SET retention='full_sync' WHERE account_id=:account AND "
            "email_id IN (SELECT email_id FROM email_mailboxes WHERE account_id=:account "
            "AND mailbox_id=:mailbox)"));
        retention.bindValue(QStringLiteral(":account"), QString::fromStdString(scope.accountId));
        retention.bindValue(QStringLiteral(":mailbox"), QString::fromStdString(scope.mailboxId));
        if (!retention.exec())
            logDatabaseFailure(QStringLiteral("Retain synchronized mail"), retention);

        progress.detail = QStringLiteral("Available offline");
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

    std::optional<AccountConnectionSettings>
    FullMailSyncService::settingsFor(const std::string_view accountId) const
    {
        const auto found = m_settings.find(std::string{accountId});
        return found == m_settings.end() ? std::nullopt
                                         : std::optional<AccountConnectionSettings>{found->second};
    }
} // namespace javelin::app
