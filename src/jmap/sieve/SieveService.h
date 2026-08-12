#pragma once

#include "storage/sqlite/DatabaseConnection.h"

#include "jmap/JmapCore.h"
#include "jmap/sieve/SieveCommandTypes.h"

#include <QCoroTask>

#include <QByteArray>
#include <QString>

#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace javelin::jmap::api
{
    class AbstractTransport;
    class JmapMethodTransport;
} // namespace javelin::jmap::api

namespace javelin::jmap::cache
{
    class DatabaseConnection;
}

namespace javelin::jmap::sieve
{
    class SieveService
    {
      public:
        SieveService(cache::DatabaseConnection& connection,
                     api::AbstractTransport& resourceTransport,
                     api::JmapMethodTransport& methodTransport);

        [[nodiscard]] QCoro::Task<SieveListResult> list(LiveConnectionSettings settings,
                                                        std::string ownerAccountId) const;
        [[nodiscard]] QCoro::Task<SieveContentResult>
        load(LiveConnectionSettings settings, std::string ownerAccountId, SieveScript script) const;
        [[nodiscard]] QCoro::Task<SieveValidationResult> validate(LiveConnectionSettings settings,
                                                                  std::string ownerAccountId,
                                                                  QByteArray content) const;
        [[nodiscard]] QCoro::Task<SieveSaveResult>
        save(LiveConnectionSettings settings, std::string ownerAccountId, SieveScript script,
             QByteArray content, std::optional<std::string> operationGroupId = std::nullopt) const;
        [[nodiscard]] QCoro::Task<SieveDeleteResult>
        remove(LiveConnectionSettings settings, std::string ownerAccountId, SieveScript script,
               std::optional<std::string> operationGroupId = std::nullopt) const;
        [[nodiscard]] QCoro::Task<SieveActivationResult>
        setActive(LiveConnectionSettings settings, std::string ownerAccountId, SieveScript script,
                  bool active, std::optional<std::string> operationGroupId = std::nullopt) const;

      private:
        cache::DatabaseConnection& m_connection;
        api::AbstractTransport& m_resourceTransport;
        api::JmapMethodTransport& m_methodTransport;
    };
} // namespace javelin::jmap::sieve
