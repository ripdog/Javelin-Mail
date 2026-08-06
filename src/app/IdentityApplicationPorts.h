#pragma once

#include "jmap/identity/IdentityCommandTypes.h"

#include <QCoroTask>

#include <string>

namespace javelin::app
{
    class IdentityCommandPort
    {
      public:
        virtual ~IdentityCommandPort() = default;

        [[nodiscard]] virtual QCoro::Task<javelin::jmap::identity::IdentityListResult>
        requestSenderIdentities(std::string accountId) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::identity::IdentitySaveResult>
        saveSenderIdentity(std::string accountId, javelin::jmap::domain::Identity identity) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::identity::IdentityDeleteResult>
        deleteSenderIdentity(std::string accountId, std::string identityId) = 0;
    };
} // namespace javelin::app
