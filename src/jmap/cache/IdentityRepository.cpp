#include "jmap/cache/IdentityRepository.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>

#include <algorithm>

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

        [[nodiscard]] QJsonObject
        serializeEmailAddress(const javelin::jmap::domain::EmailAddress& address)
        {
            QJsonObject object;
            if (address.name.has_value())
            {
                object.insert(QStringLiteral("name"), QString::fromStdString(*address.name));
            }
            object.insert(QStringLiteral("email"), QString::fromStdString(address.email));
            return object;
        }

        [[nodiscard]] QString
        serializeAddressList(const std::vector<javelin::jmap::domain::EmailAddress>& addresses)
        {
            QJsonArray array;
            for (const auto& address : addresses)
            {
                array.push_back(serializeEmailAddress(address));
            }
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
                {
                    continue;
                }

                const auto object = value.toObject();
                const auto email = object.value(QStringLiteral("email")).toString();
                if (email.isEmpty())
                {
                    continue;
                }

                const auto name = object.value(QStringLiteral("name")).toString();
                addresses.push_back(javelin::jmap::domain::EmailAddress{
                    .name = name.isEmpty() ? std::nullopt
                                           : std::optional<std::string>{name.toStdString()},
                    .email = email.toStdString(),
                });
            }
            return addresses;
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
    IdentityRepository::replaceAll(const std::string_view accountId,
                                   const std::vector<javelin::jmap::domain::Identity>& identities)
    {
        if (m_writeConnection == nullptr)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Cannot replace identities on a read-only connection"),
            };
        }
        if (const auto error = m_connection.validate())
        {
            return error;
        }

        const DatabaseWriteScope writeScope{*m_writeConnection};
        auto& database = m_writeConnection->database();
        if (!database.transaction())
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Begin identity replacement transaction: ") +
                           database.lastError().text(),
            };
        }

        QSqlQuery deleteQuery{database};
        deleteQuery.prepare(
            QStringLiteral("DELETE FROM identities WHERE account_id = :account_id"));
        deleteQuery.bindValue(QStringLiteral(":account_id"),
                              QString::fromStdString(std::string{accountId}));
        if (!deleteQuery.exec())
        {
            database.rollback();
            return makeQueryError(QStringLiteral("Delete cached identities"), deleteQuery);
        }

        QSqlQuery insertQuery{database};
        insertQuery.prepare(QStringLiteral(
            "INSERT INTO identities ("
            "account_id, identity_id, email_address, name, reply_to_json, bcc_json, "
            "text_signature, html_signature, may_delete, state"
            ") VALUES ("
            ":account_id, :identity_id, :email_address, :name, :reply_to_json, :bcc_json, "
            ":text_signature, :html_signature, :may_delete, :state"
            ")"));
        for (const auto& identity : identities)
        {
            insertQuery.bindValue(QStringLiteral(":account_id"),
                                  QString::fromStdString(std::string{accountId}));
            insertQuery.bindValue(QStringLiteral(":identity_id"),
                                  QString::fromStdString(identity.id));
            insertQuery.bindValue(QStringLiteral(":email_address"),
                                  QString::fromStdString(identity.email));
            insertQuery.bindValue(QStringLiteral(":name"), QString::fromStdString(identity.name));
            insertQuery.bindValue(QStringLiteral(":reply_to_json"),
                                  serializeAddressList(identity.replyTo));
            insertQuery.bindValue(QStringLiteral(":bcc_json"), serializeAddressList(identity.bcc));
            insertQuery.bindValue(QStringLiteral(":text_signature"),
                                  identity.textSignature.has_value()
                                      ? QVariant{QString::fromStdString(*identity.textSignature)}
                                      : QVariant{});
            insertQuery.bindValue(QStringLiteral(":html_signature"),
                                  identity.htmlSignature.has_value()
                                      ? QVariant{QString::fromStdString(*identity.htmlSignature)}
                                      : QVariant{});
            insertQuery.bindValue(QStringLiteral(":may_delete"), identity.mayDelete ? 1 : 0);
            insertQuery.bindValue(QStringLiteral(":state"), QVariant{});
            if (!insertQuery.exec())
            {
                database.rollback();
                return makeQueryError(QStringLiteral("Insert cached identity"), insertQuery);
            }
        }

        if (!database.commit())
        {
            database.rollback();
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Commit identity replacement transaction: ") +
                           database.lastError().text(),
            };
        }

        return std::nullopt;
    }

    std::variant<std::vector<javelin::jmap::domain::Identity>, DatabaseError>
    IdentityRepository::listByAccount(const std::string_view accountId) const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT identity_id, email_address, name, reply_to_json, bcc_json, text_signature, "
            "html_signature, may_delete "
            "FROM identities WHERE account_id = :account_id ORDER BY name, email_address, "
            "identity_id"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        if (!query.exec())
        {
            return makeQueryError(QStringLiteral("Read cached identities"), query);
        }

        std::vector<javelin::jmap::domain::Identity> identities;
        while (query.next())
        {
            identities.push_back(javelin::jmap::domain::Identity{
                .id = query.value(0).toString().toStdString(),
                .name = query.value(2).toString().toStdString(),
                .email = query.value(1).toString().toStdString(),
                .replyTo = deserializeAddressList(query.value(3).toString()),
                .bcc = deserializeAddressList(query.value(4).toString()),
                .textSignature =
                    query.value(5).isNull()
                        ? std::nullopt
                        : std::optional<std::string>{query.value(5).toString().toStdString()},
                .htmlSignature =
                    query.value(6).isNull()
                        ? std::nullopt
                        : std::optional<std::string>{query.value(6).toString().toStdString()},
                .mayDelete = query.value(7).toInt() != 0,
            });
        }

        return identities;
    }

    std::variant<std::optional<javelin::jmap::domain::Identity>, DatabaseError>
    IdentityRepository::find(const std::string_view accountId,
                             const std::string_view identityId) const
    {
        const auto identitiesResult = listByAccount(accountId);
        if (const auto* error = std::get_if<DatabaseError>(&identitiesResult))
        {
            return *error;
        }

        const auto& identities =
            std::get<std::vector<javelin::jmap::domain::Identity>>(identitiesResult);
        const auto it =
            std::find_if(identities.cbegin(), identities.cend(),
                         [identityId](const auto& identity) { return identity.id == identityId; });
        if (it == identities.cend())
        {
            return std::optional<javelin::jmap::domain::Identity>{std::nullopt};
        }

        return std::optional<javelin::jmap::domain::Identity>{*it};
    }

} // namespace javelin::jmap::cache
