#include "app/SieveCommandService.h"

#include "app/SieveApplicationService.h"

namespace javelin::app
{
    SieveCommandService::SieveCommandService(SieveApplicationService& service) : m_service(service)
    {
    }

    QCoro::Task<javelin::jmap::sieve::SieveListResult>
    SieveCommandService::requestSieveScripts(std::string ownerAccountId)
    {
        return m_service.requestSieveScripts(std::move(ownerAccountId));
    }

    QCoro::Task<javelin::jmap::sieve::SieveContentResult>
    SieveCommandService::requestSieveScript(std::string ownerAccountId,
                                            javelin::jmap::sieve::SieveScript script)
    {
        return m_service.requestSieveScript(std::move(ownerAccountId), std::move(script));
    }

    QCoro::Task<javelin::jmap::sieve::SieveValidationResult>
    SieveCommandService::validateSieveScript(std::string ownerAccountId, QByteArray content)
    {
        return m_service.validateSieveScript(std::move(ownerAccountId), std::move(content));
    }

    QCoro::Task<javelin::jmap::sieve::SieveSaveResult>
    SieveCommandService::saveSieveScript(std::string ownerAccountId,
                                         javelin::jmap::sieve::SieveScript script,
                                         QByteArray content, undo::CommandOrigin origin)
    {
        return m_service.saveSieveScript(std::move(ownerAccountId), std::move(script),
                                         std::move(content), origin);
    }

    QCoro::Task<javelin::jmap::sieve::SieveDeleteResult>
    SieveCommandService::deleteSieveScript(std::string ownerAccountId,
                                           javelin::jmap::sieve::SieveScript script,
                                           undo::CommandOrigin origin)
    {
        return m_service.deleteSieveScript(std::move(ownerAccountId), std::move(script), origin);
    }

    QCoro::Task<javelin::jmap::sieve::SieveActivationResult>
    SieveCommandService::setSieveScriptActive(std::string ownerAccountId,
                                              javelin::jmap::sieve::SieveScript script,
                                              const bool active, undo::CommandOrigin origin)
    {
        return m_service.setSieveScriptActive(std::move(ownerAccountId), std::move(script), active,
                                              origin);
    }
} // namespace javelin::app
