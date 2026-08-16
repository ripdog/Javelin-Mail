#include "app/MailTransferWorkService.h"

#include "app/MailTransferRepository.h"
#include "app/WorkScheduler.h"
#include "app/undo/MailTransferHistoryCoordinator.h"
#include "jmap/OperationError.h"

#include <KLocalizedString>

#include <QCoroTask>

#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QTimer>

#include <algorithm>
#include <ranges>
#include <utility>
#include <variant>
#include <vector>

namespace javelin::app
{
    namespace
    {
        constexpr std::string_view jobPrefix = "mail-transfer:";

        [[nodiscard]] std::string jobIdFor(const std::string_view operationId)
        {
            return std::string{jobPrefix} + std::string{operationId};
        }

        [[nodiscard]] std::optional<std::string> operationIdFor(const std::string_view jobId)
        {
            if (!jobId.starts_with(jobPrefix) || jobId.size() <= jobPrefix.size())
                return std::nullopt;
            return std::string{jobId.substr(jobPrefix.size())};
        }

        [[nodiscard]] QString checkpoint(const std::string_view operationId,
                                         const bool canRetry = false)
        {
            return QString::fromUtf8(QJsonDocument{
                QJsonObject{
                    {QStringLiteral("operationId"),
                     QString::fromStdString(std::string{operationId})},
                    {QStringLiteral("canRetry"), canRetry},
                }}.toJson(QJsonDocument::Compact));
        }

        [[nodiscard]] WorkStatus workStatus(const MailTransferStatus status)
        {
            switch (status)
            {
            case MailTransferStatus::Preparing:
            case MailTransferStatus::Running:
                return WorkStatus::Queued;
            case MailTransferStatus::WaitingForNetwork:
                return WorkStatus::WaitingForNetwork;
            case MailTransferStatus::WaitingForAuth:
                return WorkStatus::WaitingForAuth;
            case MailTransferStatus::WaitingForSpace:
                return WorkStatus::WaitingForSpace;
            case MailTransferStatus::BlockedUnknown:
            case MailTransferStatus::Partial:
            case MailTransferStatus::Failed:
            case MailTransferStatus::Cancelled:
                return WorkStatus::Failed;
            case MailTransferStatus::Complete:
                return WorkStatus::Complete;
            }
            return WorkStatus::Failed;
        }

        [[nodiscard]] QString detail(const MailTransferOperationRecord& operation)
        {
            switch (operation.status)
            {
            case MailTransferStatus::Preparing:
                return i18n("Preparing transfer");
            case MailTransferStatus::Running:
                return operation.operation == MailTransferOperation::Move
                           ? i18n("Moving messages")
                           : i18n("Copying messages");
            case MailTransferStatus::WaitingForNetwork:
                return i18n("Waiting for network");
            case MailTransferStatus::WaitingForAuth:
                return i18n("Waiting for sign-in");
            case MailTransferStatus::WaitingForSpace:
                return i18n("Waiting for storage space");
            case MailTransferStatus::BlockedUnknown:
                return i18n("Waiting for transfer outcome reconciliation");
            case MailTransferStatus::Partial:
                return i18n("Transfer completed partially");
            case MailTransferStatus::Failed:
                return i18n("Transfer failed");
            case MailTransferStatus::Cancelled:
                return i18n("Transfer cancelled");
            case MailTransferStatus::Complete:
                return i18n("Transfer complete");
            }
            return {};
        }

        [[nodiscard]] WorkProgress progressFor(const MailTransferOperationRecord& operation,
                                               const std::vector<MailTransferItemRecord>& items)
        {
            std::uint64_t completedBytes = 0;
            std::uint64_t totalBytes = 0;
            std::uint64_t completedUnits = 0;
            for (const auto& item : items)
            {
                totalBytes += item.sourceSize;
                if (item.phase == MailTransferItemPhase::Complete)
                {
                    ++completedUnits;
                    completedBytes += item.sourceSize;
                }
            }
            return {
                .completedUnits = completedUnits,
                .totalUnits = static_cast<std::uint64_t>(items.size()),
                .completedBytes = completedBytes,
                .totalBytes = totalBytes,
                .detail = detail(operation),
            };
        }

        [[nodiscard]] MailTransferExecutionSummary
        summarize(const MailTransferOperationRecord& operation,
                  const std::vector<MailTransferItemRecord>& items)
        {
            MailTransferExecutionSummary summary{
                .operationId = operation.operationId,
                .status = operation.status,
                .historyEntryId = operation.historyEntryId,
            };
            for (const auto& item : items)
            {
                if (item.phase == MailTransferItemPhase::Complete)
                    ++summary.completeItemCount;
                if (item.phase == MailTransferItemPhase::DestinationConfirmed)
                    ++summary.destinationConfirmedItemCount;
                if (item.phase == MailTransferItemPhase::Failed)
                    ++summary.failedItemCount;
                if (item.phase == MailTransferItemPhase::PartialSourceRetained)
                    ++summary.partialItemCount;
                if (item.phase == MailTransferItemPhase::DestinationUnknown ||
                    item.phase == MailTransferItemPhase::SourceCleanupUnknown)
                    ++summary.unknownItemCount;
            }
            return summary;
        }

