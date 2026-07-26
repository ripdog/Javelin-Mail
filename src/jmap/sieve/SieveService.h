#pragma once

#include "jmap/JmapCore.h"
#include "jmap/sieve/SieveTypes.h"

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
    struct SieveValidation
    {
        bool valid = false;
        QString message;
    };

    using SieveListResult = std::variant<std::vector<SieveScript>, OperationError>;
    using SieveContentResult = std::variant<QByteArray, OperationError>;
    using SieveValidationResult = std::variant<SieveValidation, OperationError>;
    using SieveSaveResult = std::variant<SieveScript, OperationError>;
    using SieveDeleteResult = std::variant<std::monostate, OperationError>;
    using SieveActivationResult = std::variant<std::monostate, OperationError>;

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
