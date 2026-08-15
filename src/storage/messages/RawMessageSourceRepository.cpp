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

        struct VaultProjection
        {
            std::string accountId;
            std::string emailId;
            std::string mailboxId;
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
        return upsertInstalled(accountId, source.emailId, source.blobId,
                               std::get<MailVaultObject>(installedResult));
    }

    std::optional<DatabaseError> RawMessageSourceRepository::upsertInstalled(
        const std::string_view accountId, const std::string_view emailId,
        const std::string_view blobId, const MailVaultObject& installed)
    {
        const auto result = upsertInstalledImpl(accountId, emailId, blobId, installed, false);
        if (const auto* error = std::get_if<DatabaseError>(&result))
            return *error;
        return std::nullopt;
    }

    std::variant<bool, DatabaseError> RawMessageSourceRepository::upsertInstalledIfCurrent(
        const std::string_view accountId, const std::string_view emailId,
        const std::string_view blobId, const MailVaultObject& installed)
    {
        return upsertInstalledImpl(accountId, emailId, blobId, installed, true);
    }

    std::variant<bool, DatabaseError> RawMessageSourceRepository::upsertInstalledImpl(
        const std::string_view accountId, const std::string_view emailId,
        const std::string_view blobId, const MailVaultObject& installed,
        const bool requireCurrentEmail)
    {
        if (const auto error = m_connection.validate())
            return *error;

        {
            auto transactionResult = DatabaseTransaction::begin(
                m_connection, QStringLiteral("Update raw message source"));
            if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
                return *error;
            auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));
            auto& database = m_connection.database();

            QSqlQuery objectQuery{database};
            objectQuery.prepare(QStringLiteral(
                "INSERT INTO mail_vault_objects(content_hash,relative_path,size) VALUES"
                "(:hash,:path,:size) ON CONFLICT(content_hash) DO NOTHING"));
            objectQuery.bindValue(QStringLiteral(":hash"),
                                  QString::fromStdString(installed.contentHash));
            objectQuery.bindValue(QStringLiteral(":path"), installed.relativePath);
            objectQuery.bindValue(QStringLiteral(":size"), static_cast<qulonglong>(installed.size));
            if (!objectQuery.exec())
            {
                transaction.rollback();
                return makeQueryError(QStringLiteral("Record mail vault object"), objectQuery);
            }

            if (requireCurrentEmail)
            {
                QSqlQuery currentEmail{database};
                currentEmail.prepare(QStringLiteral(
                    "SELECT EXISTS(SELECT 1 FROM emails WHERE account_id=:account_id "
                    "AND email_id=:email_id AND blob_id=:blob_id)"));
                currentEmail.bindValue(QStringLiteral(":account_id"),
                                       QString::fromStdString(std::string{accountId}));
                currentEmail.bindValue(QStringLiteral(":email_id"),
                                       QString::fromStdString(std::string{emailId}));
                currentEmail.bindValue(QStringLiteral(":blob_id"),
                                       QString::fromStdString(std::string{blobId}));
                if (!currentEmail.exec() || !currentEmail.next())
                {
                    transaction.rollback();
                    return makeQueryError(QStringLiteral("Validate downloaded message source"),
                                          currentEmail);
                }
                if (currentEmail.value(0).toInt() == 0)
                {
                    if (const auto error = transaction.commit())
                        return *error;
                    return false;
                }
            }

            QSqlQuery refQuery{database};
            refQuery.prepare(QStringLiteral(
                "INSERT INTO "
                "mail_vault_email_refs(account_id,email_id,blob_id,content_hash,retention) "
                "VALUES(:account_id,:email_id,:blob_id,:hash,'evictable') ON CONFLICT(account_id,"
                "email_id) DO UPDATE SET "
                "blob_id=excluded.blob_id,content_hash=excluded.content_hash,"
                "updated_at=CURRENT_TIMESTAMP"));
            refQuery.bindValue(QStringLiteral(":account_id"),
                               QString::fromStdString(std::string{accountId}));
            refQuery.bindValue(QStringLiteral(":email_id"),
                               QString::fromStdString(std::string{emailId}));
            refQuery.bindValue(QStringLiteral(":blob_id"),
                               QString::fromStdString(std::string{blobId}));
            refQuery.bindValue(QStringLiteral(":hash"),
                               QString::fromStdString(installed.contentHash));
            if (!refQuery.exec())
            {
                transaction.rollback();
                return makeQueryError(QStringLiteral("Record mail vault email reference"),
                                      refQuery);
            }

            QSqlQuery mailboxRefs{database};
            mailboxRefs.prepare(QStringLiteral(
                "INSERT OR IGNORE INTO mail_vault_mailbox_refs(account_id,email_id,mailbox_id) "
                "SELECT account_id,email_id,mailbox_id FROM email_mailboxes WHERE "
                "account_id=:account_id AND email_id=:email_id"));
            mailboxRefs.bindValue(QStringLiteral(":account_id"),
                                  QString::fromStdString(std::string{accountId}));
            mailboxRefs.bindValue(QStringLiteral(":email_id"),
                                  QString::fromStdString(std::string{emailId}));
            if (!mailboxRefs.exec())
            {
                transaction.rollback();
                return makeQueryError(QStringLiteral("Record mail vault mailbox references"),
                                      mailboxRefs);
            }

            QSqlQuery projectionQuery{database};
            projectionQuery.prepare(QStringLiteral(
                "INSERT INTO "
                "mail_vault_projection_jobs(account_id,email_id,mailbox_id,content_hash,"
                "operation) SELECT account_id,email_id,mailbox_id,:hash,'link' FROM "
                "email_mailboxes "
                "WHERE account_id=:account_id AND email_id=:email_id"));
            projectionQuery.bindValue(QStringLiteral(":hash"),
                                      QString::fromStdString(installed.contentHash));
            projectionQuery.bindValue(QStringLiteral(":account_id"),
                                      QString::fromStdString(std::string{accountId}));
            projectionQuery.bindValue(QStringLiteral(":email_id"),
                                      QString::fromStdString(std::string{emailId}));
            if (!projectionQuery.exec())
            {
                transaction.rollback();
                return makeQueryError(QStringLiteral("Queue mail vault projections"),
                                      projectionQuery);
            }

            if (const auto error = transaction.commit())
                return *error;
        }
        static_cast<void>(replayProjectionJobs());
        return true;
    }

    std::optional<DatabaseError>
    RawMessageSourceRepository::remove(const std::string_view accountId,
                                       const std::string_view emailId)
    {
        if (const auto error = m_connection.validate())
        {
            return error;
        }

        {
            auto transactionResult = DatabaseTransaction::begin(
                m_connection, QStringLiteral("Remove raw message source"));
            if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
                return *error;
            auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));
            auto& database = m_connection.database();
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
                transaction.rollback();
                return makeQueryError(QStringLiteral("Queue mail vault projection removal"),
                                      jobQuery);
            }

            QSqlQuery query{database};
            query.prepare(QStringLiteral(
                "DELETE FROM mail_vault_email_refs WHERE account_id = :account_id AND email_id = "
                ":email_id"));
            query.bindValue(QStringLiteral(":account_id"),
                            QString::fromStdString(std::string{accountId}));
            query.bindValue(QStringLiteral(":email_id"),
                            QString::fromStdString(std::string{emailId}));
            if (!query.exec())
            {
                transaction.rollback();
                return makeQueryError(QStringLiteral("Delete mail vault email reference"), query);
            }

            if (const auto error = transaction.commit())
                return *error;
        }

        static_cast<void>(replayProjectionJobs());
        return std::nullopt;
    }

    std::variant<std::optional<std::string>, DatabaseError>
    RawMessageSourceRepository::findBlobId(const std::string_view accountId,
                                           const std::string_view emailId) const
    {
        if (const auto error = m_connection.validate())
            return *error;

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT blob_id FROM mail_vault_email_refs WHERE account_id=:account_id AND "
            "email_id=:email_id"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":email_id"), QString::fromStdString(std::string{emailId}));
        if (!query.exec())
            return makeQueryError(QStringLiteral("Read raw message source reference"), query);
        if (query.next())
            return std::optional<std::string>{query.value(0).toString().toStdString()};
        query.finish();

        QSqlQuery legacyQuery{m_connection.database()};
        legacyQuery.prepare(QStringLiteral(
            "SELECT blob_id FROM raw_message_sources WHERE account_id=:account_id AND "
            "email_id=:email_id"));
        legacyQuery.bindValue(QStringLiteral(":account_id"),
                              QString::fromStdString(std::string{accountId}));
        legacyQuery.bindValue(QStringLiteral(":email_id"),
                              QString::fromStdString(std::string{emailId}));
        if (!legacyQuery.exec())
            return makeQueryError(QStringLiteral("Read legacy raw source reference"), legacyQuery);
        if (!legacyQuery.next())
            return std::optional<std::string>{std::nullopt};
        return std::optional<std::string>{legacyQuery.value(0).toString().toStdString()};
    }

    std::variant<std::optional<RawMessageSourceReference>, DatabaseError>
    RawMessageSourceRepository::findReference(const std::string_view accountId,
                                              const std::string_view emailId) const
    {
        if (const auto error = m_connection.validate())
            return *error;

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT r.blob_id,o.content_hash,o.relative_path,o.size FROM mail_vault_email_refs r "
            "JOIN mail_vault_objects o ON o.content_hash=r.content_hash WHERE "
            "r.account_id=:account_id AND r.email_id=:email_id"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":email_id"), QString::fromStdString(std::string{emailId}));
        if (!query.exec())
            return makeQueryError(QStringLiteral("Read raw message source reference"), query);
        if (!query.next())
            return std::optional<RawMessageSourceReference>{};

        return std::optional<RawMessageSourceReference>{RawMessageSourceReference{
            .emailId = std::string{emailId},
            .blobId = query.value(0).toString().toStdString(),
            .object =
                {
                    .contentHash = query.value(1).toString().toStdString(),
                    .relativePath = query.value(2).toString(),
                    .size = query.value(3).toULongLong(),
                },
        }};
    }

    std::variant<std::optional<MailVaultObject>, DatabaseError>
    RawMessageSourceRepository::findVaultObject(const std::string_view contentHash) const
    {
        if (const auto error = m_connection.validate())
            return *error;

        QSqlQuery query{m_connection.database()};
        query.prepare(
            QStringLiteral("SELECT content_hash,relative_path,size FROM mail_vault_objects WHERE "
                           "content_hash=:content_hash"));
        query.bindValue(QStringLiteral(":content_hash"),
                        QString::fromStdString(std::string{contentHash}));
        if (!query.exec())
            return makeQueryError(QStringLiteral("Read mail vault object"), query);
        if (!query.next())
            return std::optional<MailVaultObject>{};
        return std::optional<MailVaultObject>{MailVaultObject{
            .contentHash = query.value(0).toString().toStdString(),
            .relativePath = query.value(1).toString(),
            .size = query.value(2).toULongLong(),
        }};
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
    RawMessageSourceRepository::evictUnretained(const std::size_t limit)
    {
        if (const auto error = m_connection.validate())
            return *error;

        QSqlQuery select{m_connection.database()};
        select.prepare(
            QStringLiteral("SELECT o.content_hash,o.relative_path,o.size FROM mail_vault_objects o "
                           "WHERE NOT EXISTS(SELECT 1 FROM mail_vault_projection_jobs p WHERE "
                           "p.content_hash=o.content_hash AND p.status='pending') AND "
                           "NOT EXISTS(SELECT 1 FROM mail_vault_pins pin WHERE "
                           "pin.content_hash=o.content_hash) AND "
                           "(NOT EXISTS(SELECT 1 FROM mail_vault_email_refs r WHERE "
                           "r.content_hash=o.content_hash) "
                           "OR NOT EXISTS(SELECT 1 FROM mail_vault_email_refs r WHERE "
                           "r.content_hash=o.content_hash "
                           "AND (r.retention<>'evictable' OR r.indexed_hash IS NULL OR "
                           "r.indexed_hash<>r.content_hash))) "
                           "ORDER BY o.content_hash LIMIT :limit"));
        select.bindValue(QStringLiteral(":limit"), static_cast<qulonglong>(limit));
        if (!select.exec())
            return makeQueryError(QStringLiteral("List evictable mail vault objects"), select);

        std::vector<MailVaultObject> objects;
        while (select.next())
        {
            objects.push_back({
                .contentHash = select.value(0).toString().toStdString(),
                .relativePath = select.value(1).toString(),
                .size = select.value(2).toULongLong(),
            });
        }
        select.finish();

        const MailVault vault = MailVault::forDatabase(m_connection);
        std::size_t evicted = 0;
        for (const auto& object : objects)
        {
            if (vault.isLeased(object))
                continue;

            QSqlQuery projectionQuery{m_connection.database()};
            projectionQuery.prepare(QStringLiteral(
                "SELECT account_id,email_id,mailbox_id FROM mail_vault_projection_jobs WHERE "
                "content_hash=:content_hash AND operation='link' AND mailbox_id IS NOT NULL "
                "UNION SELECT mr.account_id,mr.email_id,mr.mailbox_id FROM "
                "mail_vault_mailbox_refs mr JOIN mail_vault_email_refs r ON "
                "r.account_id=mr.account_id AND r.email_id=mr.email_id WHERE "
                "r.content_hash=:content_hash"));
            projectionQuery.bindValue(QStringLiteral(":content_hash"),
                                      QString::fromStdString(object.contentHash));
            if (!projectionQuery.exec())
                return makeQueryError(QStringLiteral("List mail vault projections"),
                                      projectionQuery);

            std::vector<VaultProjection> projections;
            while (projectionQuery.next())
            {
                projections.push_back(
                    {.accountId = projectionQuery.value(0).toString().toStdString(),
                     .emailId = projectionQuery.value(1).toString().toStdString(),
                     .mailboxId = projectionQuery.value(2).toString().toStdString()});
            }
            projectionQuery.finish();
            for (const auto& projection : projections)
            {
                if (const auto error = vault.removeProjection(
                        projection.accountId, projection.mailboxId, projection.emailId))
                    return vaultError(*error);
            }
            if (vault.evict(object).has_value())
                continue;
            auto transactionResult =
                DatabaseTransaction::begin(m_connection, QStringLiteral("Evict mail vault object"));
            if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
                return *error;
            auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));
            QSqlQuery refs{m_connection.database()};
            refs.prepare(QStringLiteral(
                "DELETE FROM mail_vault_email_refs WHERE content_hash=:content_hash"));
            refs.bindValue(QStringLiteral(":content_hash"),
                           QString::fromStdString(object.contentHash));
            if (!refs.exec())
            {
                transaction.rollback();
                return makeQueryError(QStringLiteral("Delete evicted mail vault references"), refs);
            }
            QSqlQuery objectRow{m_connection.database()};
            objectRow.prepare(
                QStringLiteral("DELETE FROM mail_vault_objects WHERE content_hash=:content_hash"));
            objectRow.bindValue(QStringLiteral(":content_hash"),
                                QString::fromStdString(object.contentHash));
            if (!objectRow.exec())
            {
                transaction.rollback();
                return makeQueryError(QStringLiteral("Delete evicted mail vault object"),
                                      objectRow);
            }
            QSqlQuery jobs{m_connection.database()};
            jobs.prepare(QStringLiteral(
                "DELETE FROM mail_vault_projection_jobs WHERE content_hash=:content_hash"));
            jobs.bindValue(QStringLiteral(":content_hash"),
                           QString::fromStdString(object.contentHash));
            if (!jobs.exec())
            {
                transaction.rollback();
                return makeQueryError(QStringLiteral("Delete evicted mail vault projections"),
                                      jobs);
            }
            if (const auto error = transaction.commit())
                return *error;
            ++evicted;
        }
        return evicted;
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

    namespace
    {
        [[nodiscard]] std::optional<DatabaseError>
        replayProjectionJobs(DatabaseConnection& connection, const std::size_t limit,
                             const std::optional<std::pair<std::string, std::string>>& mailbox)
        {
            QSqlQuery select{connection.database()};
            QString statement = QStringLiteral(
                "SELECT j.job_id,j.account_id,j.email_id,COALESCE(j.mailbox_id,''),"
                "COALESCE(j.content_hash,''),COALESCE(o.relative_path,''),COALESCE(o.size,0),"
                "j.operation FROM mail_vault_projection_jobs j LEFT JOIN mail_vault_objects o ON "
                "o.content_hash=j.content_hash WHERE j.status='pending'");
            if (mailbox.has_value())
                statement += QStringLiteral(" AND j.account_id=:account AND j.mailbox_id=:mailbox");
            statement += QStringLiteral(" ORDER BY j.job_id LIMIT :limit");
            select.prepare(statement);
            if (mailbox.has_value())
            {
                select.bindValue(QStringLiteral(":account"),
                                 QString::fromStdString(mailbox->first));
                select.bindValue(QStringLiteral(":mailbox"),
                                 QString::fromStdString(mailbox->second));
            }
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

            const MailVault vault = MailVault::forDatabase(connection);
            std::optional<DatabaseError> firstFailure;
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
                    QSqlQuery metadata{connection.database()};
                    if (job.mailboxId.empty())
                    {
                        metadata.prepare(QStringLiteral(
                            "SELECT email_address FROM accounts WHERE account_id=:account"));
                        metadata.bindValue(QStringLiteral(":account"),
                                           QString::fromStdString(job.accountId));
                        if (!metadata.exec())
                        {
                            failure = MailVaultError{
                                .message = QStringLiteral("Read mail account metadata: ") +
                                           metadata.lastError().text()};
                        }
                        else if (metadata.next())
                        {
                            failure = vault.writeAccountMetadata(
                                job.accountId, metadata.value(0).toString().toStdString());
                        }
                    }
                    else
                    {
                        metadata.prepare(QStringLiteral(
                            "SELECT name FROM mailboxes WHERE account_id=:account AND "
                            "mailbox_id=:mailbox"));
                        metadata.bindValue(QStringLiteral(":account"),
                                           QString::fromStdString(job.accountId));
                        metadata.bindValue(QStringLiteral(":mailbox"),
                                           QString::fromStdString(job.mailboxId));
                        if (!metadata.exec())
                        {
                            failure = MailVaultError{
                                .message = QStringLiteral("Read mail mailbox metadata: ") +
                                           metadata.lastError().text()};
                        }
                        else if (metadata.next())
                        {
                            failure = vault.writeMailboxMetadata(
                                job.accountId, job.mailboxId,
                                metadata.value(0).toString().toStdString());
                        }
                    }
                }
                else
                {
                    failure = MailVaultError{
                        .message =
                            QStringLiteral("Replay mail vault projection: unsupported operation %1")
                                .arg(QString::fromStdString(job.operation))};
                }
                const DatabaseWriteScope writeScope{connection};
                QSqlQuery update{connection.database()};
                update.prepare(QStringLiteral(
                    "UPDATE mail_vault_projection_jobs SET status=:status,last_error=:error,"
                    "updated_at=CURRENT_TIMESTAMP WHERE job_id=:job_id"));
                update.bindValue(QStringLiteral(":status"),
                                 failure ? QStringLiteral("pending") : QStringLiteral("complete"));
                update.bindValue(QStringLiteral(":error"),
                                 failure ? QVariant{failure->message} : QVariant{});
                update.bindValue(QStringLiteral(":job_id"), job.id);
                if (!update.exec())
                {
                    return makeQueryError(QStringLiteral("Complete mail vault projection job"),
                                          update);
                }
                if (failure && !firstFailure.has_value())
                    firstFailure = vaultError(*failure);
            }
            return firstFailure;
        }
    } // namespace

    std::optional<DatabaseError>
    RawMessageSourceRepository::replayProjectionJobs(const std::size_t limit)
    {
        return javelin::jmap::cache::replayProjectionJobs(m_connection, limit, std::nullopt);
    }

    std::optional<DatabaseError> RawMessageSourceRepository::replayProjectionJobsForMailbox(
        const std::string_view accountId, const std::string_view mailboxId, const std::size_t limit)
    {
        return javelin::jmap::cache::replayProjectionJobs(
            m_connection, limit, std::pair{std::string{accountId}, std::string{mailboxId}});
    }

} // namespace javelin::jmap::cache
