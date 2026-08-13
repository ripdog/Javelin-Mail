#include "jmap/cache/JmapTransportPreferenceRepository.h"

#include <QSqlError>
#include <QSqlQuery>
namespace javelin::jmap::cache
{
    namespace
    {
        [[nodiscard]] DatabaseError queryError(const QString& operation, const QSqlQuery& query)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = operation + QStringLiteral(": ") + query.lastError().text(),
            };
        }

    } // namespace

    JmapTransportPreferenceRepository::JmapTransportPreferenceRepository(
        DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    JmapTransportTargetResult
    JmapTransportPreferenceRepository::resolve(const std::string_view accountId) const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(
            QStringLiteral("SELECT COALESCE(accounts.owner_account_id, accounts.account_id), "
                           "sessions.websocket_url "
                           "FROM accounts "
                           "LEFT JOIN sessions ON sessions.account_id = "
                           "COALESCE(accounts.owner_account_id, accounts.account_id) "
                           "WHERE accounts.account_id = :account_id"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        if (!query.exec())
        {
            return queryError(QStringLiteral("Resolve JMAP transport preference"), query);
        }
        if (!query.next() || query.value(1).isNull())
        {
            return std::optional<JmapTransportTarget>{std::nullopt};
        }

        return std::optional{JmapTransportTarget{
            .ownerAccountId = query.value(0).toString().toStdString(),
            .webSocketUrl = query.value(1).toString().toStdString(),
        }};
    }

} // namespace javelin::jmap::cache
