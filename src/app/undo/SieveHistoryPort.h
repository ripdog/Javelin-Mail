#pragma once

#include "app/undo/HistoryTypes.h"
#include "jmap/sieve/SieveService.h"

namespace javelin::app::undo
{

    class SieveHistoryPort
    {
      public:
        virtual ~SieveHistoryPort() = default;

        [[nodiscard]] virtual QCoro::Task<javelin::jmap::sieve::SieveListResult>
        requestSieveScripts(std::string ownerAccountId) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::sieve::SieveContentResult>
        requestSieveScript(std::string ownerAccountId,
                           javelin::jmap::sieve::SieveScript script) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::sieve::SieveSaveResult>
        saveSieveScript(std::string ownerAccountId, javelin::jmap::sieve::SieveScript script,
                        QByteArray content, CommandOrigin origin) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::sieve::SieveDeleteResult>
        deleteSieveScript(std::string ownerAccountId, javelin::jmap::sieve::SieveScript script,
                          CommandOrigin origin) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::sieve::SieveActivationResult>
        setSieveScriptActive(std::string ownerAccountId, javelin::jmap::sieve::SieveScript script,
                             bool active, CommandOrigin origin) = 0;
    };

} // namespace javelin::app::undo