        [[nodiscard]] std::optional<javelin::jmap::OperationError>
        syncJob(WorkScheduler& scheduler, MailTransferRepository& repository,
                const std::string_view operationId, const std::string_view jobId,
                const std::optional<QString>& fallbackError = std::nullopt)
        {
            const auto operationResult = repository.findOperation(operationId);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&operationResult))
                return javelin::jmap::operationError(*error);
            const auto& operation =
                std::get<std::optional<MailTransferOperationRecord>>(operationResult);
            if (!operation.has_value())
            {
                return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::InvalidRequest,
                    .message = i18n("The mail transfer is no longer available."),
                };
            }
            const auto itemsResult = repository.listItems(operationId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&itemsResult))
                return javelin::jmap::operationError(*error);
            const auto& items = std::get<std::vector<MailTransferItemRecord>>(itemsResult);
            auto status = workStatus(operation->status);
            auto errorText =
                operation->lastError.has_value() ? operation->lastError : fallbackError;
            if (status != WorkStatus::Failed)
                errorText.reset();
            if (const auto error = scheduler.update(
                    jobId, status, progressFor(*operation, items),
                    checkpoint(operationId,
                               operation->status == MailTransferStatus::BlockedUnknown),
                    errorText))
                return javelin::jmap::operationError(*error);
            return std::nullopt;
        }

        [[nodiscard]] std::variant<MailTransferExecutionSummary, javelin::jmap::OperationError>
        waitingSummary(MailTransferRepository& repository, const std::string_view operationId,
                       javelin::jmap::OperationError fallback)
        {
            const auto operationResult = repository.findOperation(operationId);
            const auto* operation =
                std::get_if<std::optional<MailTransferOperationRecord>>(&operationResult);
            if (operation == nullptr || !operation->has_value())
                return fallback;
            if ((*operation)->status != MailTransferStatus::WaitingForNetwork &&
                (*operation)->status != MailTransferStatus::WaitingForAuth &&
                (*operation)->status != MailTransferStatus::WaitingForSpace)
                return fallback;
            const auto itemsResult = repository.listItems(operationId);
            const auto* items = std::get_if<std::vector<MailTransferItemRecord>>(&itemsResult);
            return items == nullptr
                       ? std::variant<MailTransferExecutionSummary,
                                      javelin::jmap::OperationError>{std::move(fallback)}
                       : std::variant<MailTransferExecutionSummary, javelin::jmap::OperationError>{
                             summarize(**operation, *items)};
        }
    } // namespace

    MailTransferWorkService::MailTransferWorkService(
        javelin::jmap::cache::DatabaseConnection& databaseConnection,
        javelin::jmap::api::AbstractTransport& resourceTransport,
        javelin::jmap::api::JmapMethodTransport& methodTransport,
        javelin::jmap::MessageContentClient& messageContentClient,
        const AccountConnectionProvider& connectionProvider,
        javelin::app::undo::MailTransferHistoryCoordinator& historyCoordinator,
        WorkScheduler& workScheduler, CompletionCallback completionCallback, QObject* parent)
        : QObject(parent), m_databaseConnection(databaseConnection),
          m_resourceTransport(resourceTransport), m_methodTransport(methodTransport),
          m_messageContentClient(messageContentClient), m_connectionProvider(connectionProvider),
          m_historyCoordinator(historyCoordinator), m_workScheduler(workScheduler),
          m_completionCallback(std::move(completionCallback))
    {
        connect(&m_workScheduler, &WorkScheduler::jobsChanged, this, [this] { schedulePump(); });
        connect(&m_workScheduler, &WorkScheduler::foregroundAvailabilityChanged, this,
                [this] { schedulePump(); });
    }

    QCoro::Task<MailTransferExecutionResult>
    MailTransferWorkService::advanceForeground(std::string operationId)
    {
        const auto jobId = jobIdFor(operationId);
        m_runningOperations.insert(operationId);
        const auto runningGuard = qScopeGuard(
            [this, &operationId]
            {
                m_runningOperations.erase(operationId);
                schedulePump();
            });
        ensureTracked(operationId);
        MailTransferRepository repository{m_databaseConnection};
        const auto operationResult = repository.findOperation(operationId);
        if (const auto* operation =
                std::get_if<std::optional<MailTransferOperationRecord>>(&operationResult);
            operation != nullptr && operation->has_value())
        {
            const auto itemsResult = repository.listItems(operationId);
            if (const auto* items = std::get_if<std::vector<MailTransferItemRecord>>(&itemsResult))
            {
                auto running = **operation;
                running.status = MailTransferStatus::Running;
                static_cast<void>(m_workScheduler.update(jobId, WorkStatus::Running,
                                                         progressFor(running, *items),
                                                         checkpoint(operationId)));
            }
        }
        co_return co_await executeAndTrack(operationId, jobId);
    }

    void MailTransferWorkService::networkBecameReachable()
    {
        restoreRecoverable();
        requeueWaiting(WorkStatus::WaitingForNetwork);
    }

    void MailTransferWorkService::authenticationBecameAvailable()
    {
        restoreRecoverable();
        requeueWaiting(WorkStatus::WaitingForAuth);
    }

    void MailTransferWorkService::restoreRecoverable()
    {
        m_backgroundEnabled = true;
        MailTransferRepository repository{m_databaseConnection};
        const auto recoverable = repository.listRecoverable();
        const auto* operations =
            std::get_if<std::vector<MailTransferOperationRecord>>(&recoverable);
        if (operations == nullptr)
            return;
        for (const auto& operation : *operations)
        {
            ensureTracked(operation.operationId);
            const auto jobId = jobIdFor(operation.operationId);
            if (operation.status == MailTransferStatus::Complete &&
                !operation.historyEntryId.has_value())
            {
                repairCompletedHistory(operation, jobId);
                continue;
            }
            static_cast<void>(syncJob(m_workScheduler, repository, operation.operationId, jobId,
                                      operation.lastError));
        }
        schedulePump();
    }

    void MailTransferWorkService::schedulePump()
    {
        if (m_pumpScheduled)
            return;
        m_pumpScheduled = true;
        QTimer::singleShot(0, this,
                           [this]
                           {
                               m_pumpScheduled = false;
                               pump();
                           });
    }

    void MailTransferWorkService::pump()
    {
        if (!m_backgroundEnabled)
            return;
        const auto listed = m_workScheduler.list();
        const auto* jobs = std::get_if<std::vector<WorkRecord>>(&listed);
        if (jobs == nullptr)
            return;
        for (const auto& job : *jobs)
        {
            if (job.kind != WorkKind::MailTransfer || job.status != WorkStatus::Queued ||
                job.pauseRequested)
                continue;
            const auto operationId = operationIdFor(job.jobId);
            if (!operationId.has_value() || m_runningOperations.contains(*operationId))
                continue;
            MailTransferRepository repository{m_databaseConnection};
            const auto operationResult = repository.findOperation(*operationId);
            if (const auto* operation =
                    std::get_if<std::optional<MailTransferOperationRecord>>(&operationResult);
                operation != nullptr && operation->has_value() &&
                (*operation)->status == MailTransferStatus::Complete &&
                !(*operation)->historyEntryId.has_value())
            {
                repairCompletedHistory(**operation, job.jobId);
                continue;
            }
            if (!m_workScheduler.admit(job.jobId).has_value())
                continue;
            m_runningOperations.insert(*operationId);
            auto task = runBackground(*operationId, job.jobId);
            QCoro::connect(std::move(task), this, [] {});
        }
    }

    QCoro::Task<void> MailTransferWorkService::runBackground(std::string operationId,
                                                             std::string jobId)
    {
        const auto runningGuard = qScopeGuard(
            [this, &operationId, &jobId]
            {
                m_workScheduler.release(jobId);
                m_runningOperations.erase(operationId);
                schedulePump();
            });
        MailTransferRepository repository{m_databaseConnection};
        const auto operationResult = repository.findOperation(operationId);
        if (const auto* operation =
                std::get_if<std::optional<MailTransferOperationRecord>>(&operationResult);
            operation != nullptr && operation->has_value())
        {
            const auto itemsResult = repository.listItems(operationId);
            if (const auto* items = std::get_if<std::vector<MailTransferItemRecord>>(&itemsResult))
            {
                auto running = **operation;
                running.status = MailTransferStatus::Running;
                static_cast<void>(m_workScheduler.update(jobId, WorkStatus::Running,
                                                         progressFor(running, *items),
                                                         checkpoint(operationId)));
            }
        }
        static_cast<void>(co_await executeAndTrack(operationId, jobId));
    }

    QCoro::Task<MailTransferExecutionResult>
    MailTransferWorkService::executeAndTrack(std::string operationId, std::string jobId)
    {
        MailTransferExecutor executor{m_databaseConnection, m_resourceTransport,
                                      m_methodTransport,    m_messageContentClient,
                                      m_connectionProvider, &m_historyCoordinator};
        auto result = co_await executor.advance(operationId);
        MailTransferRepository repository{m_databaseConnection};
        if (auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            auto normalized = waitingSummary(repository, operationId, *error);
            if (const auto* summary = std::get_if<MailTransferExecutionSummary>(&normalized))
                result = *summary;
        }
        const auto fallbackError =
            std::holds_alternative<javelin::jmap::OperationError>(result)
                ? std::optional<QString>{std::get<javelin::jmap::OperationError>(result).message}
                : std::nullopt;
        if (const auto error =
                syncJob(m_workScheduler, repository, operationId, jobId, fallbackError))
        {
            if (std::holds_alternative<MailTransferExecutionSummary>(result))
                co_return *error;
        }
        if (const auto* executionError = std::get_if<javelin::jmap::OperationError>(&result))
        {
            const auto operationResult = repository.findOperation(operationId);
            if (const auto* operation =
                    std::get_if<std::optional<MailTransferOperationRecord>>(&operationResult);
                operation != nullptr && operation->has_value() &&
                (*operation)->status == MailTransferStatus::Complete &&
                !(*operation)->historyEntryId.has_value())
            {
                const auto itemsResult = repository.listItems(operationId);
                if (const auto* items =
                        std::get_if<std::vector<MailTransferItemRecord>>(&itemsResult))
                {
                    auto progress = progressFor(**operation, *items);
                    progress.detail = i18n("Transfer complete; Undo history needs repair");
                    static_cast<void>(m_workScheduler.update(jobId, WorkStatus::Failed, progress,
                                                             checkpoint(operationId, true),
                                                             executionError->message));
                }
            }
        }
        if (m_completionCallback && std::holds_alternative<MailTransferExecutionSummary>(result) &&
            std::get<MailTransferExecutionSummary>(result).status == MailTransferStatus::Complete)
        {
            const auto completedOperation = repository.findOperation(operationId);
            if (const auto* operation =
                    std::get_if<std::optional<MailTransferOperationRecord>>(&completedOperation);
                operation != nullptr && operation->has_value())
                m_completionCallback(**operation);
        }
        co_return result;
    }

    void MailTransferWorkService::ensureTracked(const std::string_view operationId)
    {
        MailTransferRepository repository{m_databaseConnection};
        const auto operationResult = repository.findOperation(operationId);
        const auto* operation =
            std::get_if<std::optional<MailTransferOperationRecord>>(&operationResult);
        if (operation == nullptr || !operation->has_value())
            return;
        const auto jobId = jobIdFor(operationId);
        if (const auto error = m_workScheduler.ensure({
                .jobId = jobId,
                .parentJobId = std::nullopt,
                .accountId = (*operation)->sourceAccountId,
                .kind = WorkKind::MailTransfer,
                .priority = WorkPriority::Foreground,
                .title = (*operation)->title,
                .checkpointJson = checkpoint(operationId),
            }))
        {
            qWarning().noquote() << "Could not expose mail transfer as background work"
                                 << error->message;
        }
    }

    void
    MailTransferWorkService::repairCompletedHistory(const MailTransferOperationRecord& operation,
                                                    const std::string_view jobId)
    {
        MailTransferRepository repository{m_databaseConnection};
        const auto finalized = m_historyCoordinator.finalizeCompleted(operation.operationId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&finalized))
        {
            const auto itemsResult = repository.listItems(operation.operationId);
            if (const auto* items = std::get_if<std::vector<MailTransferItemRecord>>(&itemsResult))
            {
                auto progress = progressFor(operation, *items);
                progress.detail = i18n("Transfer complete; Undo history needs repair");
                static_cast<void>(m_workScheduler.update(jobId, WorkStatus::Failed, progress,
                                                         checkpoint(operation.operationId, true),
                                                         error->message));
            }
            return;
        }
        static_cast<void>(syncJob(m_workScheduler, repository, operation.operationId, jobId));
        if (!m_completionCallback)
            return;
        const auto refreshed = repository.findOperation(operation.operationId);
        if (const auto* value = std::get_if<std::optional<MailTransferOperationRecord>>(&refreshed);
            value != nullptr && value->has_value())
            m_completionCallback(**value);
    }

    void MailTransferWorkService::requeueWaiting(const WorkStatus status)
    {
        const auto listed = m_workScheduler.list();
        const auto* jobs = std::get_if<std::vector<WorkRecord>>(&listed);
        if (jobs == nullptr)
            return;
        for (const auto& job : *jobs)
        {
            if (job.kind != WorkKind::MailTransfer || job.status != status)
                continue;
            static_cast<void>(m_workScheduler.update(job.jobId, WorkStatus::Queued, job.progress,
                                                     job.checkpointJson));
        }
        schedulePump();
    }

} // namespace javelin::app
