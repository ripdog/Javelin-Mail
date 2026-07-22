#include "app/MailIndexService.h"

#include "app/WorkScheduler.h"
#include "jmap/cache/MailSearchIndex.h"
#include "jmap/cache/MailVault.h"
#include "jmap/cache/MimeMessageParser.h"

#include <QCoroFuture>
#include <QCoroTask>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QTextDocument>
#include <QTimer>
#include <QtConcurrentRun>

#include <optional>

namespace javelin::app
{
    namespace
    {
        struct ParsedIndexDocument
        {
            std::optional<javelin::jmap::cache::SearchIndexDocument> document;
            QString preview;
            QString error;
        };

        [[nodiscard]] std::string indexJobId(const std::string_view accountId)
        {
            return "mail-index-" +
                   QCryptographicHash::hash(QByteArray::fromStdString(std::string{accountId}),
                                            QCryptographicHash::Sha256)
                       .toHex()
                       .toStdString();
        }

        [[nodiscard]] ParsedIndexDocument parseDocument(QString path, std::string emailId,
                                                        std::string contentHash, QString subject)
        {
            QFile file{path};
            if (!file.open(QIODevice::ReadOnly))
                return {.document = std::nullopt, .preview = {}, .error = file.errorString()};
            const QByteArray payload = file.readAll();
            if (file.error() != QFileDevice::NoError)
                return {.document = std::nullopt, .preview = {}, .error = file.errorString()};
            const auto parsed = javelin::jmap::cache::parseMessageSource(emailId, payload);
            QString body;
            if (parsed.plainTextBody)
                body += QString::fromStdString(parsed.plainTextBody->value);
            else if (parsed.htmlBody)
            {
                QTextDocument document;
                document.setHtml(QString::fromStdString(parsed.htmlBody->value));
                body += document.toPlainText();
            }
            const QString preview = body.simplified().left(256);
            return {.document =
                        javelin::jmap::cache::SearchIndexDocument{
                            .emailId = std::move(emailId),
                            .sourceHash = std::move(contentHash),
                            .subject = std::move(subject),
                            .body = std::move(body),
                        },
                    .preview = preview,
                    .error = {}};
        }

        [[nodiscard]] QString
        commitIndexDocument(const QString& databasePath, const std::string& accountId,
                            javelin::jmap::cache::SearchIndexDocument document, QString preview)
        {
            const javelin::jmap::cache::SerializedDatabaseWrite writeGuard;
            const std::string emailId = document.emailId;
            const std::string contentHash = document.sourceHash;
            javelin::jmap::cache::ThreadConnectionFactory factory({
                .connectionNamePrefix = QStringLiteral("mail-index-cache"),
                .databasePath = databasePath,
            });
            auto opened = factory.openForCurrentThread(accountId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&opened))
                return error->message;
            auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
            javelin::jmap::cache::MailSearchIndex index{connection};
            if (const auto error = index.upsert(accountId, document))
                return error->message;

