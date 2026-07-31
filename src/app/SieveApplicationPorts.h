#pragma once

#include "app/undo/HistoryTypes.h"
#include "jmap/sieve/SieveCommandTypes.h"

#include <QCoroTask>

#include <QByteArray>

#include <string>

namespace javelin::app
{
    class SieveCommandPort
    {
      public:
        virtual ~SieveCommandPort() = default;

        [[nodiscard]] virtual QCoro::Task<javelin::jmap::sieve::SieveListResult>
        requestSieveScripts(std::string ownerAccountId) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::sieve::SieveContentResult>
        requestSieveScript(std::string ownerAccountId,
                           javelin::jmap::sieve::SieveScript script) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::sieve::SieveValidationResult>
        validateSieveScript(std::string ownerAccountId, QByteArray content) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::sieve::SieveSaveResult>
        saveSieveScript(std::string ownerAccountId, javelin::jmap::sieve::SieveScript script,
                        QByteArray content,
                        undo::CommandOrigin origin = undo::CommandOrigin::User) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::sieve::SieveDeleteResult>
        deleteSieveScript(std::string ownerAccountId, javelin::jmap::sieve::SieveScript script,
                          undo::CommandOrigin origin = undo::CommandOrigin::User) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::sieve::SieveActivationResult>
        setSieveScriptActive(std::string ownerAccountId, javelin::jmap::sieve::SieveScript script,
                             bool active,
                             undo::CommandOrigin origin = undo::CommandOrigin::User) = 0;
    };
} // namespace javelin::app
