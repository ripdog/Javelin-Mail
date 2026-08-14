#pragma once

#include "jmap/api/LiveConnectionSettings.h"
#include "jmap/sieve/SieveCommandTypes.h"

#include <QCoroTask>

#include <QByteArray>

#include <optional>
#include <string>

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
    class SieveProtocolClient;

    class SieveMutationEngine
    {
      public:
        SieveMutationEngine(cache::DatabaseConnection& connection,
                            SieveProtocolClient& protocolClient);

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
        SieveProtocolClient& m_protocolClient;
        api::AbstractTransport& m_resourceTransport;
        api::JmapMethodTransport& m_methodTransport;
    };
} // namespace javelin::jmap::sieve
