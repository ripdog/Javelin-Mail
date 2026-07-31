#include "jmap/cache/RawMessageSourceReadRepository.h"

#include "jmap/cache/MailVault.h"

#include <QSqlError>
#include <QSqlQuery>

namespace javelin::jmap::cache
{
    namespace
    {
        [[nodiscard]] DatabaseError makeQueryError(const QString& operation, const QSqlQuery& query)
        {
            return {.code = DatabaseErrorCode::QueryFailed,
                    .message = operation + QStringLiteral(": ") + query.lastError().text()};
        }

        [[nodiscard]] DatabaseError vaultError(const MailVaultError& error)
        {
            return {.code = DatabaseErrorCode::QueryFailed, .message = error.message};
        }
    } // namespace

    RawMessageSourceReadRepository::RawMessageSourceReadRepository(
        const DatabaseReadView& connection)
        : m_connection(connection)
    {
    }

    std::variant<std::optional<RawMessageSource>, DatabaseError>
    RawMessageSourceReadRepository::find(const std::string_view accountId,
                                         const std::string_view emailId) const
    {
        if (const auto error = m_connection.validate())
            return *error;

        const auto& database = m_connection.database();
        QSqlQuery query{database};
        query.prepare(QStringLiteral(
            "SELECT r.blob_id,o.content_hash,o.relative_path,o.size FROM mail_vault_email_refs r "
            "JOIN mail_vault_objects o ON o.content_hash=r.content_hash WHERE "
            "r.account_id=:account_id AND r.email_id=:email_id"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":email_id"), QString::fromStdString(std::string{emailId}));
        if (!query.exec())
            return makeQueryError(QStringLiteral("Read raw message source"), query);
        if (query.next())
        {
            const MailVaultObject object{
                .contentHash = query.value(1).toString().toStdString(),
                .relativePath = query.value(2).toString(),
                .size = query.value(3).toULongLong(),
            };
            const auto payload = MailVault::forDatabase(m_connection).read(object);
            if (const auto* error = std::get_if<MailVaultError>(&payload))
                return vaultError(*error);
            return std::optional<RawMessageSource>{RawMessageSource{
                .emailId = std::string{emailId},
                .blobId = query.value(0).toString().toStdString(),
                .payload = std::get<QByteArray>(payload),
            }};
        }

        QSqlQuery migrationQuery{database};
        migrationQuery.prepare(
            QStringLiteral("SELECT status FROM local_data_migrations WHERE migration_key="
                           "'raw_message_sources_to_vault'"));
        if (!migrationQuery.exec() || !migrationQuery.next())
            return makeQueryError(QStringLiteral("Read raw source migration state"),
                                  migrationQuery);
        if (migrationQuery.value(0).toString() == QStringLiteral("complete"))
            return std::optional<RawMessageSource>{std::nullopt};

        QSqlQuery legacyQuery{database};
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
} // namespace javelin::jmap::cache
