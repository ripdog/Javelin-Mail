#include "jmap/cache/RawMessageSourceRepository.h"

#include "jmap/cache/MailVault.h"

#include <QSqlError>
#include <QSqlQuery>

#include <algorithm>
#include <vector>

namespace javelin::jmap::cache
{

    namespace
    {

        [[nodiscard]] DatabaseError makeQueryError(const QString& operation, const QSqlQuery& query)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = operation + QStringLiteral(": ") + query.lastError().text(),
            };
        }

        [[nodiscard]] DatabaseError vaultError(const MailVaultError& error)
        {
            return {.code = DatabaseErrorCode::QueryFailed, .message = error.message};
        }

        struct ProjectionJob
        {
            qlonglong id = 0;
            std::string accountId;
            std::string emailId;
            std::string mailboxId;
            std::string contentHash;
            QString relativePath;
            std::uint64_t size = 0;
            std::string operation;
        };

    } // namespace

    RawMessageSourceRepository::RawMessageSourceRepository(DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    std::optional<DatabaseError>
    RawMessageSourceRepository::upsert(const std::string_view accountId,
                                       const RawMessageSource& source)
    {
        if (const auto error = m_connection.validate())
        {
            return error;
        }

        const MailVault vault = MailVault::forDatabase(m_connection);
        const auto installedResult = vault.install(source.payload);
        if (const auto* error = std::get_if<MailVaultError>(&installedResult))
        {
            return vaultError(*error);
        }
        const auto& installed = std::get<MailVaultObject>(installedResult);

        const DatabaseWriteScope writeScope{m_connection};
        auto& database = m_connection.database();
        if (!database.transaction())
        {
            return DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                 .message = QStringLiteral("Begin raw message source update: ") +
                                            database.lastError().text()};
        }

        QSqlQuery objectQuery{database};
        objectQuery.prepare(
            QStringLiteral("INSERT INTO mail_vault_objects(content_hash,relative_path,size) VALUES"
                           "(:hash,:path,:size) ON CONFLICT(content_hash) DO NOTHING"));
        objectQuery.bindValue(QStringLiteral(":hash"),
                              QString::fromStdString(installed.contentHash));
        objectQuery.bindValue(QStringLiteral(":path"), installed.relativePath);
        objectQuery.bindValue(QStringLiteral(":size"), static_cast<qulonglong>(installed.size));
        if (!objectQuery.exec())
        {
            database.rollback();
            return makeQueryError(QStringLiteral("Record mail vault object"), objectQuery);
        }

        QSqlQuery refQuery{database};
        refQuery.prepare(QStringLiteral(
            "INSERT INTO mail_vault_email_refs(account_id,email_id,blob_id,content_hash,retention) "
            "VALUES(:account_id,:email_id,:blob_id,:hash,'evictable') ON CONFLICT(account_id,"
            "email_id) DO UPDATE SET blob_id=excluded.blob_id,content_hash=excluded.content_hash,"
            "updated_at=CURRENT_TIMESTAMP"));
        refQuery.bindValue(QStringLiteral(":account_id"),
                           QString::fromStdString(std::string{accountId}));
        refQuery.bindValue(QStringLiteral(":email_id"), QString::fromStdString(source.emailId));
        refQuery.bindValue(QStringLiteral(":blob_id"), QString::fromStdString(source.blobId));
        refQuery.bindValue(QStringLiteral(":hash"), QString::fromStdString(installed.contentHash));
        if (!refQuery.exec())
        {
            database.rollback();
            return makeQueryError(QStringLiteral("Record mail vault email reference"), refQuery);
        }

        QSqlQuery projectionQuery{database};
        projectionQuery.prepare(QStringLiteral(
            "INSERT INTO mail_vault_projection_jobs(account_id,email_id,mailbox_id,content_hash,"
            "operation) SELECT account_id,email_id,mailbox_id,:hash,'link' FROM email_mailboxes "
            "WHERE account_id=:account_id AND email_id=:email_id"));
        projectionQuery.bindValue(QStringLiteral(":hash"),
                                  QString::fromStdString(installed.contentHash));
        projectionQuery.bindValue(QStringLiteral(":account_id"),
                                  QString::fromStdString(std::string{accountId}));
        projectionQuery.bindValue(QStringLiteral(":email_id"),
                                  QString::fromStdString(source.emailId));
        if (!projectionQuery.exec())
        {
            database.rollback();
            return makeQueryError(QStringLiteral("Queue mail vault projections"), projectionQuery);
        }

        if (!database.commit())
        {
            return DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                 .message = QStringLiteral("Commit raw message source update: ") +
                                            database.lastError().text()};
        }
        return replayProjectionJobs();
    }

    std::optional<DatabaseError>
    RawMessageSourceRepository::remove(const std::string_view accountId,
                                       const std::string_view emailId)
    {
        if (const auto error = m_connection.validate())
        {
            return error;
        }

        const DatabaseWriteScope writeScope{m_connection};
        auto& database = m_connection.database();
        if (!database.transaction())
        {
            return DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                 .message = QStringLiteral("Begin raw message source removal: ") +
                                            database.lastError().text()};
        }
        QSqlQuery jobQuery{database};
        jobQuery.prepare(QStringLiteral(
            "INSERT INTO mail_vault_projection_jobs(account_id,email_id,mailbox_id,operation) "
            "SELECT account_id,email_id,mailbox_id,'unlink' FROM email_mailboxes WHERE "
            "account_id=:account_id AND email_id=:email_id"));
        jobQuery.bindValue(QStringLiteral(":account_id"),
                           QString::fromStdString(std::string{accountId}));
        jobQuery.bindValue(QStringLiteral(":email_id"),
                           QString::fromStdString(std::string{emailId}));
        if (!jobQuery.exec())
        {
            database.rollback();
            return makeQueryError(QStringLiteral("Queue mail vault projection removal"), jobQuery);
        }

        QSqlQuery query{database};
        query.prepare(QStringLiteral(
            "DELETE FROM mail_vault_email_refs WHERE account_id = :account_id AND email_id = "
            ":email_id"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":email_id"), QString::fromStdString(std::string{emailId}));
        if (!query.exec())
        {
            database.rollback();
            return makeQueryError(QStringLiteral("Delete mail vault email reference"), query);
        }

        if (!database.commit())
        {
            return DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                 .message = QStringLiteral("Commit raw message source removal: ") +
                                            database.lastError().text()};
        }

        return replayProjectionJobs();
    }

    std::variant<std::optional<RawMessageSource>, DatabaseError>
    RawMessageSourceRepository::find(const std::string_view accountId,
                                     const std::string_view emailId) const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT r.blob_id,o.content_hash,o.relative_path,o.size FROM mail_vault_email_refs r "
            "JOIN mail_vault_objects o ON o.content_hash=r.content_hash WHERE "
            "r.account_id=:account_id AND r.email_id=:email_id"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":email_id"), QString::fromStdString(std::string{emailId}));
        if (!query.exec())
        {
            return makeQueryError(QStringLiteral("Read raw message source"), query);
        }

        if (query.next())
        {
            const MailVaultObject object{
                .contentHash = query.value(1).toString().toStdString(),
                .relativePath = query.value(2).toString(),
                .size = query.value(3).toULongLong(),
            };
            const auto payloadResult = MailVault::forDatabase(m_connection).read(object);
            if (const auto* error = std::get_if<MailVaultError>(&payloadResult))
            {
                return vaultError(*error);
            }
            return std::optional<RawMessageSource>{RawMessageSource{
                .emailId = std::string{emailId},
                .blobId = query.value(0).toString().toStdString(),
                .payload = std::get<QByteArray>(payloadResult),
            }};
        }

        QSqlQuery migrationQuery{m_connection.database()};
        migrationQuery.prepare(
            QStringLiteral("SELECT status FROM local_data_migrations WHERE migration_key="
                           "'raw_message_sources_to_vault'"));
        if (!migrationQuery.exec() || !migrationQuery.next())
        {
            return makeQueryError(QStringLiteral("Read raw source migration state"),
                                  migrationQuery);
        }
        if (migrationQuery.value(0).toString() == QStringLiteral("complete"))
        {
            return std::optional<RawMessageSource>{std::nullopt};
        }

        QSqlQuery legacyQuery{m_connection.database()};
        legacyQuery.prepare(QStringLiteral(
            "SELECT blob_id,payload FROM raw_message_sources WHERE account_id=:account_id AND "
            "email_id=:email_id"));
        legacyQuery.bindValue(QStringLiteral(":account_id"),
                              QString::fromStdString(std::string{accountId}));
        legacyQuery.bindValue(QStringLiteral(":email_id"),
                              QString::fromStdString(std::string{emailId}));
        if (!legacyQuery.exec())
            return makeQueryError(QStringLiteral("Read legacy raw message source"), legacyQuery);
        if (!legacyQuery.next())
            return std::optional<RawMessageSource>{std::nullopt};
        return std::optional<RawMessageSource>{RawMessageSource{
            .emailId = std::string{emailId},
            .blobId = legacyQuery.value(0).toString().toStdString(),
            .payload = legacyQuery.value(1).toByteArray(),
        }};
    }

    std::variant<std::size_t, DatabaseError>
    RawMessageSourceRepository::migrateLegacySources(const std::size_t limit)
    {
        QSqlQuery select{m_connection.database()};
        select.prepare(QStringLiteral(
            "SELECT s.account_id,s.email_id,s.blob_id,s.payload FROM raw_message_sources s LEFT "
            "JOIN mail_vault_email_refs r ON r.account_id=s.account_id AND r.email_id=s.email_id "
            "WHERE r.email_id IS NULL ORDER BY s.account_id,s.email_id LIMIT :limit"));
        select.bindValue(QStringLiteral(":limit"), static_cast<qulonglong>(limit));
        if (!select.exec())
            return makeQueryError(QStringLiteral("Read legacy raw sources for migration"), select);

        std::vector<std::pair<std::string, RawMessageSource>> sources;
        while (select.next())
        {
            sources.emplace_back(
                select.value(0).toString().toStdString(),
                RawMessageSource{.emailId = select.value(1).toString().toStdString(),
                                 .blobId = select.value(2).toString().toStdString(),
                                 .payload = select.value(3).toByteArray()});
        }
        select.finish();
        for (const auto& [accountId, source] : sources)
        {
            if (const auto failure = upsert(accountId, source))
                return *failure;
        }

        QSqlQuery remaining{m_connection.database()};
        if (!remaining.exec(QStringLiteral(
                "SELECT EXISTS(SELECT 1 FROM raw_message_sources s LEFT JOIN mail_vault_email_refs "
                "r ON r.account_id=s.account_id AND r.email_id=s.email_id WHERE r.email_id IS "
                "NULL)")))
            return makeQueryError(QStringLiteral("Check legacy raw source migration"), remaining);
        remaining.next();
        const bool hasRemainingSources = remaining.value(0).toBool();
        remaining.finish();
        if (!hasRemainingSources)
        {
            auto transactionResult = DatabaseTransaction::begin(
                m_connection, QStringLiteral("Complete raw message source migration"));
            if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
                return *error;
            auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));
            QSqlQuery clearLegacy{m_connection.database()};
            if (!clearLegacy.exec(QStringLiteral("DELETE FROM raw_message_sources")))
                return makeQueryError(QStringLiteral("Clear migrated raw message sources"),
                                      clearLegacy);
            QSqlQuery complete{m_connection.database()};
            if (!complete.exec(QStringLiteral(
                    "UPDATE local_data_migrations SET status='complete',checkpoint=NULL,"
                    "latest_error=NULL,updated_at=CURRENT_TIMESTAMP WHERE migration_key="
                    "'raw_message_sources_to_vault'")))
                return makeQueryError(QStringLiteral("Complete raw source migration"), complete);
            if (const auto error = transaction.commit())
                return *error;
        }
        return sources.size();
    }

    std::optional<DatabaseError>
    RawMessageSourceRepository::replayProjectionJobs(const std::size_t limit)
    {
        QSqlQuery select{m_connection.database()};
        select.prepare(
            QStringLiteral("SELECT j.job_id,j.account_id,j.email_id,COALESCE(j.mailbox_id,''),"
                           "COALESCE(j.content_hash,''),COALESCE(o.relative_path,''),COALESCE(o."
                           "size,0),j.operation "
                           "FROM mail_vault_projection_jobs j LEFT JOIN mail_vault_objects o ON "
                           "o.content_hash=j.content_hash WHERE j.status='pending' ORDER BY "
                           "j.job_id LIMIT :limit"));
        select.bindValue(QStringLiteral(":limit"), static_cast<qulonglong>(limit));
        if (!select.exec())
            return makeQueryError(QStringLiteral("Read mail vault projection jobs"), select);
        std::vector<ProjectionJob> jobs;
        while (select.next())
        {
            jobs.push_back({.id = select.value(0).toLongLong(),
                            .accountId = select.value(1).toString().toStdString(),
                            .emailId = select.value(2).toString().toStdString(),
                            .mailboxId = select.value(3).toString().toStdString(),
                            .contentHash = select.value(4).toString().toStdString(),
                            .relativePath = select.value(5).toString(),
                            .size = select.value(6).toULongLong(),
                            .operation = select.value(7).toString().toStdString()});
        }
        select.finish();

        const MailVault vault = MailVault::forDatabase(m_connection);
        for (const auto& job : jobs)
        {
            std::optional<MailVaultError> failure;
            if (job.operation == "link")
            {
                failure = vault.project(job.accountId, job.mailboxId, job.emailId,
                                        {.contentHash = job.contentHash,
                                         .relativePath = job.relativePath,
                                         .size = job.size});
            }
            else if (job.operation == "unlink")
            {
                failure = vault.removeProjection(job.accountId, job.mailboxId, job.emailId);
            }
            else if (job.operation == "metadata")
            {
                QSqlQuery metadata{m_connection.database()};
                if (job.mailboxId.empty())
                {
                    metadata.prepare(QStringLiteral(
                        "SELECT email_address FROM accounts WHERE account_id=:account"));
                    metadata.bindValue(QStringLiteral(":account"),
                                       QString::fromStdString(job.accountId));
                    if (!metadata.exec() || !metadata.next())
                        failure = MailVaultError{
                            .message = QStringLiteral("Read mail account metadata: ") +
                                       metadata.lastError().text()};
                    else
                        failure = vault.writeAccountMetadata(
                            job.accountId, metadata.value(0).toString().toStdString());
                }
                else
                {
                    metadata.prepare(
                        QStringLiteral("SELECT name FROM mailboxes WHERE account_id=:account AND "
                                       "mailbox_id=:mailbox"));
                    metadata.bindValue(QStringLiteral(":account"),
                                       QString::fromStdString(job.accountId));
                    metadata.bindValue(QStringLiteral(":mailbox"),
                                       QString::fromStdString(job.mailboxId));
                    if (!metadata.exec() || !metadata.next())
                        failure = MailVaultError{
                            .message = QStringLiteral("Read mail mailbox metadata: ") +
                                       metadata.lastError().text()};
                    else
                        failure =
                            vault.writeMailboxMetadata(job.accountId, job.mailboxId,
                                                       metadata.value(0).toString().toStdString());
                }
            }
            else
            {
                failure = MailVaultError{
                    .message = QStringLiteral("Replay mail vault projection: unsupported operation "
                                              "%1")
                                   .arg(QString::fromStdString(job.operation))};
            }
            const DatabaseWriteScope writeScope{m_connection};
            QSqlQuery update{m_connection.database()};
            update.prepare(QStringLiteral(
                "UPDATE mail_vault_projection_jobs SET status=:status,last_error=:error,"
                "updated_at=CURRENT_TIMESTAMP WHERE job_id=:job_id"));
            update.bindValue(QStringLiteral(":status"),
                             failure ? QStringLiteral("pending") : QStringLiteral("complete"));
            update.bindValue(QStringLiteral(":error"),
                             failure ? QVariant{failure->message} : QVariant{});
            update.bindValue(QStringLiteral(":job_id"), job.id);
            if (!update.exec())
                return makeQueryError(QStringLiteral("Complete mail vault projection job"), update);
            if (failure)
                return vaultError(*failure);
        }
        return std::nullopt;
    }

} // namespace javelin::jmap::cache