            auto& database = connection.database();
            if (!database.transaction())
                return database.lastError().text();
            QSqlQuery updatePreview{database};
            updatePreview.prepare(QStringLiteral(
                "UPDATE emails SET preview=:preview WHERE account_id=:account AND email_id=:email "
                "AND preview=''"));
            updatePreview.bindValue(QStringLiteral(":preview"), preview);
            updatePreview.bindValue(QStringLiteral(":account"), QString::fromStdString(accountId));
            updatePreview.bindValue(QStringLiteral(":email"), QString::fromStdString(emailId));
            if (!updatePreview.exec())
            {
                const QString error = updatePreview.lastError().text();
                database.rollback();
                return error;
            }
            QSqlQuery indexed{database};
            indexed.prepare(QStringLiteral(
                "UPDATE mail_vault_email_refs SET indexed_hash=:hash WHERE account_id=:account "
                "AND email_id=:email AND content_hash=:hash"));
            indexed.bindValue(QStringLiteral(":hash"), QString::fromStdString(contentHash));
            indexed.bindValue(QStringLiteral(":account"), QString::fromStdString(accountId));
            indexed.bindValue(QStringLiteral(":email"), QString::fromStdString(emailId));
            if (!indexed.exec())
            {
                const QString error = indexed.lastError().text();
                database.rollback();
                return error;
            }
            if (!database.commit())
                return database.lastError().text();
            return {};
        }
    } // namespace

    MailIndexService::MailIndexService(javelin::jmap::cache::DatabaseConnection& connection,
                                       WorkScheduler& scheduler, QObject* parent)
        : QObject(parent), m_connection(connection), m_scheduler(scheduler)
    {
        connect(&m_scheduler, &WorkScheduler::jobsChanged, this, [this]() { schedulePump(); });
        connect(&m_scheduler, &WorkScheduler::foregroundAvailabilityChanged, this,
                [this]() { schedulePump(); });
    }

    void MailIndexService::applyAccounts(std::vector<std::string> accountIds)
    {
        m_jobs.clear();
        for (auto& accountId : accountIds)
        {
            const auto id = indexJobId(accountId);
            m_jobs.insert_or_assign(id, accountId);
            QSqlQuery pending{m_connection.database()};
            pending.prepare(QStringLiteral(
                "SELECT EXISTS(SELECT 1 FROM mail_vault_email_refs WHERE account_id=:account AND "
                "(indexed_hash IS NULL OR indexed_hash<>content_hash))"));
            pending.bindValue(QStringLiteral(":account"), QString::fromStdString(accountId));
            if (!pending.exec() || !pending.next())
                continue;
            const bool hasPendingIndexWork = pending.value(0).toBool();
            pending.finish();
            if (!hasPendingIndexWork)
                continue;
            static_cast<void>(m_scheduler.ensure({
                .jobId = id,
                .parentJobId = std::nullopt,
                .accountId = accountId,
                .kind = WorkKind::SearchIndex,
                .priority = WorkPriority::Derived,
                .title = QStringLiteral("Index downloaded mail"),
                .checkpointJson = QStringLiteral("{}"),
            }));
        }
        schedulePump();
    }

    void MailIndexService::requestIndex(const std::string_view accountId)
    {
        const auto id = indexJobId(accountId);
        m_jobs.insert_or_assign(id, std::string{accountId});
        static_cast<void>(m_scheduler.ensure({
            .jobId = id,
            .parentJobId = std::nullopt,
            .accountId = std::string{accountId},
            .kind = WorkKind::SearchIndex,
            .priority = WorkPriority::Derived,
            .title = QStringLiteral("Index downloaded mail"),
            .checkpointJson = QStringLiteral("{}"),
        }));
        QSqlQuery queue{m_connection.database()};
        queue.prepare(QStringLiteral(
            "UPDATE background_jobs SET status='queued',updated_at=CURRENT_TIMESTAMP WHERE "
            "job_id=:id AND status='complete'"));
        queue.bindValue(QStringLiteral(":id"), QString::fromStdString(id));
        static_cast<void>(queue.exec());
        schedulePump();
    }

    void MailIndexService::schedulePump()
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

    void MailIndexService::pump()
    {
        if (!m_scheduler.mayStartBackgroundNetwork())
            return;
        const auto records = m_scheduler.list();
        const auto* jobs = std::get_if<std::vector<WorkRecord>>(&records);
        if (jobs == nullptr)
            return;
        for (const auto& job : *jobs)
        {
            if (job.kind != WorkKind::SearchIndex || job.status != WorkStatus::Queued)
                continue;
            const auto account = m_jobs.find(job.jobId);
            if (account == m_jobs.end() || m_runningAccounts.contains(account->second))
                continue;
            m_runningAccounts.insert(account->second);
            auto task = runAccount(account->second, job.jobId);
            QCoro::connect(std::move(task), this,
                           [this, accountId = account->second]()
                           {
                               m_runningAccounts.erase(accountId);
                               schedulePump();
                           });
        }
    }

    QCoro::Task<void> MailIndexService::runAccount(std::string accountId, std::string jobId)
    {
        QSqlQuery totalQuery{m_connection.database()};
        totalQuery.prepare(QStringLiteral(
            "SELECT COUNT(*) FROM mail_vault_email_refs WHERE account_id=:account AND "
            "(indexed_hash IS NULL OR indexed_hash<>content_hash)"));
        totalQuery.bindValue(QStringLiteral(":account"), QString::fromStdString(accountId));
        if (!totalQuery.exec() || !totalQuery.next())
            co_return;
        const std::uint64_t total = totalQuery.value(0).toULongLong();
        totalQuery.finish();
        WorkProgress progress{.completedUnits = 0,
                              .totalUnits = total,
                              .completedBytes = 0,
                              .totalBytes = std::nullopt,
                              .detail = QStringLiteral("Preparing local search")};
        static_cast<void>(m_scheduler.update(jobId, WorkStatus::Running, progress));
        const auto vault = javelin::jmap::cache::MailVault::forDatabase(m_connection);
        while (true)
        {
            const auto control = m_scheduler.find(jobId);
            const auto* active = std::get_if<std::optional<WorkRecord>>(&control);
            if (active == nullptr || !active->has_value() || (*active)->pauseRequested)
            {
                static_cast<void>(m_scheduler.pause(jobId));
                co_return;
            }
            if (!m_scheduler.mayStartBackgroundNetwork())
            {
                static_cast<void>(m_scheduler.update(jobId, WorkStatus::Queued, progress));
                co_return;
            }

            QSqlQuery next{m_connection.database()};
            next.prepare(QStringLiteral(
                "SELECT r.email_id,r.content_hash,o.relative_path,e.subject,o.size FROM "
                "mail_vault_email_refs r JOIN mail_vault_objects o ON "
                "o.content_hash=r.content_hash "
                "JOIN emails e ON e.account_id=r.account_id AND e.email_id=r.email_id WHERE "
                "r.account_id=:account AND (r.indexed_hash IS NULL OR "
                "r.indexed_hash<>r.content_hash) ORDER BY r.updated_at LIMIT 1"));
            next.bindValue(QStringLiteral(":account"), QString::fromStdString(accountId));
            if (!next.exec() || !next.next())
                break;
            const auto emailId = next.value(0).toString().toStdString();
            const auto contentHash = next.value(1).toString().toStdString();
            const QString path = QDir(vault.rootPath()).filePath(next.value(2).toString());
            const QString subject = next.value(3).toString();
            const std::uint64_t size = next.value(4).toULongLong();
            next.finish();

            auto future = QtConcurrent::run(parseDocument, path, emailId, contentHash, subject);
            auto parsed = co_await qCoro(future).takeResult();
            if (!parsed.document)
            {
                static_cast<void>(m_scheduler.update(jobId, WorkStatus::Failed, progress,
                                                     QStringLiteral("{}"), parsed.error));
                co_return;
            }
            auto commitFuture = QtConcurrent::run(
                commitIndexDocument, m_connection.database().databaseName(), accountId,
                std::move(*parsed.document), std::move(parsed.preview));
            const QString commitError = co_await qCoro(commitFuture).takeResult();
            if (!commitError.isEmpty())
            {
                static_cast<void>(m_scheduler.update(jobId, WorkStatus::Failed, progress,
                                                     QStringLiteral("{}"), commitError));
                co_return;
            }
            ++progress.completedUnits;
            progress.completedBytes += size;
            progress.detail = QStringLiteral("Indexing downloaded mail");
            static_cast<void>(m_scheduler.update(jobId, WorkStatus::Running, progress));
        }
        progress.detail = QStringLiteral("Search index is current");
        static_cast<void>(m_scheduler.update(jobId, WorkStatus::Complete, progress));
    }
} // namespace javelin::app
