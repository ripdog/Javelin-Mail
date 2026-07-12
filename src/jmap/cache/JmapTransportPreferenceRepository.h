#pragma once

#include "jmap/cache/Database.h"

#include <QDateTime>
#include <QString>

#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace javelin::jmap::cache
{
    enum class JmapTransportMode
    {
        Unknown,
        WebSocket,
        HttpFallback,
    };

    struct JmapTransportTarget
    {
        std::string ownerAccountId;
        std::string webSocketUrl;
        JmapTransportMode mode = JmapTransportMode::Unknown;
        std::optional<QDateTime> retryAfter;
        std::optional<QString> lastError;

        [[nodiscard]] bool shouldAttemptWebSocket(const QDateTime& now) const;
    };

    using JmapTransportTargetResult =
        std::variant<std::optional<JmapTransportTarget>, DatabaseError>;

    class JmapTransportPreferenceRepository
    {
      public:
        explicit JmapTransportPreferenceRepository(DatabaseConnection& connection);

        [[nodiscard]] JmapTransportTargetResult resolve(std::string_view accountId) const;
        [[nodiscard]] std::optional<DatabaseError>
        markWebSocketAvailable(std::string_view ownerAccountId, std::string_view webSocketUrl);
        [[nodiscard]] std::optional<DatabaseError>
        markHttpFallback(std::string_view ownerAccountId, std::string_view webSocketUrl,
                         const QDateTime& retryAfter, const QString& errorMessage);

      private:
        [[nodiscard]] std::optional<DatabaseError>
        upsert(std::string_view ownerAccountId, std::string_view webSocketUrl,
               JmapTransportMode mode, const std::optional<QDateTime>& retryAfter,
               const std::optional<QString>& errorMessage);

        DatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::cache
