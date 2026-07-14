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

    enum class SieveServiceErrorCode
    {
        Authentication,
        Unsupported,
        Transport,
        Protocol,
        InvalidScript,
    };

    struct SieveServiceError
    {
        SieveServiceErrorCode code = SieveServiceErrorCode::Protocol;
        QString message;
    };

    struct SieveValidation
    {
        bool valid = false;
        QString message;
    };

    using SieveListResult = std::variant<std::vector<SieveScript>, SieveServiceError>;
    using SieveContentResult = std::variant<QByteArray, SieveServiceError>;
    using SieveValidationResult = std::variant<SieveValidation, SieveServiceError>;
    using SieveSaveResult = std::variant<SieveScript, SieveServiceError>;
    using SieveDeleteResult = std::variant<std::monostate, SieveServiceError>;

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

      private:
        api::AbstractTransport& m_resourceTransport;
        api::JmapMethodTransport& m_methodTransport;
    };
} // namespace javelin::jmap::sieve
