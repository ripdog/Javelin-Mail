#include "jmap/cache/IdentityRepository.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>

#include <algorithm>
#include <utility>

namespace javelin::jmap::cache
{

    namespace
    {
        using Identity = javelin::jmap::domain::Identity;

        [[nodiscard]] DatabaseError queryError(const QString& operation, const QSqlQuery& query)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = operation + QStringLiteral(": ") + query.lastError().text(),
            };
        }

        [[nodiscard]] QJsonObject
        serializeEmailAddress(const javelin::jmap::domain::EmailAddress& address)
        {
            QJsonObject object;
            if (address.name.has_value())
                object.insert(QStringLiteral("name"), QString::fromStdString(*address.name));
            object.insert(QStringLiteral("email"), QString::fromStdString(address.email));
            return object;
        }

        [[nodiscard]] QString
        serializeAddressList(const std::vector<javelin::jmap::domain::EmailAddress>& addresses)
        {
            QJsonArray array;
            for (const auto& address : addresses)
                array.push_back(serializeEmailAddress(address));
            return QString::fromUtf8(QJsonDocument{array}.toJson(QJsonDocument::Compact));
        }

        [[nodiscard]] std::vector<javelin::jmap::domain::EmailAddress>
        deserializeAddressList(const QString& json)
        {
            const auto array = QJsonDocument::fromJson(json.toUtf8()).array();
            std::vector<javelin::jmap::domain::EmailAddress> addresses;
            addresses.reserve(static_cast<std::size_t>(array.size()));
            for (const auto& value : array)
            {
                if (!value.isObject())
                    continue;
                const auto object = value.toObject();
                const auto email = object.value(QStringLiteral("email")).toString();
                if (email.isEmpty())
                    continue;
                const auto name = object.value(QStringLiteral("name")).toString();
                addresses.push_back({
                    .name = name.isEmpty() ? std::nullopt
                                           : std::optional<std::string>{name.toStdString()},
                    .email = email.toStdString(),
                });
            }
            return addresses;
        }

        void bindIdentity(QSqlQuery& query, const std::string_view accountId,
                          const Identity& identity)
        {
            query.bindValue(QStringLiteral(":account"),
                            QString::fromStdString(std::string{accountId}));
            query.bindValue(QStringLiteral(":id"), QString::fromStdString(identity.id));
            query.bindValue(QStringLiteral(":email"), QString::fromStdString(identity.email));
            query.bindValue(QStringLiteral(":name"), QString::fromStdString(identity.name));
            query.bindValue(QStringLiteral(":reply_to"), serializeAddressList(identity.replyTo));
            query.bindValue(QStringLiteral(":bcc"), serializeAddressList(identity.bcc));
            query.bindValue(QStringLiteral(":text_signature"),
                            identity.textSignature.has_value()
                                ? QVariant{QString::fromStdString(*identity.textSignature)}
                                : QVariant{});
            query.bindValue(QStringLiteral(":html_signature"),
                            identity.htmlSignature.has_value()
                                ? QVariant{QString::fromStdString(*identity.htmlSignature)}
                                : QVariant{});
            query.bindValue(QStringLiteral(":may_delete"), identity.mayDelete ? 1 : 0);
        }

        [[nodiscard]] Identity identityFromQuery(const QSqlQuery& query, const int offset = 0)
        {
            return {
                .id = query.value(offset).toString().toStdString(),
                .name = query.value(offset + 2).toString().toStdString(),
                .email = query.value(offset + 1).toString().toStdString(),
                .replyTo = deserializeAddressList(query.value(offset + 3).toString()),
                .bcc = deserializeAddressList(query.value(offset + 4).toString()),
                .textSignature = query.value(offset + 5).isNull()
                                     ? std::nullopt
                                     : std::optional<std::string>{query.value(offset + 5)
                                                                      .toString()
                                                                      .toStdString()},
                .htmlSignature = query.value(offset + 6).isNull()
                                     ? std::nullopt
                                     : std::optional<std::string>{query.value(offset + 6)
                                                                      .toString()
                                                                      .toStdString()},
                .mayDelete = query.value(offset + 7).toInt() != 0,
            };
        }

        [[nodiscard]] std::optional<DatabaseError> upsertIdentity(DatabaseConnection& connection,
                                                                  const std::string_view accountId,
                                                                  const Identity& identity)
        {
            QSqlQuery query{connection.database()};
            query.prepare(QStringLiteral(
                "INSERT INTO identities(account_id,identity_id,email_address,name,reply_to_json,"
                "bcc_json,text_signature,html_signature,may_delete,state) VALUES("
                ":account,:id,:email,:name,:reply_to,:bcc,:text_signature,:html_signature,"
                ":may_delete,NULL) ON CONFLICT(account_id,identity_id) DO UPDATE SET "
                "email_address=excluded.email_address,name=excluded.name,"
                "reply_to_json=excluded.reply_to_json,bcc_json=excluded.bcc_json,"
                "text_signature=excluded.text_signature,html_signature=excluded.html_signature,"
                "may_delete=excluded.may_delete"));
            bindIdentity(query, accountId, identity);
            if (!query.exec())
                return queryError(QStringLiteral("Store cached identity"), query);
            return std::nullopt;
        }
    } // namespace

    IdentityRepository::IdentityRepository(DatabaseConnection& connection)
        : m_connection(connection), m_writeConnection(&connection)
    {
    }

    IdentityRepository::IdentityRepository(ReadOnlyDatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    std::optional<DatabaseError>
    IdentityRepository::requireWritableTransaction(const DatabaseTransaction& transaction,
                                                   QString operation) const
    {
        if (m_writeConnection != nullptr && transaction.isActive() &&
            &transaction.connection() == m_writeConnection)
            return std::nullopt;
        return DatabaseError{
            .code = DatabaseErrorCode::QueryFailed,
            .message =
                std::move(operation) + QStringLiteral(" requires a matching writable transaction"),
        };
    }

    std::optional<DatabaseError>
    IdentityRepository::replaceAll(const std::string_view accountId,
                                   const std::vector<Identity>& identities,
                                   const std::string_view state)
    {
        if (m_writeConnection == nullptr)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Cannot replace identities on a read-only connection"),
            };
        }
        auto transactionResult =
            DatabaseTransaction::begin(*m_writeConnection, QStringLiteral("Replace identities"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));
        if (const auto error = replaceAll(transaction, accountId, identities, state))
            return error;
        return transaction.commit();
    }

    std::optional<DatabaseError> IdentityRepository::replaceAll(
        DatabaseTransaction& transaction, const std::string_view accountId,
        const std::vector<Identity>& identities, const std::string_view state)
    {
        if (const auto error =
                requireWritableTransaction(transaction, QStringLiteral("Identity replacement")))
            return error;

        QSqlQuery clear{m_writeConnection->database()};
        clear.prepare(QStringLiteral("DELETE FROM identities WHERE account_id=:account"));
        clear.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        if (!clear.exec())
            return queryError(QStringLiteral("Clear cached identities"), clear);
        for (const auto& identity : identities)
        {
            if (const auto error = upsertIdentity(*m_writeConnection, accountId, identity))
                return error;
        }

        QSqlQuery syncState{m_writeConnection->database()};
        syncState.prepare(
            QStringLiteral("INSERT INTO sync_state(account_id,object_type,query_key,state_token) "
                           "VALUES(:account,'Identity','',:state) ON CONFLICT(account_id,"
                           "object_type,query_key) DO UPDATE SET state_token=excluded.state_token,"
                           "updated_at=CURRENT_TIMESTAMP"));
        syncState.bindValue(QStringLiteral(":account"),
                            QString::fromStdString(std::string{accountId}));
        syncState.bindValue(QStringLiteral(":state"), QString::fromStdString(std::string{state}));
        if (!syncState.exec())
            return queryError(QStringLiteral("Store Identity state"), syncState);
        return std::nullopt;
    }

    std::optional<DatabaseError> IdentityRepository::applyChanges(
        const std::string_view accountId, const std::vector<Identity>& upserts,
        const std::vector<std::string>& destroyedIds, const std::string_view state)
    {
        if (m_writeConnection == nullptr)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message =
                    QStringLiteral("Cannot apply Identity changes on a read-only connection"),
            };
        }
        auto transactionResult = DatabaseTransaction::begin(
            *m_writeConnection, QStringLiteral("Apply incremental Identity changes"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));
        for (const auto& identity : upserts)
        {
            if (const auto error = projectUpsert(transaction, accountId, identity))
                return error;
        }
        for (const auto& identityId : destroyedIds)
        {
            if (const auto error = projectDestroy(transaction, accountId, identityId))
                return error;
        }

        QSqlQuery syncState{m_writeConnection->database()};
        syncState.prepare(
            QStringLiteral("INSERT INTO sync_state(account_id,object_type,query_key,state_token) "
                           "VALUES(:account,'Identity','',:state) ON CONFLICT(account_id,"
                           "object_type,query_key) DO UPDATE SET state_token=excluded.state_token,"
                           "updated_at=CURRENT_TIMESTAMP"));
        syncState.bindValue(QStringLiteral(":account"),
                            QString::fromStdString(std::string{accountId}));
        syncState.bindValue(QStringLiteral(":state"), QString::fromStdString(std::string{state}));
        if (!syncState.exec())
            return queryError(QStringLiteral("Store incremental Identity state"), syncState);
        return transaction.commit();
    }

    std::optional<DatabaseError> IdentityRepository::projectUpsert(DatabaseTransaction& transaction,
                                                                   const std::string_view accountId,
                                                                   const Identity& identity)
    {
        if (const auto error =
                requireWritableTransaction(transaction, QStringLiteral("Identity projection")))
            return error;
        return upsertIdentity(*m_writeConnection, accountId, identity);
    }

    std::optional<DatabaseError>
    IdentityRepository::projectDestroy(DatabaseTransaction& transaction,
                                       const std::string_view accountId,
                                       const std::string_view identityId)
    {
        if (const auto error =
                requireWritableTransaction(transaction, QStringLiteral("Identity projection")))
            return error;
        QSqlQuery query{m_writeConnection->database()};
        query.prepare(QStringLiteral("DELETE FROM identities WHERE account_id=:account AND "
                                     "identity_id=:identity"));
        query.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":identity"),
                        QString::fromStdString(std::string{identityId}));
        if (!query.exec())
            return queryError(QStringLiteral("Project Identity deletion"), query);
        return std::nullopt;
    }

    std::optional<DatabaseError> IdentityRepository::projectPendingCreate(
        DatabaseTransaction& transaction, const std::string_view accountId,
        const std::string_view creationId, const std::string_view mutationId,
        const Identity& identity)
    {
        if (const auto error = requireWritableTransaction(
                transaction, QStringLiteral("Identity create projection")))
            return error;
        QSqlQuery query{m_writeConnection->database()};
        query.prepare(QStringLiteral(
            "INSERT INTO identity_create_projections(account_id,creation_id,mutation_id,"
            "email_address,name,reply_to_json,bcc_json,text_signature,html_signature) VALUES("
            ":account,:creation,:mutation,:email,:name,:reply_to,:bcc,:text_signature,"
            ":html_signature)"));
        query.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":creation"),
                        QString::fromStdString(std::string{creationId}));
        query.bindValue(QStringLiteral(":mutation"),
                        QString::fromStdString(std::string{mutationId}));
        query.bindValue(QStringLiteral(":email"), QString::fromStdString(identity.email));
        query.bindValue(QStringLiteral(":name"), QString::fromStdString(identity.name));
        query.bindValue(QStringLiteral(":reply_to"), serializeAddressList(identity.replyTo));
        query.bindValue(QStringLiteral(":bcc"), serializeAddressList(identity.bcc));
        query.bindValue(QStringLiteral(":text_signature"),
                        identity.textSignature.has_value()
                            ? QVariant{QString::fromStdString(*identity.textSignature)}
                            : QVariant{});
        query.bindValue(QStringLiteral(":html_signature"),
                        identity.htmlSignature.has_value()
                            ? QVariant{QString::fromStdString(*identity.htmlSignature)}
                            : QVariant{});
        if (!query.exec())
            return queryError(QStringLiteral("Project Identity creation"), query);
        return std::nullopt;
    }

    std::optional<DatabaseError>
    IdentityRepository::removePendingCreate(DatabaseTransaction& transaction,
                                            const std::string_view accountId,
                                            const std::string_view creationId)
    {
        if (const auto error = requireWritableTransaction(
                transaction, QStringLiteral("Identity create projection removal")))
            return error;
        QSqlQuery query{m_writeConnection->database()};
        query.prepare(QStringLiteral("DELETE FROM identity_create_projections WHERE "
                                     "account_id=:account AND creation_id=:creation"));
        query.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":creation"),
                        QString::fromStdString(std::string{creationId}));
        if (!query.exec())
            return queryError(QStringLiteral("Remove Identity create projection"), query);
        return std::nullopt;
    }

    std::optional<DatabaseError>
    IdentityRepository::removeAllPendingCreates(DatabaseTransaction& transaction,
                                                const std::string_view accountId)
    {
        if (const auto error = requireWritableTransaction(
                transaction, QStringLiteral("Identity create projection removal")))
            return error;
        QSqlQuery query{m_writeConnection->database()};
        query.prepare(
            QStringLiteral("DELETE FROM identity_create_projections WHERE account_id=:account"));
        query.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        if (!query.exec())
            return queryError(QStringLiteral("Remove Identity create projections"), query);
        return std::nullopt;
    }

    std::variant<std::vector<Identity>, DatabaseError>
    IdentityRepository::listByAccount(const std::string_view accountId) const
    {
        if (const auto error = m_connection.validate())
            return *error;
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT identity_id,email_address,name,reply_to_json,bcc_json,text_signature,"
            "html_signature,may_delete FROM identities WHERE account_id=:account ORDER BY "
            "name COLLATE NOCASE,email_address COLLATE NOCASE,identity_id"));
        query.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        if (!query.exec())
            return queryError(QStringLiteral("Read cached identities"), query);
        std::vector<Identity> identities;
        while (query.next())
            identities.push_back(identityFromQuery(query));
        return identities;
    }

    std::variant<std::optional<Identity>, DatabaseError>
    IdentityRepository::find(const std::string_view accountId,
                             const std::string_view identityId) const
    {
        if (const auto error = m_connection.validate())
            return *error;
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT identity_id,email_address,name,reply_to_json,bcc_json,text_signature,"
            "html_signature,may_delete FROM identities WHERE account_id=:account AND "
            "identity_id=:identity"));
        query.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":identity"),
                        QString::fromStdString(std::string{identityId}));
        if (!query.exec())
            return queryError(QStringLiteral("Read cached identity"), query);
        if (!query.next())
            return std::optional<Identity>{};
        return std::optional<Identity>{identityFromQuery(query)};
    }

    std::variant<std::vector<PendingIdentityCreate>, DatabaseError>
    IdentityRepository::listPendingCreates(const std::string_view accountId) const
    {
        if (const auto error = m_connection.validate())
            return *error;
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT p.creation_id,p.mutation_id,p.email_address,p.name,p.reply_to_json,p.bcc_json,"
            "p.text_signature,p.html_signature,j.status,j.error_json FROM "
            "identity_create_projections p JOIN mutation_journal j ON "
            "j.mutation_id=p.mutation_id WHERE p.account_id=:account ORDER BY j.sequence"));
        query.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        if (!query.exec())
            return queryError(QStringLiteral("Read pending Identity creations"), query);
        std::vector<PendingIdentityCreate> pending;
        while (query.next())
        {
            pending.push_back({
                .creationId = query.value(0).toString().toStdString(),
                .mutationId = query.value(1).toString().toStdString(),
                .identity =
                    Identity{
                        .id = {},
                        .name = query.value(3).toString().toStdString(),
                        .email = query.value(2).toString().toStdString(),
                        .replyTo = deserializeAddressList(query.value(4).toString()),
                        .bcc = deserializeAddressList(query.value(5).toString()),
                        .textSignature = query.value(6).isNull()
                                             ? std::nullopt
                                             : std::optional<std::string>{query.value(6)
                                                                              .toString()
                                                                              .toStdString()},
                        .htmlSignature = query.value(7).isNull()
                                             ? std::nullopt
                                             : std::optional<std::string>{query.value(7)
                                                                              .toString()
                                                                              .toStdString()},
                        .mayDelete = true,
                    },
                .status = query.value(8).toString().toStdString(),
                .errorJson =
                    query.value(9).isNull()
                        ? std::nullopt
                        : std::optional<std::string>{query.value(9).toString().toStdString()},
            });
        }
        return pending;
    }

    std::variant<std::optional<std::string>, DatabaseError>
    IdentityRepository::state(const std::string_view accountId) const
    {
        if (const auto error = m_connection.validate())
            return *error;
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral("SELECT state_token FROM sync_state WHERE account_id=:account "
                                     "AND object_type='Identity' AND query_key=''"));
        query.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        if (!query.exec())
            return queryError(QStringLiteral("Read Identity state"), query);
        if (!query.next())
            return std::optional<std::string>{};
        return std::optional<std::string>{query.value(0).toString().toStdString()};
    }

} // namespace javelin::jmap::cache
