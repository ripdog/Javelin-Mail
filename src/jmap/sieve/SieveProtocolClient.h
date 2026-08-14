#pragma once

#include "jmap/api/LiveConnectionSettings.h"
#include "jmap/sieve/SieveCommandTypes.h"

#include <QCoroTask>

#include <QByteArray>

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
    class SieveMutationEngine;

    class SieveProtocolClient
    {
      public:
        SieveProtocolClient(cache::DatabaseConnection& connection,
                            api::AbstractTransport& resourceTransport,
                            api::JmapMethodTransport& methodTransport);

        [[nodiscard]] QCoro::Task<SieveListResult> list(LiveConnectionSettings settings,
                                                        std::string ownerAccountId) const;
        [[nodiscard]] QCoro::Task<SieveContentResult>
        load(LiveConnectionSettings settings, std::string ownerAccountId, SieveScript script) const;
        [[nodiscard]] QCoro::Task<SieveValidationResult> validate(LiveConnectionSettings settings,
                                                                  std::string ownerAccountId,
                                                                  QByteArray content) const;

      private:
        friend class SieveMutationEngine;

        cache::DatabaseConnection& m_connection;
        api::AbstractTransport& m_resourceTransport;
        api::JmapMethodTransport& m_methodTransport;
    };
} // namespace javelin::jmap::sieve
