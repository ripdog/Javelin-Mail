#pragma once

#include "jmap/JmapCore.h"
#include "jmap/identity/IdentityCommandTypes.h"

#include <QCoroTask>

#include <functional>
#include <optional>
#include <string>

namespace javelin::jmap::api
{
    class JmapMethodTransport;
}
namespace javelin::jmap::cache
{
    class DatabaseConnection;
}

namespace javelin::jmap::identity
{
    class IdentityService
    {
      public:
        IdentityService(cache::DatabaseConnection& connection,
                        api::JmapMethodTransport& methodTransport);

        [[nodiscard]] QCoro::Task<IdentityListResult> refresh(LiveConnectionSettings settings,
                                                              std::string accountId) const;
        [[nodiscard]] QCoro::Task<IdentityListResult> refresh(LiveConnectionSettings settings,
                                                              std::string ownerAccountId,
                                                              std::string accountId) const;
        [[nodiscard]] QCoro::Task<IdentitySaveResult>
        save(LiveConnectionSettings settings, std::string accountId,
             javelin::jmap::domain::Identity identity,
             std::optional<std::string> operationGroupId = std::nullopt,
             std::function<void()> projectionCommitted = {}) const;
        [[nodiscard]] QCoro::Task<IdentitySaveResult>
        save(LiveConnectionSettings settings, std::string ownerAccountId, std::string accountId,
             javelin::jmap::domain::Identity identity,
             std::optional<std::string> operationGroupId = std::nullopt,
             std::function<void()> projectionCommitted = {}) const;
        [[nodiscard]] QCoro::Task<IdentityDeleteResult>
        remove(LiveConnectionSettings settings, std::string accountId, std::string identityId,
               std::optional<std::string> operationGroupId = std::nullopt,
               std::function<void()> projectionCommitted = {}) const;
        [[nodiscard]] QCoro::Task<IdentityDeleteResult>
        remove(LiveConnectionSettings settings, std::string ownerAccountId, std::string accountId,
               std::string identityId, std::optional<std::string> operationGroupId = std::nullopt,
               std::function<void()> projectionCommitted = {}) const;

      private:
        cache::DatabaseConnection& m_connection;
        api::JmapMethodTransport& m_methodTransport;
    };
} // namespace javelin::jmap::identity
