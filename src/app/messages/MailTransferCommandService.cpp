#include "app/MailTransferCommandService.h"

#include "app/MailTransferApplicationService.h"
#include "app/MailTransferWorkService.h"
#include "app/undo/MailTransferHistoryCoordinator.h"

#include <KLocalizedString>

#include <utility>

namespace javelin::app
{
    MailTransferCommandService::MailTransferCommandService(
        javelin::jmap::cache::DatabaseConnection& databaseConnection,
        javelin::jmap::api::AbstractTransport& resourceTransport,
        javelin::jmap::api::JmapMethodTransport& methodTransport,
        javelin::jmap::MessageContentClient& messageContentClient,
        const AccountConnectionProvider& connectionProvider,
        javelin::app::undo::MailTransferHistoryCoordinator& historyCoordinator,
        ThreadMaterializationCoordinator& threadMaterializationCoordinator)
        : m_databaseConnection(databaseConnection), m_resourceTransport(resourceTransport),
          m_methodTransport(methodTransport), m_messageContentClient(messageContentClient),
          m_connectionProvider(connectionProvider), m_historyCoordinator(historyCoordinator),
          m_threadMaterializationCoordinator(threadMaterializationCoordinator)
    {
    }

    void MailTransferCommandService::setWorkService(MailTransferWorkService* workService)
    {
        m_workService = workService;
    }

    QCoro::Task<MailTransferExecutionResult>
    MailTransferCommandService::transfer(CrossAccountMailTransferIntent intent)
    {
        if (intent.sourceAccountId.empty() || intent.destinationAccountId.empty() ||
            intent.destinationMailboxId.empty() || intent.selection.empty())
        {
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::InvalidRequest,
                .message = i18n("The cross-account mail transfer request is incomplete."),
            };
        }
        if (intent.sourceAccountId == intent.destinationAccountId)
        {
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::InvalidRequest,
                .message = i18n("Same-account Move and Copy must use the mailbox mutation path."),
            };
        }

        MailTransferApplicationService preparation{m_databaseConnection,
                                                   &m_threadMaterializationCoordinator};
        auto prepared = co_await preparation.prepare({
            .intent =
                {
                    .sourceAccountId = intent.sourceAccountId,
                    .sourceMailboxId = intent.sourceMailboxId,
                    .destinationAccountId = intent.destinationAccountId,
                    .destinationMailboxId = intent.destinationMailboxId,
                    .operation = intent.operation,
                },
            .selection = std::move(intent.selection),
            .sourceCleanupOverrides = {},
        });
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&prepared))
            co_return *error;

        auto operationId = std::get<PreparedMailTransfer>(prepared).operationId;
        if (m_workService != nullptr)
            co_return co_await m_workService->advanceForeground(std::move(operationId));

        MailTransferExecutor executor{m_databaseConnection, m_resourceTransport,
                                      m_methodTransport,    m_messageContentClient,
                                      m_connectionProvider, &m_historyCoordinator};
        co_return co_await executor.advance(std::move(operationId));
    }

} // namespace javelin::app
