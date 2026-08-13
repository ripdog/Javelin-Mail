#pragma once

#include "app/undo/SieveHistoryPort.h"
#include "jmap/sieve/SieveService.h"

#include <QObject>

#include <string>

namespace javelin::app::undo
{
    class UndoManager;
}

namespace javelin::app
{
    class AccountRuntimeManager;
    class ApplicationErrorCoordinator;
    class WorkScheduler;

    class SieveApplicationService final : public QObject,
                                          public javelin::app::undo::SieveHistoryPort
    {
        Q_OBJECT

      public:
        SieveApplicationService(javelin::jmap::sieve::SieveService& sieveService,
                                AccountRuntimeManager& accountRuntime,
                                ApplicationErrorCoordinator& errorCoordinator,
                                WorkScheduler& workScheduler,
                                javelin::app::undo::UndoManager& undoManager,
                                QObject* parent = nullptr);

        [[nodiscard]] QCoro::Task<javelin::jmap::sieve::SieveListResult>
        requestSieveScripts(std::string ownerAccountId) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::sieve::SieveContentResult>
        requestSieveScript(std::string ownerAccountId,
                           javelin::jmap::sieve::SieveScript script) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::sieve::SieveValidationResult>
        validateSieveScript(std::string ownerAccountId, QByteArray content);
        [[nodiscard]] QCoro::Task<javelin::jmap::sieve::SieveSaveResult>
        saveSieveScript(std::string ownerAccountId, javelin::jmap::sieve::SieveScript script,
                        QByteArray content,
                        javelin::app::undo::CommandOrigin origin =
                            javelin::app::undo::CommandOrigin::User) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::sieve::SieveDeleteResult>
        deleteSieveScript(std::string ownerAccountId, javelin::jmap::sieve::SieveScript script,
                          javelin::app::undo::CommandOrigin origin =
                              javelin::app::undo::CommandOrigin::User) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::sieve::SieveActivationResult>
        setSieveScriptActive(std::string ownerAccountId, javelin::jmap::sieve::SieveScript script,
                             bool active,
                             javelin::app::undo::CommandOrigin origin =
                                 javelin::app::undo::CommandOrigin::User) override;

      private:
        javelin::jmap::sieve::SieveService& m_sieveService;
        AccountRuntimeManager& m_accountRuntime;
        ApplicationErrorCoordinator& m_errorCoordinator;
        WorkScheduler& m_workScheduler;
        javelin::app::undo::UndoManager& m_undoManager;
    };

} // namespace javelin::app
