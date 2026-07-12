#include "jmap/cache/JmapTransportPreferenceRepository.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace javelin::jmap::cache
{
    namespace
    {
        [[nodiscard]] QString modeName(const JmapTransportMode mode)
        {
            switch (mode)
            {
            case JmapTransportMode::Unknown:
                return QStringLiteral("unknown");
            case JmapTransportMode::WebSocket:
                return QStringLiteral("websocket");
            case JmapTransportMode::HttpFallback:
                return QStringLiteral("http_fallback");
            }

            return QStringLiteral("unknown");
        }

        [[nodiscard]] JmapTransportMode parseMode(const QString& value)
        {
            if (value == QStringLiteral("websocket"))
            {
                return JmapTransportMode::WebSocket;
            }
            if (value == QStringLiteral("http_fallback"))
            {
                return JmapTransportMode::HttpFallback;
            }
            return JmapTransportMode::Unknown;
        }

        [[nodiscard]] DatabaseError queryError(const QString& operation, const QSqlQuery& query)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = operation + QStringLiteral(": ") + query.lastError().text(),
            };
        }

        [[nodiscard]] std::optional<QDateTime> parseDateTime(const QVariant& value)
        {
            if (value.isNull())
            {
                return std::nullopt;
            }

            const auto parsed = QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
            return parsed.isValid() ? std::optional{parsed} : std::nullopt;
        }
    } // namespace

    bool JmapTransportTarget::shouldAttemptWebSocket(const QDateTime& now) const
    {
        return mode != JmapTransportMode::HttpFallback || !retryAfter.has_value() ||
               *retryAfter <= now;
    }

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
        query.prepare(QStringLiteral(
            "SELECT COALESCE(accounts.owner_account_id, accounts.account_id), "
            "sessions.websocket_url, transport.websocket_url, transport.mode, "
            "transport.retry_after, transport.last_error "
            "FROM accounts "
            "LEFT JOIN sessions ON sessions.account_id = "
            "COALESCE(accounts.owner_account_id, accounts.account_id) "
            "LEFT JOIN jmap_transport_preferences AS transport ON "
            "transport.owner_account_id = COALESCE(accounts.owner_account_id, "
            "accounts.account_id) "
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

        const auto advertisedUrl = query.value(1).toString();
        const bool preferenceMatches =
            !query.value(2).isNull() && query.value(2).toString() == advertisedUrl;
        return std::optional{JmapTransportTarget{
            .ownerAccountId = query.value(0).toString().toStdString(),
            .webSocketUrl = advertisedUrl.toStdString(),
            .mode = preferenceMatches ? parseMode(query.value(3).toString())
                                      : JmapTransportMode::Unknown,
            .retryAfter = preferenceMatches ? parseDateTime(query.value(4)) : std::nullopt,
            .lastError = preferenceMatches && !query.value(5).isNull()
                             ? std::optional{query.value(5).toString()}
                             : std::nullopt,
        }};
    }

    std::optional<DatabaseError> JmapTransportPreferenceRepository::markWebSocketAvailable(
        const std::string_view ownerAccountId, const std::string_view webSocketUrl)
    {
        return upsert(ownerAccountId, webSocketUrl, JmapTransportMode::WebSocket, std::nullopt,
                      std::nullopt);
    }

    std::optional<DatabaseError> JmapTransportPreferenceRepository::markHttpFallback(
        const std::string_view ownerAccountId, const std::string_view webSocketUrl,
        const QDateTime& retryAfter, const QString& errorMessage)
    {
        return upsert(ownerAccountId, webSocketUrl, JmapTransportMode::HttpFallback, retryAfter,
                      errorMessage);
    }

    std::optional<DatabaseError> JmapTransportPreferenceRepository::upsert(
        const std::string_view ownerAccountId, const std::string_view webSocketUrl,
        const JmapTransportMode mode, const std::optional<QDateTime>& retryAfter,
        const std::optional<QString>& errorMessage)
    {
        if (const auto error = m_connection.validate())
        {
            return error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "INSERT INTO jmap_transport_preferences (owner_account_id, websocket_url, mode, "
            "retry_after, last_error, updated_at) VALUES (:owner_account_id, :websocket_url, "
            ":mode, :retry_after, :last_error, CURRENT_TIMESTAMP) "
            "ON CONFLICT(owner_account_id) DO UPDATE SET websocket_url = excluded.websocket_url, "
            "mode = excluded.mode, retry_after = excluded.retry_after, "
            "last_error = excluded.last_error, updated_at = CURRENT_TIMESTAMP"));
        query.bindValue(QStringLiteral(":owner_account_id"),
                        QString::fromStdString(std::string{ownerAccountId}));
        query.bindValue(QStringLiteral(":websocket_url"),
                        QString::fromStdString(std::string{webSocketUrl}));
        query.bindValue(QStringLiteral(":mode"), modeName(mode));
        query.bindValue(QStringLiteral(":retry_after"),
                        retryAfter.has_value()
                            ? QVariant{retryAfter->toUTC().toString(Qt::ISODateWithMs)}
                            : QVariant{});
        query.bindValue(QStringLiteral(":last_error"),
                        errorMessage.has_value() ? QVariant{*errorMessage} : QVariant{});
        if (!query.exec())
        {
            return queryError(QStringLiteral("Store JMAP transport preference"), query);
        }
        return std::nullopt;
    }

} // namespace javelin::jmap::cache
