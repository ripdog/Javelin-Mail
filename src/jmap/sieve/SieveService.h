#pragma once

#include "jmap/JmapCore.h"

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

namespace javelin::jmap::sieve
{
    struct SieveScript
    {
        std::string id;
        std::string name;
        std::string blobId;
        bool isActive = false;
    };

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
        SieveService(api::AbstractTransport& resourceTransport,
                     api::JmapMethodTransport& methodTransport);

        [[nodiscard]] QCoro::Task<SieveListResult> list(LiveConnectionSettings settings,
                                                        std::string ownerAccountId) const;
        [[nodiscard]] QCoro::Task<SieveContentResult>
        load(LiveConnectionSettings settings, std::string ownerAccountId, SieveScript script) const;
        [[nodiscard]] QCoro::Task<SieveValidationResult> validate(LiveConnectionSettings settings,
                                                                  std::string ownerAccountId,
                                                                  QByteArray content) const;
        [[nodiscard]] QCoro::Task<SieveSaveResult> save(LiveConnectionSettings settings,
                                                        std::string ownerAccountId,
                                                        SieveScript script,
                                                        QByteArray content) const;
        [[nodiscard]] QCoro::Task<SieveDeleteResult> remove(LiveConnectionSettings settings,
                                                            std::string ownerAccountId,
                                                            SieveScript script) const;
        [[nodiscard]] QCoro::Task<SieveActivationResult> setActive(LiveConnectionSettings settings,
                                                                   std::string ownerAccountId,
                                                                   SieveScript script,
                                                                   bool active) const;

      private:
        api::AbstractTransport& m_resourceTransport;
        api::JmapMethodTransport& m_methodTransport;
    };
} // namespace javelin::jmap::sieve
