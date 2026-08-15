#pragma once

#include "app/MailTransferPlanning.h"
#include "app/MessageSelection.h"
#include "jmap/OperationError.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <QCoroTask>

#include <cstddef>
#include <optional>
#include <string>
#include <variant>

namespace javelin::app
{
    class ThreadMaterializationCoordinator;

    struct MailTransferPreparationRequest
    {
        MailTransferIntent intent;
        MessageSelection selection;
        std::vector<MailTransferSourceCleanupOverride> sourceCleanupOverrides = {};
    };

    struct PreparedMailTransfer
    {
        std::string operationId;
        std::size_t itemCount = 0;
        MailTransferTopology topology = MailTransferTopology::CrossServerImport;
    };

    using MailTransferPreparationResult =
        std::variant<PreparedMailTransfer, javelin::jmap::OperationError>;

    class MailTransferApplicationService
    {
      public:
        explicit MailTransferApplicationService(
            javelin::jmap::cache::DatabaseConnection& databaseConnection,
            ThreadMaterializationCoordinator* threadMaterializationCoordinator = nullptr);

        [[nodiscard]] QCoro::Task<MailTransferPreparationResult>
        prepare(MailTransferPreparationRequest request);

      private:
        [[nodiscard]] QCoro::Task<std::optional<javelin::jmap::OperationError>>
        ensureSelectionMaterialized(std::string accountId,
                                    std::optional<std::string> sourceMailboxId,
                                    MessageSelection selection);

        javelin::jmap::cache::DatabaseConnection& m_databaseConnection;
        ThreadMaterializationCoordinator* m_threadMaterializationCoordinator = nullptr;
    };

} // namespace javelin::app
