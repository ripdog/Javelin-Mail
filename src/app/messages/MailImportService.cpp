#include "app/MailImportService.h"

#include "app/AccountConnectionProvider.h"
#include "app/AccountConnectionSettings.h"
#include "app/MailImportPlanner.h"
#include "app/MailImportRepository.h"
#include "app/MailImportSource.h"
#include "app/WorkScheduler.h"
#include "jmap/EmailMutation.h"
#include "jmap/OperationError.h"
#include "jmap/api/BlobUpload.h"
#include "jmap/api/MailMethods.h"
#include "jmap/api/MethodCaller.h"
#include "jmap/api/RequestBuilder.h"
#include "jmap/api/ResponseReader.h"
#include "jmap/cache/AccountRepository.h"
#include "jmap/cache/MailboxReadRepository.h"
#include "jmap/cache/MailboxRepository.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/sync/EmailMutationEngine.h"
#include "jmap/sync/EmailMutationJournal.h"
#include "jmap/sync/LongPollWorker.h"
#include "jmap/sync/MailboxMutationEngine.h"

#include <KLocalizedString>

#include <QCoroFuture>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QTimer>
#include <QUuid>
#include <QtConcurrentRun>

#include <algorithm>
#include <chrono>
#include <ranges>
#include <utility>

namespace javelin::app
{
    namespace
    {
        using DatabaseError = javelin::jmap::cache::DatabaseError;
        using OperationError = javelin::jmap::OperationError;
        using OperationErrorCode = javelin::jmap::OperationErrorCode;

        constexpr std::string_view jobPrefix = "mail-import:";
        constexpr std::size_t maximumChangePages = 32;
        constexpr std::uint64_t changesPageSize = 256;
        constexpr std::uint64_t synchronizationIntervalItems = 50;

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

        [[nodiscard]] QString checkpoint(const std::string_view operationId)
        {
            return QString::fromUtf8(
                QJsonDocument{QJsonObject{{QStringLiteral("operationId"),
                                           QString::fromStdString(std::string{operationId})}}}
                    .toJson(QJsonDocument::Compact));
        }

        [[nodiscard]] javelin::jmap::LiveConnectionSettings
        liveSettings(const AccountConnectionSettings& settings)
        {
            return {
                .sessionUrl = settings.sessionUrl,
                .loginEmail = settings.loginEmail,
                .apiKey = settings.apiKey,
            };
        }

        [[nodiscard]] javelin::jmap::api::ApiRequestContext
        requestContext(const AccountConnectionSettings& settings,
                       const std::string_view localAccountId,
                       const javelin::jmap::api::Session& session)
        {
            return {
                .credentials =
                    {
                        .accountId = std::string{localAccountId},
                        .emailAddress = settings.loginEmail,
                        .sessionUrl = settings.sessionUrl,
                        .token = {.accessToken = settings.apiKey,
                                  .refreshToken = std::nullopt,
                                  .expiry = std::nullopt},
                    },
                .apiUrl = session.apiUrl,
                .requestLimits = javelin::jmap::api::coreRequestLimits(session),
            };
        }

        [[nodiscard]] bool retryable(const OperationError& error)
        {
            return javelin::jmap::isAuthenticationError(error) ||
                   javelin::jmap::isTransientError(error);
        }

        [[nodiscard]] MailImportStatus waitStatus(const OperationError& error)
        {
            return javelin::jmap::isAuthenticationError(error)
                       ? MailImportStatus::WaitingForAuth
                       : MailImportStatus::WaitingForNetwork;
        }

        [[nodiscard]] WorkStatus workStatus(const MailImportStatus status)
        {
            switch (status)
            {
            case MailImportStatus::Preparing:
            case MailImportStatus::Running:
                return WorkStatus::Queued;
            case MailImportStatus::WaitingForNetwork:
                return WorkStatus::WaitingForNetwork;
            case MailImportStatus::WaitingForAuth:
                return WorkStatus::WaitingForAuth;
            case MailImportStatus::WaitingForSpace:
                return WorkStatus::WaitingForSpace;
            case MailImportStatus::Complete:
            case MailImportStatus::Partial:
                return WorkStatus::Complete;
            case MailImportStatus::BlockedUnknown:
            case MailImportStatus::Failed:
                return WorkStatus::Failed;
            }
            return WorkStatus::Failed;
        }

        [[nodiscard]] QString detail(const MailImportOperationRecord& operation)
        {
            switch (operation.status)
            {
            case MailImportStatus::Preparing:
                return i18n("Scanning import sources");
            case MailImportStatus::Running:
                return operation.scanSealed ? i18n("Importing messages")
                                            : i18n("Scanning import sources");
            case MailImportStatus::WaitingForNetwork:
                return i18n("Waiting for network");
            case MailImportStatus::WaitingForAuth:
                return i18n("Waiting for sign-in");
            case MailImportStatus::WaitingForSpace:
                return i18n("Destination account is over quota");
            case MailImportStatus::BlockedUnknown:
                return i18n("Import needs outcome reconciliation");
            case MailImportStatus::Partial:
                return i18n("Import completed partially");
            case MailImportStatus::Failed:
                return i18n("Import failed");
            case MailImportStatus::Complete:
                return i18n("Import complete");
            }
            return {};
        }

        [[nodiscard]] WorkProgress progressFor(const MailImportOperationRecord& operation,
                                               const MailImportProgressSnapshot& progress)
        {
            return {
                .completedUnits = progress.completedItems,
                .totalUnits = operation.scanSealed
                                  ? std::optional<std::uint64_t>{progress.totalItems}
                                  : std::nullopt,
                .completedBytes = progress.completedBytes,
                .totalBytes = operation.scanSealed
                                  ? std::optional<std::uint64_t>{progress.totalBytes}
                                  : std::nullopt,
                .detail = detail(operation),
            };
        }

        [[nodiscard]] std::optional<OperationError>
        syncJob(WorkScheduler& scheduler, MailImportRepository& repository,
                const MailImportOperationRecord& operation, const std::string_view jobId,
                const bool admitted = false)
        {
            const auto progressResult = repository.progress(operation.operationId);
            if (const auto* error = std::get_if<DatabaseError>(&progressResult))
                return javelin::jmap::operationError(*error);
            const auto& progress = std::get<MailImportProgressSnapshot>(progressResult);

            auto status = workStatus(operation.status);
            const auto jobResult = scheduler.find(jobId);
            if (const auto* error = std::get_if<DatabaseError>(&jobResult))
                return javelin::jmap::operationError(*error);
            if (const auto& job = std::get<std::optional<WorkRecord>>(jobResult);
                job.has_value() && job->pauseRequested)
            {
                status = WorkStatus::Paused;
            }
            else if (admitted && (operation.status == MailImportStatus::Preparing ||
                                  operation.status == MailImportStatus::Running))
            {
                status = WorkStatus::Running;
            }

            if (const auto error =
                    scheduler.update(jobId, status, progressFor(operation, progress),
                                     checkpoint(operation.operationId), operation.lastError))
                return javelin::jmap::operationError(*error);
            return std::nullopt;
        }

        [[nodiscard]] OperationError invalidImport(QString message)
        {
            return {.code = OperationErrorCode::InvalidUserInput, .message = std::move(message)};
        }

        [[nodiscard]] std::variant<javelin::jmap::api::Session, OperationError>
        requireSession(javelin::jmap::cache::DatabaseConnection& database,
                       const std::string& localAccountId)
        {
            const auto result =
                javelin::jmap::cache::SessionRepository{database}.load(localAccountId);
            if (const auto* error = std::get_if<DatabaseError>(&result))
                return javelin::jmap::operationError(*error);
            const auto& session = std::get<std::optional<javelin::jmap::api::Session>>(result);
            if (!session.has_value())
                return OperationError{
                    .code = OperationErrorCode::NetworkUnavailable,
                    .message = i18n("No cached JMAP session is available for this import.")};
            return *session;
        }

        [[nodiscard]] std::variant<javelin::jmap::cache::CachedAccount, OperationError>
        requireAccount(javelin::jmap::cache::DatabaseConnection& database,
                       const std::string& localAccountId)
        {
            const auto result =
                javelin::jmap::cache::AccountRepository{database}.findById(localAccountId);
            if (const auto* error = std::get_if<DatabaseError>(&result))
                return javelin::jmap::operationError(*error);
            const auto& account =
                std::get<std::optional<javelin::jmap::cache::CachedAccount>>(result);
            if (!account.has_value() || account->remoteAccountId.empty())
                return OperationError{
                    .code = OperationErrorCode::NotFound,
                    .message = i18n("The mail account is no longer available for import.")};
            return *account;
        }

        [[nodiscard]] OperationError
        callerError(const javelin::jmap::api::MethodCallerResult& result)
        {
            if (const auto* error = std::get_if<javelin::jmap::api::TransportError>(&result))
                return javelin::jmap::operationError(*error);
            if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&result))
                return javelin::jmap::operationError(*error);
            if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&result))
                return javelin::jmap::operationError(*error);
            return {.code = OperationErrorCode::ProtocolViolation,
                    .message = i18n("The JMAP import request returned an invalid result.")};
        }

        [[nodiscard]] std::variant<std::string, OperationError>
        hashImportItem(const MailImportItemRecord& item)
        {
            std::unique_ptr<QIODevice> device;
            if (item.sourceKind == MailImportFileKind::Eml)
            {
                auto file = std::make_unique<QFile>(item.sourcePath);
                if (!file->open(QIODevice::ReadOnly))
                    return OperationError{.code = OperationErrorCode::LocalStorageFailure,
                                          .message = i18n("Could not read import source %1: %2",
                                                          item.sourcePath, file->errorString())};
                device = std::move(file);
            }
            else
            {
                if (!item.contentOffset.has_value() || !item.contentEnd.has_value())
                    return OperationError{.code = OperationErrorCode::PreconditionFailed,
                                          .message = i18n("The mbox import record is incomplete.")};
                auto record = std::make_unique<MailImportMboxRecordDevice>(
                    item.sourcePath, MailImportMboxRecord{.ordinal = item.ordinal,
                                                          .contentOffset = *item.contentOffset,
                                                          .contentEnd = *item.contentEnd,
                                                          .decodedSize = item.decodedSize,
                                                          .receivedAt = item.receivedAt});
                if (!record->openReadOnly())
                    return OperationError{
                        .code = OperationErrorCode::LocalStorageFailure,
                        .message = i18n("Could not open mbox record in %1.", item.sourcePath)};
                device = std::move(record);
            }

            QCryptographicHash hash{QCryptographicHash::Sha256};
            QByteArray buffer;
            buffer.resize(1024 * 1024);
            while (true)
            {
                const auto count = device->read(buffer.data(), buffer.size());
                if (count < 0)
                    return OperationError{
                        .code = OperationErrorCode::LocalStorageFailure,
                        .message = i18n("Could not hash import source %1.", item.sourcePath)};
                if (count == 0)
                    break;
                hash.addData(QByteArrayView{buffer.constData(), count});
            }
            return hash.result().toHex().toStdString();
        }

        [[nodiscard]] bool sourceStillMatches(const MailImportItemRecord& item)
        {
            const auto current = mailImportSourceFingerprint(item.sourcePath);
            const auto* fingerprint = std::get_if<MailImportSourceFingerprint>(&current);
            return fingerprint != nullptr && *fingerprint == item.sourceFingerprint;
        }
    } // namespace

    MailImportService::MailImportService(
        javelin::jmap::cache::DatabaseConnection& databaseConnection,
        javelin::jmap::api::AbstractTransport& resourceTransport,
        javelin::jmap::api::JmapMethodTransport& methodTransport,
        const AccountConnectionProvider& connectionProvider, WorkScheduler& workScheduler,
        std::function<void(std::string_view, std::string_view)> requestMailboxResync,
        MailImportScheduling scheduling, QObject* parent)
        : QObject(parent), m_databaseConnection(databaseConnection),
          m_resourceTransport(resourceTransport), m_methodTransport(methodTransport),
          m_connectionProvider(connectionProvider), m_workScheduler(workScheduler),
          m_requestMailboxResync(std::move(requestMailboxResync))
    {
        m_defer = scheduling.defer ? std::move(scheduling.defer)
                                   : [this](std::function<void()> callback)
        { QTimer::singleShot(0, this, std::move(callback)); };
        m_retry = scheduling.retry ? std::move(scheduling.retry)
                                   : [this](const std::chrono::milliseconds delay,
                                            std::function<void()> callback)
        { QTimer::singleShot(delay, this, std::move(callback)); };
        connect(&m_workScheduler, &WorkScheduler::jobsChanged, this, [this] { schedulePump(); });
        connect(&m_workScheduler, &WorkScheduler::foregroundAvailabilityChanged, this,
                [this] { schedulePump(); });
    }

    QCoro::Task<MailImportStartResult> MailImportService::startImport(MailImportIntent intent)
    {
        if (intent.accountId.empty() || intent.sourcePaths.empty())
            co_return invalidImport(i18n("Choose mail files or a directory to import."));
        if (!intent.recreateHierarchy && !intent.mailboxId.has_value())
            co_return invalidImport(i18n("Choose a destination mailbox for the import."));
        if (intent.recreateHierarchy && intent.sourcePaths.size() != 1)
            co_return invalidImport(i18n("Hierarchy recreation accepts one directory at a time."));
        if (!m_connectionProvider.connectionSettingsFor(intent.accountId).has_value())
            co_return OperationError{.code = OperationErrorCode::NotFound,
                                     .message = i18n("The selected mail account is unavailable.")};

        QString destinationName;
        if (intent.mailboxId.has_value())
        {
            const auto mailboxesResult =
                javelin::jmap::cache::MailboxReadRepository{m_databaseConnection}.listMailboxTree(
                    intent.accountId);
            if (const auto* error = std::get_if<DatabaseError>(&mailboxesResult))
                co_return javelin::jmap::operationError(*error);
            const auto& mailboxes =
                std::get<std::vector<javelin::jmap::cache::MailboxTreeItem>>(mailboxesResult);
            const auto mailbox = std::ranges::find(mailboxes, *intent.mailboxId,
                                                   &javelin::jmap::cache::MailboxTreeItem::id);
            if (mailbox == mailboxes.end() || mailbox->pendingCreate ||
                !mailbox->myRights.mayAddItems)
                co_return OperationError{
                    .code = OperationErrorCode::PermissionDenied,
                    .message = i18n("The selected mailbox does not allow adding messages.")};
            if (intent.recreateHierarchy && !mailbox->myRights.mayCreateChild)
                co_return OperationError{
                    .code = OperationErrorCode::PermissionDenied,
                    .message =
                        i18n("The selected mailbox does not allow creating child mailboxes.")};
            destinationName = QString::fromStdString(mailbox->name);
        }

        for (const auto& path : intent.sourcePaths)
        {
            if (!QDir::isAbsolutePath(path))
                co_return invalidImport(i18n("Import source paths must be absolute: %1", path));
            const QFileInfo info{path};
            if (!info.exists() || !info.isReadable() || info.isSymLink())
                co_return invalidImport(
                    i18n("Import source is unavailable or is a symbolic link: %1", path));
        }

        const auto operationId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        MailImportOperationRecord operation{
            .operationId = operationId,
            .accountId = intent.accountId,
            .mailboxId = intent.mailboxId,
            .sourcePaths = std::move(intent.sourcePaths),
            .recreateHierarchy = intent.recreateHierarchy,
            .status = MailImportStatus::Preparing,
            .scanSealed = false,
            .title = destinationName.isEmpty() ? i18n("Import mail")
                                               : i18n("Import mail into “%1”", destinationName),
            .createdAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs),
            .lastError = std::nullopt,
        };
        MailImportRepository repository{m_databaseConnection};
        if (const auto error = repository.createOperation(operation))
            co_return javelin::jmap::operationError(*error);
        if (const auto trackingError = ensureTracked(operationId))
        {
            static_cast<void>(repository.setStatus(operationId, MailImportStatus::Failed,
                                                   trackingError->message));
            co_return *trackingError;
        }
        m_backgroundEnabled = true;
        schedulePump();
        co_return MailImportAdmission{.operationId = operationId, .jobId = jobIdFor(operationId)};
    }

    void MailImportService::restoreRecoverable()
    {
        m_backgroundEnabled = true;
        MailImportRepository repository{m_databaseConnection};
        const auto result = repository.listRecoverable();
        const auto* operations = std::get_if<std::vector<MailImportOperationRecord>>(&result);
        if (operations == nullptr)
            return;
        for (const auto& operation : *operations)
        {
            if (ensureTracked(operation.operationId).has_value())
                continue;
            static_cast<void>(
                syncJob(m_workScheduler, repository, operation, jobIdFor(operation.operationId)));
            if (operation.status == MailImportStatus::WaitingForNetwork)
            {
                scheduleTransientRetry(
                    operation.operationId,
                    OperationError{.code = OperationErrorCode::NetworkUnavailable,
                                   .message = operation.lastError.value_or(
                                       i18n("Mail import is waiting for a transient retry."))});
            }
        }
        schedulePump();
    }

    void MailImportService::networkBecameReachable()
    {
        restoreRecoverable();
        requeueWaiting(false);
    }

    void MailImportService::authenticationBecameAvailable()
    {
        restoreRecoverable();
        requeueWaiting(true);
    }

    void MailImportService::schedulePump()
    {
        if (m_pumpScheduled)
            return;
        m_pumpScheduled = true;
        m_defer(
            [this]
            {
                m_pumpScheduled = false;
                pump();
            });
    }

    void MailImportService::pump()
    {
        if (!m_backgroundEnabled)
            return;
        const auto listed = m_workScheduler.list();
        const auto* jobs = std::get_if<std::vector<WorkRecord>>(&listed);
        if (jobs == nullptr)
            return;
        for (const auto& job : *jobs)
        {
            if (job.kind != WorkKind::MailImport || job.status != WorkStatus::Queued ||
                job.pauseRequested)
                continue;
            const auto operationId = operationIdFor(job.jobId);
            if (!operationId.has_value() || m_runningOperations.contains(*operationId))
                continue;
            if (!m_workScheduler.admit(job.jobId).has_value())
                continue;
            m_runningOperations.insert(*operationId);
            auto task = runOne(*operationId, job.jobId);
            QCoro::connect(std::move(task), this, [] {});
        }
    }

    std::optional<OperationError>
    MailImportService::ensureTracked(const std::string_view operationId)
    {
        MailImportRepository repository{m_databaseConnection};
        const auto result = repository.findOperation(operationId);
        if (const auto* error = std::get_if<DatabaseError>(&result))
            return javelin::jmap::operationError(*error);
        const auto& operation = std::get<std::optional<MailImportOperationRecord>>(result);
        if (!operation.has_value())
            return OperationError{.code = OperationErrorCode::NotFound,
                                  .message = i18n("The mail import job is no longer available.")};
        const auto jobId = jobIdFor(operationId);
        if (const auto error = m_workScheduler.ensure({
                .jobId = jobId,
                .parentJobId = std::nullopt,
                .accountId = operation->accountId,
                .kind = WorkKind::MailImport,
                .priority = WorkPriority::Bulk,
                .title = operation->title,
                .checkpointJson = checkpoint(operationId),
            }))
            return javelin::jmap::operationError(*error);
        return std::nullopt;
    }

    void MailImportService::requeueWaiting(const bool authentication)
    {
        const auto listed = m_workScheduler.list();
        const auto* jobs = std::get_if<std::vector<WorkRecord>>(&listed);
        if (jobs == nullptr)
            return;
        const auto wanted =
            authentication ? WorkStatus::WaitingForAuth : WorkStatus::WaitingForNetwork;
        for (const auto& job : *jobs)
        {
            if (job.kind != WorkKind::MailImport || job.status != wanted)
                continue;
            const auto operationId = operationIdFor(job.jobId);
            if (!operationId.has_value())
                continue;
            requeueWaitingOperation(*operationId, authentication);
        }
    }

    void MailImportService::scheduleTransientRetry(std::string operationId,
                                                   const OperationError& error,
                                                   const bool authentication)
    {
        auto& state = m_transientRetries[operationId];
        if (state.scheduled && state.authentication == authentication)
            return;
        ++state.attempts;
        ++state.generation;
        state.scheduled = true;
        state.authentication = authentication;

        auto delay = javelin::jmap::sync::BackoffPolicy{}.delayForAttempt(state.attempts);
        if (error.retryAfter.has_value())
        {
            delay = std::max(
                delay, std::chrono::duration_cast<std::chrono::milliseconds>(*error.retryAfter));
        }
        const auto generation = state.generation;
        m_retry(delay,
                [this, operationId = std::move(operationId), generation, authentication]
                {
                    const auto found = m_transientRetries.find(operationId);
                    if (found == m_transientRetries.end() || found->second.generation != generation)
                        return;
                    found->second.scheduled = false;
                    requeueWaitingOperation(operationId, authentication);
                });
    }

    void MailImportService::resetTransientRetry(const std::string_view operationId)
    {
        m_transientRetries.erase(std::string{operationId});
    }

    void MailImportService::requeueWaitingOperation(const std::string_view operationId,
                                                    const bool authentication)
    {
        MailImportRepository repository{m_databaseConnection};
        const auto operationResult = repository.findOperation(operationId);
        const auto* operation =
            std::get_if<std::optional<MailImportOperationRecord>>(&operationResult);
        if (operation == nullptr || !operation->has_value())
            return;
        const auto readyStatus =
            (*operation)->scanSealed ? MailImportStatus::Running : MailImportStatus::Preparing;
        const auto waitingStatus =
            authentication ? MailImportStatus::WaitingForAuth : MailImportStatus::WaitingForNetwork;
        if ((*operation)->status != waitingStatus && (*operation)->status != readyStatus)
            return;

        const auto jobId = jobIdFor(operationId);
        const auto jobResult = m_workScheduler.find(jobId);
        const auto* job = std::get_if<std::optional<WorkRecord>>(&jobResult);
        const auto waitingWorkStatus =
            authentication ? WorkStatus::WaitingForAuth : WorkStatus::WaitingForNetwork;
        if (job == nullptr || !job->has_value() || (*job)->pauseRequested)
            return;
        const auto workStatus = (*job)->status;
        if (workStatus != waitingWorkStatus && workStatus != WorkStatus::Running)
            return;

        if ((*operation)->status == waitingStatus)
        {
            if (const auto error = repository.setStatus(operationId, readyStatus))
            {
                scheduleTransientRetry(std::string{operationId},
                                       javelin::jmap::operationError(*error), authentication);
                return;
            }
        }
        if (const auto error = m_workScheduler.update(jobId, WorkStatus::Queued, (*job)->progress,
                                                      (*job)->checkpointJson))
        {
            scheduleTransientRetry(std::string{operationId}, javelin::jmap::operationError(*error),
                                   authentication);
            return;
        }
        resetTransientRetry(operationId);
        schedulePump();
    }

    QCoro::Task<void> MailImportService::runOne(std::string operationId, std::string jobId)
    {
        const auto guard = qScopeGuard(
            [this, &operationId, &jobId]
            {
                m_workScheduler.release(jobId);
                m_runningOperations.erase(operationId);
                schedulePump();
            });
        MailImportRepository repository{m_databaseConnection};
        const auto before = repository.findOperation(operationId);
        if (const auto* operation = std::get_if<std::optional<MailImportOperationRecord>>(&before);
            operation != nullptr && operation->has_value())
        {
            if ((*operation)->status == MailImportStatus::WaitingForNetwork)
                resetTransientRetry(operationId);
            if ((*operation)->status == MailImportStatus::WaitingForNetwork ||
                (*operation)->status == MailImportStatus::WaitingForAuth ||
                (*operation)->status == MailImportStatus::WaitingForSpace ||
                (*operation)->status == MailImportStatus::BlockedUnknown ||
                (*operation)->status == MailImportStatus::Failed)
                static_cast<void>(repository.setStatus(
                    operationId, (*operation)->scanSealed ? MailImportStatus::Running
                                                          : MailImportStatus::Preparing));
            const auto current = repository.findOperation(operationId);
            if (const auto* refreshed =
                    std::get_if<std::optional<MailImportOperationRecord>>(&current);
                refreshed != nullptr && refreshed->has_value())
            {
                const auto progress = repository.progress(operationId);
                if (const auto* snapshot = std::get_if<MailImportProgressSnapshot>(&progress))
                    static_cast<void>(m_workScheduler.update(jobId, WorkStatus::Running,
                                                             progressFor(**refreshed, *snapshot),
                                                             checkpoint(operationId)));
            }
        }
        while (true)
        {
            const auto jobResult = m_workScheduler.find(jobId);
            const auto* job = std::get_if<std::optional<WorkRecord>>(&jobResult);
            if (job == nullptr || !job->has_value() || (*job)->pauseRequested)
                co_return;

            const auto operationResult = repository.findOperation(operationId);
            const auto* operation =
                std::get_if<std::optional<MailImportOperationRecord>>(&operationResult);
            if (operation == nullptr || !operation->has_value())
                co_return;
            if ((*operation)->status != MailImportStatus::Preparing &&
                (*operation)->status != MailImportStatus::Running)
                co_return;

            co_await advanceOne(operationId, jobId);
        }
    }

    QCoro::Task<void> MailImportService::advanceOne(std::string operationId, std::string jobId)
    {
        MailImportRepository repository{m_databaseConnection};
        auto result = repository.findOperation(operationId);
        auto* operation = std::get_if<std::optional<MailImportOperationRecord>>(&result);
        if (operation == nullptr || !operation->has_value())
            co_return;

        std::optional<OperationError> error;
        if (!(*operation)->scanSealed)
            error = co_await prepareScan(**operation, jobId);
        else
        {
            const auto mailboxResult = repository.nextPendingMailbox(operationId);
            if (const auto* databaseError = std::get_if<DatabaseError>(&mailboxResult))
                error = javelin::jmap::operationError(*databaseError);
            else if (std::get<std::optional<MailImportMailboxRecord>>(mailboxResult).has_value())
                error = co_await resolveNextMailbox(**operation, jobId);
            else
                error = co_await processNextItem(**operation, jobId);
        }
        if (error.has_value())
        {
            if (retryable(*error))
            {
                const bool authentication = javelin::jmap::isAuthenticationError(*error);
                if (const auto statusError =
                        repository.setStatus(operationId, waitStatus(*error), error->message))
                {
                    scheduleTransientRetry(operationId, javelin::jmap::operationError(*statusError),
                                           authentication);
                }
                else if (authentication)
                {
                    resetTransientRetry(operationId);
                }
                else
                {
                    scheduleTransientRetry(operationId, *error);
                }
            }
            else
            {
                const auto refreshed = repository.findOperation(operationId);
                const auto* current =
                    std::get_if<std::optional<MailImportOperationRecord>>(&refreshed);
                if (current != nullptr && current->has_value() &&
                    (*current)->status != MailImportStatus::BlockedUnknown &&
                    (*current)->status != MailImportStatus::WaitingForSpace)
                    static_cast<void>(repository.setStatus(operationId, MailImportStatus::Failed,
                                                           error->message));
                resetTransientRetry(operationId);
            }
        }
        else
        {
            resetTransientRetry(operationId);
        }

        result = repository.findOperation(operationId);
        operation = std::get_if<std::optional<MailImportOperationRecord>>(&result);
        if (operation != nullptr && operation->has_value())
        {
            const auto syncError = syncJob(m_workScheduler, repository, **operation, jobId, true);
            const auto currentStatus = (*operation)->status;
            if (syncError.has_value() && (currentStatus == MailImportStatus::WaitingForNetwork ||
                                          currentStatus == MailImportStatus::WaitingForAuth))
            {
                scheduleTransientRetry(operationId, *syncError,
                                       currentStatus == MailImportStatus::WaitingForAuth);
            }
            bool synchronize = currentStatus != MailImportStatus::Preparing &&
                               currentStatus != MailImportStatus::Running;
            if (!synchronize && currentStatus == MailImportStatus::Running)
            {
                const auto progress = repository.progress(operationId);
                if (const auto* snapshot = std::get_if<MailImportProgressSnapshot>(&progress))
                    synchronize = snapshot->completedItems > 0 &&
                                  snapshot->completedItems % synchronizationIntervalItems == 0;
            }
            if (synchronize)
                requestOperationSynchronization(**operation);
        }
    }

    QCoro::Task<std::optional<OperationError>>
    MailImportService::prepareScan(MailImportOperationRecord operation, std::string jobId)
    {
        const auto job = m_workScheduler.find(jobId);
        if (const auto* record = std::get_if<std::optional<WorkRecord>>(&job);
            record != nullptr && record->has_value() &&
            ((*record)->pauseRequested || (*record)->status == WorkStatus::Paused))
            co_return std::nullopt;

        auto scan =
            co_await QtConcurrent::run([operation] { return planMailImportSources(operation); });
        if (const auto* error = std::get_if<OperationError>(&scan))
            co_return *error;
        auto plan = std::get<MailImportScanPlan>(std::move(scan));

        if (operation.recreateHierarchy && operation.mailboxId.has_value())
        {
            for (auto& item : plan.items)
            {
                if (item.destinationRelativePath.has_value() &&
                    item.destinationRelativePath->isEmpty())
                    item.resolvedMailboxId = operation.mailboxId;
            }
        }
        MailImportRepository repository{m_databaseConnection};
        if (const auto error =
                repository.replaceScan(operation.operationId, plan.mailboxes, plan.items))
            co_return javelin::jmap::operationError(*error);
        co_return std::nullopt;
    }

    QCoro::Task<std::optional<OperationError>>
    MailImportService::resolveNextMailbox(MailImportOperationRecord operation, std::string jobId)
    {
        MailImportRepository repository{m_databaseConnection};
        const auto nextResult = repository.nextPendingMailbox(operation.operationId);
        if (const auto* error = std::get_if<DatabaseError>(&nextResult))
            co_return javelin::jmap::operationError(*error);
        const auto& next = std::get<std::optional<MailImportMailboxRecord>>(nextResult);
        if (!next.has_value())
            co_return std::nullopt;

        std::optional<std::string> parentId = operation.mailboxId;
        if (next->parentRelativePath.has_value())
        {
            const auto mailboxesResult = repository.listMailboxes(operation.operationId);
            if (const auto* error = std::get_if<DatabaseError>(&mailboxesResult))
                co_return javelin::jmap::operationError(*error);
            const auto& mailboxes = std::get<std::vector<MailImportMailboxRecord>>(mailboxesResult);
            const auto parent = std::ranges::find(mailboxes, *next->parentRelativePath,
                                                  &MailImportMailboxRecord::relativePath);
            if (parent == mailboxes.end() || !parent->resolvedMailboxId.has_value())
            {
                const auto message =
                    i18n("The parent mailbox for “%1” could not be created.", next->displayName);
                if (const auto error =
                        repository.failMailbox(operation.operationId, next->relativePath, message))
                    co_return javelin::jmap::operationError(*error);
                if (const auto error = repository.propagateMailboxResolution(
                        operation.operationId, next->relativePath, std::nullopt, message))
                    co_return javelin::jmap::operationError(*error);
                co_return std::nullopt;
            }
            parentId = parent->resolvedMailboxId;
        }

        const auto settings = m_connectionProvider.connectionSettingsFor(operation.accountId);
        if (!settings.has_value())
            co_return OperationError{.code = OperationErrorCode::NotFound,
                                     .message = i18n("The mail account is no longer available.")};

        javelin::jmap::MailboxMutationEngine engine{m_databaseConnection, m_methodTransport};
        auto reconciled =
            co_await engine.reconcileCreate(liveSettings(*settings), operation.accountId, jobId);
        if (const auto* error = std::get_if<OperationError>(&reconciled))
        {
            if (retryable(*error))
            {
                static_cast<void>(repository.setStatus(operation.operationId, waitStatus(*error),
                                                       error->message));
                co_return *error;
            }
            static_cast<void>(repository.setStatus(
                operation.operationId, MailImportStatus::BlockedUnknown, error->message));
            co_return std::nullopt;
        }
        const auto& recoveredCreate = std::get<javelin::jmap::MailboxCreateChange>(reconciled);
        if (!recoveredCreate.mailboxId.empty())
        {
            if (recoveredCreate.name != next->displayName.toStdString())
            {
                const auto message =
                    i18n("A recovered mailbox creation did not match the import hierarchy.");
                static_cast<void>(repository.setStatus(operation.operationId,
                                                       MailImportStatus::BlockedUnknown, message));
                co_return std::nullopt;
            }
            if (const auto error = repository.resolveMailbox(
                    operation.operationId, next->relativePath, MailImportMailboxPhase::Created,
                    recoveredCreate.mailboxId))
                co_return javelin::jmap::operationError(*error);
            if (const auto error = repository.propagateMailboxResolution(
                    operation.operationId, next->relativePath,
                    std::optional<std::string_view>{recoveredCreate.mailboxId}))
                co_return javelin::jmap::operationError(*error);
            co_return std::nullopt;
        }

        javelin::jmap::cache::MailboxRepository mailboxRepository{m_databaseConnection};
        const auto siblings = mailboxRepository.listByParent(
            operation.accountId,
            parentId.has_value() ? std::optional<std::string_view>{*parentId} : std::nullopt);
        if (const auto* error = std::get_if<DatabaseError>(&siblings))
            co_return javelin::jmap::operationError(*error);
        const auto& siblingList = std::get<std::vector<javelin::jmap::domain::Mailbox>>(siblings);
        const auto existing = std::ranges::find(siblingList, next->displayName.toStdString(),
                                                &javelin::jmap::domain::Mailbox::name);
        if (existing != siblingList.end())
        {
            if (!existing->myRights.mayAddItems)
            {
                const auto message = i18n("Existing mailbox “%1” does not allow adding messages.",
                                          next->displayName);
                if (const auto error =
                        repository.failMailbox(operation.operationId, next->relativePath, message))
                    co_return javelin::jmap::operationError(*error);
                if (const auto error = repository.propagateMailboxResolution(
                        operation.operationId, next->relativePath, std::nullopt, message))
                    co_return javelin::jmap::operationError(*error);
                co_return std::nullopt;
            }
            if (const auto error =
                    repository.resolveMailbox(operation.operationId, next->relativePath,
                                              MailImportMailboxPhase::Reused, existing->id))
                co_return javelin::jmap::operationError(*error);
            if (const auto error = repository.propagateMailboxResolution(
                    operation.operationId, next->relativePath,
                    std::optional<std::string_view>{existing->id}))
                co_return javelin::jmap::operationError(*error);
            co_return std::nullopt;
        }

        auto create =
            co_await engine.createInParent(liveSettings(*settings), operation.accountId,
                                           next->displayName.toStdString(), parentId, jobId);
        if (const auto* error = std::get_if<OperationError>(&create))
        {
            if (retryable(*error))
            {
                static_cast<void>(repository.setStatus(operation.operationId, waitStatus(*error),
                                                       error->message));
                co_return *error;
            }
            const auto message = error->message;
            if (const auto databaseError =
                    repository.failMailbox(operation.operationId, next->relativePath, message))
                co_return javelin::jmap::operationError(*databaseError);
            if (const auto databaseError = repository.propagateMailboxResolution(
                    operation.operationId, next->relativePath, std::nullopt, message))
                co_return javelin::jmap::operationError(*databaseError);
            co_return std::nullopt;
        }
        const auto& created = std::get<javelin::jmap::MailboxCreateChange>(create);
        if (const auto error =
                repository.resolveMailbox(operation.operationId, next->relativePath,
                                          MailImportMailboxPhase::Created, created.mailboxId))
            co_return javelin::jmap::operationError(*error);
        if (const auto error = repository.propagateMailboxResolution(
                operation.operationId, next->relativePath,
                std::optional<std::string_view>{created.mailboxId}))
            co_return javelin::jmap::operationError(*error);
        static_cast<void>(jobId);
        co_return std::nullopt;
    }

    QCoro::Task<std::optional<OperationError>>
    MailImportService::processNextItem(MailImportOperationRecord operation, std::string jobId)
    {
        MailImportRepository repository{m_databaseConnection};
        const auto nextResult = repository.nextActionableItem(operation.operationId);
        if (const auto* error = std::get_if<DatabaseError>(&nextResult))
            co_return javelin::jmap::operationError(*error);
        auto item = std::get<std::optional<MailImportItemRecord>>(nextResult);
        if (!item.has_value())
            co_return finalizeImport(operation, jobId);

        if (item->phase == MailImportItemPhase::Creating && !item->existingEmailId.has_value())
        {
            if (const auto error = repository.transitionItem(
                    item->itemId,
                    {.phase = MailImportItemPhase::Unknown,
                     .lastError =
                         i18n("Javelin restarted while Email/import may have been dispatched.")}))
                co_return javelin::jmap::operationError(*error);
            item->phase = MailImportItemPhase::Unknown;
        }
        if (item->phase == MailImportItemPhase::Unknown ||
            (item->phase == MailImportItemPhase::Creating && item->existingEmailId.has_value()))
            co_return co_await reconcileUnknownItem(operation, *item, jobId);

        if (!item->resolvedMailboxId.has_value())
        {
            const auto message = i18n("No destination mailbox was resolved for this message.");
            if (const auto error = repository.transitionItem(
                    item->itemId,
                    {.phase = MailImportItemPhase::NoDestination, .lastError = message}))
                co_return javelin::jmap::operationError(*error);
            co_return std::nullopt;
        }

        if (item->phase == MailImportItemPhase::Pending ||
            item->phase == MailImportItemPhase::Uploading)
        {
            if (!sourceStillMatches(*item))
            {
                const auto message =
                    i18n("Import source changed after it was scanned: %1", item->sourcePath);
                if (const auto error = repository.transitionItem(
                        item->itemId, {.phase = MailImportItemPhase::Failed, .lastError = message}))
                    co_return javelin::jmap::operationError(*error);
                co_return std::nullopt;
            }
            if (item->phase == MailImportItemPhase::Pending)
            {
                if (const auto error = repository.transitionItem(
                        item->itemId, {.phase = MailImportItemPhase::Uploading}))
                    co_return javelin::jmap::operationError(*error);
                item->phase = MailImportItemPhase::Uploading;
            }

            const auto hashResult =
                co_await QtConcurrent::run([record = *item] { return hashImportItem(record); });
            if (const auto* error = std::get_if<OperationError>(&hashResult))
            {
                if (const auto databaseError = repository.transitionItem(
                        item->itemId,
                        {.phase = MailImportItemPhase::Failed, .lastError = error->message}))
                    co_return javelin::jmap::operationError(*databaseError);
                co_return std::nullopt;
            }
            const auto sourceHash = std::get<std::string>(hashResult);

            const auto accountResult = requireAccount(m_databaseConnection, operation.accountId);
            if (const auto* error = std::get_if<OperationError>(&accountResult))
                co_return *error;
            const auto account = std::get<javelin::jmap::cache::CachedAccount>(accountResult);
            const auto sessionResult = requireSession(m_databaseConnection, operation.accountId);
            if (const auto* error = std::get_if<OperationError>(&sessionResult))
            {
                if (retryable(*error))
                    static_cast<void>(repository.setStatus(operation.operationId,
                                                           waitStatus(*error), error->message));
                co_return *error;
            }
            const auto session = std::get<javelin::jmap::api::Session>(sessionResult);
            const auto settings = m_connectionProvider.connectionSettingsFor(operation.accountId);
            if (!settings.has_value())
                co_return OperationError{.code = OperationErrorCode::NotFound,
                                         .message =
                                             i18n("The mail account is no longer available.")};
            const auto uploadContext =
                javelin::jmap::api::blobUploadContext(session, account.remoteAccountId);
            if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&uploadContext))
                co_return javelin::jmap::operationError(*error);

            javelin::jmap::api::BlobUploadResult upload;
            if (item->sourceKind == MailImportFileKind::Eml)
            {
                upload = co_await javelin::jmap::api::uploadBlobFromFile(
                    m_resourceTransport,
                    std::get<javelin::jmap::api::BlobUploadContext>(uploadContext),
                    operation.accountId, account.remoteAccountId, settings->apiKey,
                    item->sourcePath, "message/rfc822");
            }
            else
            {
                if (!item->contentOffset.has_value() || !item->contentEnd.has_value())
                    co_return OperationError{.code = OperationErrorCode::PreconditionFailed,
                                             .message =
                                                 i18n("The mbox import record is incomplete.")};
                MailImportMboxRecordDevice device{item->sourcePath,
                                                  {.ordinal = item->ordinal,
                                                   .contentOffset = *item->contentOffset,
                                                   .contentEnd = *item->contentEnd,
                                                   .decodedSize = item->decodedSize,
                                                   .receivedAt = item->receivedAt}};
                if (!device.openReadOnly())
                {
                    const auto message =
                        i18n("Could not open mbox record in %1.", item->sourcePath);
                    if (const auto error = repository.transitionItem(
                            item->itemId,
                            {.phase = MailImportItemPhase::Failed, .lastError = message}))
                        co_return javelin::jmap::operationError(*error);
                    co_return std::nullopt;
                }
                upload = co_await javelin::jmap::api::uploadBlobFromDevice(
                    m_resourceTransport,
                    std::get<javelin::jmap::api::BlobUploadContext>(uploadContext),
                    operation.accountId, account.remoteAccountId, settings->apiKey, device,
                    item->decodedSize, "message/rfc822");
            }
            if (const auto* error = std::get_if<javelin::jmap::api::TransportError>(&upload))
            {
                const auto converted = javelin::jmap::operationError(*error);
                if (retryable(converted))
                {
                    static_cast<void>(repository.setStatus(
                        operation.operationId, waitStatus(converted), converted.message));
                    co_return converted;
                }
                if (const auto databaseError = repository.transitionItem(
                        item->itemId,
                        {.phase = MailImportItemPhase::Failed, .lastError = converted.message}))
                    co_return javelin::jmap::operationError(*databaseError);
                co_return std::nullopt;
            }
            if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&upload))
            {
                const auto converted = javelin::jmap::operationError(*error);
                if (const auto databaseError = repository.transitionItem(
                        item->itemId,
                        {.phase = MailImportItemPhase::Failed, .lastError = converted.message}))
                    co_return javelin::jmap::operationError(*databaseError);
                co_return std::nullopt;
            }
            const auto& uploaded = std::get<javelin::jmap::api::BlobUploadResponse>(upload);
            if (const auto error =
                    repository.transitionItem(item->itemId, {.phase = MailImportItemPhase::Uploaded,
                                                             .sourceSha256 = sourceHash,
                                                             .uploadedBlobId = uploaded.blobId}))
                co_return javelin::jmap::operationError(*error);
            item->phase = MailImportItemPhase::Uploaded;
            item->sourceSha256 = sourceHash;
            item->uploadedBlobId = uploaded.blobId;
        }

        if (item->phase != MailImportItemPhase::Uploaded || !item->uploadedBlobId.has_value())
            co_return std::nullopt;

        const auto accountResult = requireAccount(m_databaseConnection, operation.accountId);
        if (const auto* error = std::get_if<OperationError>(&accountResult))
            co_return *error;
        const auto account = std::get<javelin::jmap::cache::CachedAccount>(accountResult);
        const auto sessionResult = requireSession(m_databaseConnection, operation.accountId);
        if (const auto* error = std::get_if<OperationError>(&sessionResult))
        {
            if (retryable(*error))
                static_cast<void>(repository.setStatus(operation.operationId, waitStatus(*error),
                                                       error->message));
            co_return *error;
        }
        const auto session = std::get<javelin::jmap::api::Session>(sessionResult);
        const auto settings = m_connectionProvider.connectionSettingsFor(operation.accountId);
        if (!settings.has_value())
            co_return OperationError{.code = OperationErrorCode::NotFound,
                                     .message = i18n("The mail account is no longer available.")};
        javelin::jmap::api::MethodCaller caller{m_methodTransport};
        const auto context = requestContext(*settings, operation.accountId, session);

        const auto stateRequest = javelin::jmap::api::emailGet({
            .accountId = account.remoteAccountId,
            .ids = std::vector<std::string>{},
            .idsReference = std::nullopt,
            .properties = std::vector<std::string>{"id"},
        });
        if (!stateRequest.has_value())
            co_return OperationError{.code = OperationErrorCode::ProtocolViolation,
                                     .message = i18n("Could not encode the import state request.")};
        javelin::jmap::api::RequestBuilder stateBuilder;
        stateBuilder.useCore().useMail();
        const auto stateHandle = stateBuilder.call(*stateRequest, "mail-import-state");
        auto stateCalled = co_await caller.call(context, stateBuilder);
        if (!std::holds_alternative<javelin::jmap::api::ResponseEnvelope>(stateCalled))
        {
            const auto error = callerError(stateCalled);
            if (retryable(error))
            {
                static_cast<void>(
                    repository.setStatus(operation.operationId, waitStatus(error), error.message));
                co_return error;
            }
            co_return error;
        }
        const auto stateRead =
            javelin::jmap::api::ResponseReader{
                std::get<javelin::jmap::api::ResponseEnvelope>(stateCalled)}
                .require(stateHandle);
        if (const auto* error = std::get_if<javelin::jmap::api::ResponseReaderError>(&stateRead))
            co_return javelin::jmap::operationError(*error);
        const auto& state = std::get<javelin::jmap::api::EmailGetResponse>(stateRead);
        if (state.accountId != account.remoteAccountId || state.state.empty())
            co_return OperationError{.code = OperationErrorCode::ProtocolViolation,
                                     .message =
                                         i18n("The server returned an invalid Email state.")};

        if (const auto error = repository.transitionItem(
                item->itemId, {.phase = MailImportItemPhase::Creating, .preState = state.state}))
            co_return javelin::jmap::operationError(*error);
        item->phase = MailImportItemPhase::Creating;
        item->preState = state.state;

        const auto creationId = item->itemId;
        javelin::jmap::api::EmailImport imported{
            .blobId = *item->uploadedBlobId,
            .mailboxIds = {{*item->resolvedMailboxId, true}},
            .keywords = {},
            .receivedAt = item->receivedAt,
        };
        const auto importRequest = javelin::jmap::api::emailImport({
            .accountId = account.remoteAccountId,
            .ifInState = std::nullopt,
            .emails = {{creationId, std::move(imported)}},
        });
        if (!importRequest.has_value())
            co_return OperationError{.code = OperationErrorCode::ProtocolViolation,
                                     .message = i18n("Could not encode Email/import.")};
        javelin::jmap::api::RequestBuilder importBuilder;
        importBuilder.useCore().useMail();
        const auto importHandle = importBuilder.call(*importRequest, "mail-import-create");
        bool dispatched = false;
        auto called =
            co_await caller.call(context, importBuilder, {}, [&dispatched] { dispatched = true; });
        if (!std::holds_alternative<javelin::jmap::api::ResponseEnvelope>(called))
        {
            const auto error = callerError(called);
            if (dispatched)
            {
                if (const auto databaseError = repository.transitionItem(
                        item->itemId,
                        {.phase = MailImportItemPhase::Unknown, .lastError = error.message}))
                    co_return javelin::jmap::operationError(*databaseError);
                static_cast<void>(repository.setStatus(
                    operation.operationId, MailImportStatus::BlockedUnknown, error.message));
                co_return std::nullopt;
            }
            if (retryable(error))
            {
                if (const auto databaseError = repository.transitionItem(
                        item->itemId,
                        {.phase = MailImportItemPhase::Uploaded, .lastError = error.message}))
                    co_return javelin::jmap::operationError(*databaseError);
                static_cast<void>(
                    repository.setStatus(operation.operationId, waitStatus(error), error.message));
                co_return error;
            }
            if (const auto databaseError =
                    repository.transitionItem(item->itemId, {.phase = MailImportItemPhase::Failed,
                                                             .lastError = error.message}))
                co_return javelin::jmap::operationError(*databaseError);
            co_return std::nullopt;
        }

        const auto read =
            javelin::jmap::api::ResponseReader{
                std::get<javelin::jmap::api::ResponseEnvelope>(called)}
                .require(importHandle);
        if (const auto* responseError = std::get_if<javelin::jmap::api::ResponseReaderError>(&read))
        {
            const auto error = javelin::jmap::operationError(*responseError);
            if (const auto databaseError =
                    repository.transitionItem(item->itemId, {.phase = MailImportItemPhase::Unknown,
                                                             .lastError = error.message}))
                co_return javelin::jmap::operationError(*databaseError);
            static_cast<void>(repository.setStatus(
                operation.operationId, MailImportStatus::BlockedUnknown, error.message));
            co_return std::nullopt;
        }
        const auto& response = std::get<javelin::jmap::api::EmailImportResponse>(read);
        if (response.accountId != account.remoteAccountId)
        {
            const auto message = i18n("Email/import returned the wrong account id.");
            if (const auto databaseError = repository.transitionItem(
                    item->itemId, {.phase = MailImportItemPhase::Unknown, .lastError = message}))
                co_return javelin::jmap::operationError(*databaseError);
            static_cast<void>(repository.setStatus(operation.operationId,
                                                   MailImportStatus::BlockedUnknown, message));
            co_return std::nullopt;
        }
        const auto created = response.created.find(creationId);
        if (created != response.created.end())
        {
            if (const auto error =
                    repository.transitionItem(item->itemId, {.phase = MailImportItemPhase::Created,
                                                             .createdEmailId = created->second.id}))
                co_return javelin::jmap::operationError(*error);
            co_return std::nullopt;
        }

        const auto rejected = response.notCreated.find(creationId);
        if (rejected == response.notCreated.end())
        {
            const auto message = i18n("Email/import did not account for the imported message.");
            if (const auto error = repository.transitionItem(
                    item->itemId, {.phase = MailImportItemPhase::Unknown, .lastError = message}))
                co_return javelin::jmap::operationError(*error);
            static_cast<void>(repository.setStatus(operation.operationId,
                                                   MailImportStatus::BlockedUnknown, message));
            co_return std::nullopt;
        }
        if (rejected->second.type == "overQuota")
        {
            const auto message = i18n("The destination account is over quota.");
            if (const auto error = repository.transitionItem(
                    item->itemId, {.phase = MailImportItemPhase::Uploaded, .lastError = message}))
                co_return javelin::jmap::operationError(*error);
            static_cast<void>(repository.setStatus(operation.operationId,
                                                   MailImportStatus::WaitingForSpace, message));
            co_return std::nullopt;
        }
        if (rejected->second.type != "alreadyExists" || !rejected->second.existingId.has_value())
        {
            const auto message = QString::fromStdString(
                rejected->second.description.value_or("The server rejected the imported message."));
            if (const auto error = repository.transitionItem(
                    item->itemId, {.phase = MailImportItemPhase::Failed, .lastError = message}))
                co_return javelin::jmap::operationError(*error);
            co_return std::nullopt;
        }

        const auto existingId = *rejected->second.existingId;
        javelin::jmap::EmailMutationEngine mutationEngine{m_databaseConnection, m_methodTransport};
        auto authoritative = co_await mutationEngine.getAuthoritative(
            liveSettings(*settings), operation.accountId, {existingId});
        if (const auto* error = std::get_if<OperationError>(&authoritative))
        {
            if (retryable(*error))
            {
                static_cast<void>(repository.setStatus(operation.operationId, waitStatus(*error),
                                                       error->message));
                co_return *error;
            }
            if (const auto databaseError =
                    repository.transitionItem(item->itemId, {.phase = MailImportItemPhase::Failed,
                                                             .lastError = error->message}))
                co_return javelin::jmap::operationError(*databaseError);
            co_return std::nullopt;
        }
        const auto& existing = std::get<javelin::jmap::AuthoritativeEmails>(authoritative);
        if (existing.emails.size() != 1 || existing.emails.front().id != existingId)
        {
            const auto message =
                i18n("The server reported an existing message that could not be verified.");
            if (const auto error = repository.transitionItem(
                    item->itemId, {.phase = MailImportItemPhase::Failed, .lastError = message}))
                co_return javelin::jmap::operationError(*error);
            co_return std::nullopt;
        }
        const auto& email = existing.emails.front();
        if (std::ranges::contains(email.mailboxIds, *item->resolvedMailboxId))
        {
            if (const auto error =
                    repository.transitionItem(item->itemId, {.phase = MailImportItemPhase::Reused,
                                                             .existingEmailId = existingId}))
                co_return javelin::jmap::operationError(*error);
            co_return std::nullopt;
        }

        if (const auto error =
                repository.transitionItem(item->itemId, {.phase = MailImportItemPhase::Creating,
                                                         .preState = existing.state,
                                                         .existingEmailId = existingId}))
            co_return javelin::jmap::operationError(*error);
        const auto queued =
            mutationEngine.queue(operation.accountId, {.emailId = existingId,
                                                       .addMailboxIds = {*item->resolvedMailboxId},
                                                       .removeMailboxIds = {},
                                                       .addKeywords = {},
                                                       .removeKeywords = {},
                                                       .operationGroupId = jobId,
                                                       .ifInState = existing.state,
                                                       .authoritativeMailboxIds = email.mailboxIds,
                                                       .authoritativeKeywords = email.keywords,
                                                       .destroy = false});
        if (const auto* error = std::get_if<OperationError>(&queued))
        {
            if (const auto databaseError =
                    repository.transitionItem(item->itemId, {.phase = MailImportItemPhase::Failed,
                                                             .lastError = error->message}))
                co_return javelin::jmap::operationError(*databaseError);
            co_return std::nullopt;
        }
        const auto submitted = co_await mutationEngine.submitPending(
            liveSettings(*settings), operation.accountId, jobId, 1, existing.state);
        if (const auto* error = std::get_if<OperationError>(&submitted))
        {
            javelin::jmap::sync::EmailMutationJournal journal{m_databaseConnection};
            const auto records = journal.listForOperationGroup(operation.accountId, jobId);
            const auto* mutationRecords =
                std::get_if<std::vector<javelin::jmap::sync::EmailMutationRecord>>(&records);
            const bool unknown =
                mutationRecords != nullptr &&
                std::ranges::any_of(
                    *mutationRecords, [](const auto& record)
                    { return record.status == javelin::jmap::sync::MutationStatus::Unknown; });
            if (unknown)
            {
                if (const auto databaseError = repository.transitionItem(
                        item->itemId, {.phase = MailImportItemPhase::Unknown,
                                       .existingEmailId = existingId,
                                       .lastError = error->message}))
                    co_return javelin::jmap::operationError(*databaseError);
                static_cast<void>(repository.setStatus(
                    operation.operationId, MailImportStatus::BlockedUnknown, error->message));
                co_return std::nullopt;
            }
            if (retryable(*error))
            {
                static_cast<void>(repository.setStatus(operation.operationId, waitStatus(*error),
                                                       error->message));
                co_return *error;
            }
            if (const auto databaseError =
                    repository.transitionItem(item->itemId, {.phase = MailImportItemPhase::Failed,
                                                             .lastError = error->message}))
                co_return javelin::jmap::operationError(*databaseError);
            co_return std::nullopt;
        }
        const auto& submittedResult = std::get<javelin::jmap::SubmittedEmailMutations>(submitted);
        const auto accepted =
            std::ranges::find(submittedResult.items, existingId,
                              &javelin::jmap::SubmittedEmailMutations::Item::emailId);
        if (accepted == submittedResult.items.end() || !accepted->accepted)
        {
            const auto message =
                accepted != submittedResult.items.end() && accepted->error.has_value()
                    ? QString::fromStdString(*accepted->error)
                    : i18n("The server rejected adding the existing message to the mailbox.");
            if (const auto databaseError = repository.transitionItem(
                    item->itemId, {.phase = MailImportItemPhase::Failed, .lastError = message}))
                co_return javelin::jmap::operationError(*databaseError);
            co_return std::nullopt;
        }
        if (const auto error =
                repository.transitionItem(item->itemId, {.phase = MailImportItemPhase::Reused,
                                                         .existingEmailId = existingId}))
            co_return javelin::jmap::operationError(*error);
        co_return std::nullopt;
    }

    QCoro::Task<std::optional<OperationError>>
    MailImportService::reconcileUnknownItem(MailImportOperationRecord operation,
                                            MailImportItemRecord item, std::string jobId)
    {
        MailImportRepository repository{m_databaseConnection};
        const auto settings = m_connectionProvider.connectionSettingsFor(operation.accountId);
        if (!settings.has_value())
            co_return OperationError{.code = OperationErrorCode::NotFound,
                                     .message = i18n("The mail account is no longer available.")};

        if (item.existingEmailId.has_value())
        {
            javelin::jmap::sync::EmailMutationJournal journal{m_databaseConnection};
            const auto records = journal.listForOperationGroup(operation.accountId, jobId);
            if (const auto* error = std::get_if<DatabaseError>(&records))
                co_return javelin::jmap::operationError(*error);
            const auto& mutations =
                std::get<std::vector<javelin::jmap::sync::EmailMutationRecord>>(records);
            if (std::ranges::any_of(
                    mutations,
                    [](const auto& record)
                    {
                        return record.status == javelin::jmap::sync::MutationStatus::Pending ||
                               record.status == javelin::jmap::sync::MutationStatus::InFlight ||
                               record.status == javelin::jmap::sync::MutationStatus::Unknown;
                    }))
            {
                const auto message = i18n("The mailbox placement of an existing imported message "
                                          "is still being reconciled.");
                static_cast<void>(repository.setStatus(operation.operationId,
                                                       MailImportStatus::BlockedUnknown, message));
                co_return std::nullopt;
            }
            javelin::jmap::EmailMutationEngine engine{m_databaseConnection, m_methodTransport};
            const auto authoritative = co_await engine.getAuthoritative(
                liveSettings(*settings), operation.accountId, {*item.existingEmailId});
            if (const auto* error = std::get_if<OperationError>(&authoritative))
            {
                if (retryable(*error))
                {
                    static_cast<void>(repository.setStatus(operation.operationId,
                                                           waitStatus(*error), error->message));
                    co_return *error;
                }
                co_return *error;
            }
            const auto& response = std::get<javelin::jmap::AuthoritativeEmails>(authoritative);
            if (response.emails.size() == 1 && item.resolvedMailboxId.has_value() &&
                std::ranges::contains(response.emails.front().mailboxIds, *item.resolvedMailboxId))
            {
                if (const auto error = repository.transitionItem(
                        item.itemId, {.phase = MailImportItemPhase::Reused,
                                      .existingEmailId = item.existingEmailId}))
                    co_return javelin::jmap::operationError(*error);
                static_cast<void>(
                    repository.setStatus(operation.operationId, MailImportStatus::Running));
                co_return std::nullopt;
            }
            const auto message =
                i18n("The existing message placement could not be reconciled safely.");
            static_cast<void>(repository.setStatus(operation.operationId,
                                                   MailImportStatus::BlockedUnknown, message));
            co_return std::nullopt;
        }

        if (!item.preState.has_value() || !item.uploadedBlobId.has_value() ||
            !item.resolvedMailboxId.has_value())
            co_return OperationError{
                .code = OperationErrorCode::PreconditionFailed,
                .message = i18n("The uncertain import is missing reconciliation state.")};

        const auto accountResult = requireAccount(m_databaseConnection, operation.accountId);
        if (const auto* error = std::get_if<OperationError>(&accountResult))
            co_return *error;
        const auto account = std::get<javelin::jmap::cache::CachedAccount>(accountResult);
        const auto sessionResult = requireSession(m_databaseConnection, operation.accountId);
        if (const auto* error = std::get_if<OperationError>(&sessionResult))
        {
            if (retryable(*error))
                static_cast<void>(repository.setStatus(operation.operationId, waitStatus(*error),
                                                       error->message));
            co_return *error;
        }
        const auto session = std::get<javelin::jmap::api::Session>(sessionResult);
        const auto context = requestContext(*settings, operation.accountId, session);
        javelin::jmap::api::MethodCaller caller{m_methodTransport};

        std::vector<std::string> createdIds;
        std::string sinceState = *item.preState;
        bool complete = false;
        for (std::size_t page = 0; page < maximumChangePages; ++page)
        {
            const auto request = javelin::jmap::api::emailChanges({
                .accountId = account.remoteAccountId,
                .sinceState = sinceState,
                .maxChanges = changesPageSize,
            });
            if (!request.has_value())
                co_return OperationError{
                    .code = OperationErrorCode::ProtocolViolation,
                    .message = i18n("Could not encode Email/changes for import reconciliation.")};
            javelin::jmap::api::RequestBuilder builder;
            builder.useCore().useMail();
            const auto handle = builder.call(*request, "mail-import-reconcile-changes");
            auto called = co_await caller.call(context, builder);
            if (!std::holds_alternative<javelin::jmap::api::ResponseEnvelope>(called))
            {
                const auto error = callerError(called);
                if (retryable(error))
                {
                    static_cast<void>(repository.setStatus(operation.operationId, waitStatus(error),
                                                           error.message));
                    co_return error;
                }
                static_cast<void>(repository.setStatus(
                    operation.operationId, MailImportStatus::BlockedUnknown, error.message));
                co_return std::nullopt;
            }
            const auto read =
                javelin::jmap::api::ResponseReader{
                    std::get<javelin::jmap::api::ResponseEnvelope>(called)}
                    .require(handle);
            if (const auto* error = std::get_if<javelin::jmap::api::ResponseReaderError>(&read))
            {
                const auto converted = javelin::jmap::operationError(*error);
                static_cast<void>(repository.setStatus(
                    operation.operationId, MailImportStatus::BlockedUnknown, converted.message));
                co_return std::nullopt;
            }
            const auto& changes = std::get<javelin::jmap::api::EmailChangesResponse>(read);
            if (changes.accountId != account.remoteAccountId || changes.oldState != sinceState ||
                changes.newState.empty() ||
                (changes.hasMoreChanges && changes.newState == sinceState))
            {
                const auto message = i18n(
                    "The server returned inconsistent Email changes during import reconciliation.");
                static_cast<void>(repository.setStatus(operation.operationId,
                                                       MailImportStatus::BlockedUnknown, message));
                co_return std::nullopt;
            }
            createdIds.insert(createdIds.end(), changes.created.begin(), changes.created.end());
            if (!changes.hasMoreChanges)
            {
                complete = true;
                break;
            }
            sinceState = changes.newState;
        }
        if (!complete)
        {
            const auto message =
                i18n("Too many Email changes occurred to reconcile the uncertain import safely.");
            static_cast<void>(repository.setStatus(operation.operationId,
                                                   MailImportStatus::BlockedUnknown, message));
            co_return std::nullopt;
        }
        std::ranges::sort(createdIds);
        createdIds.erase(std::unique(createdIds.begin(), createdIds.end()), createdIds.end());
        if (createdIds.empty())
        {
            if (const auto error = repository.transitionItem(
                    item.itemId, {.phase = MailImportItemPhase::Uploaded}))
                co_return javelin::jmap::operationError(*error);
            static_cast<void>(
                repository.setStatus(operation.operationId, MailImportStatus::Running));
            co_return std::nullopt;
        }
        const auto limits = context.requestLimits;
        if (!limits.has_value() || createdIds.size() > limits->maxObjectsInGet)
        {
            const auto message = i18n("The server may contain the imported message, but Javelin "
                                      "cannot identify it safely.");
            static_cast<void>(repository.setStatus(operation.operationId,
                                                   MailImportStatus::BlockedUnknown, message));
            co_return std::nullopt;
        }

        const auto getRequest = javelin::jmap::api::emailGet({
            .accountId = account.remoteAccountId,
            .ids = createdIds,
            .idsReference = std::nullopt,
            .properties =
                std::vector<std::string>{"id", "blobId", "mailboxIds", "size", "receivedAt"},
        });
        if (!getRequest.has_value())
            co_return OperationError{.code = OperationErrorCode::ProtocolViolation,
                                     .message = i18n("Could not encode import candidate lookup.")};
        javelin::jmap::api::RequestBuilder builder;
        builder.useCore().useMail();
        const auto handle = builder.call(*getRequest, "mail-import-reconcile-candidates");
        auto called = co_await caller.call(context, builder);
        if (!std::holds_alternative<javelin::jmap::api::ResponseEnvelope>(called))
        {
            const auto error = callerError(called);
            if (retryable(error))
            {
                static_cast<void>(
                    repository.setStatus(operation.operationId, waitStatus(error), error.message));
                co_return error;
            }
            static_cast<void>(repository.setStatus(
                operation.operationId, MailImportStatus::BlockedUnknown, error.message));
            co_return std::nullopt;
        }
        const auto read =
            javelin::jmap::api::ResponseReader{
                std::get<javelin::jmap::api::ResponseEnvelope>(called)}
                .require(handle);
        if (const auto* error = std::get_if<javelin::jmap::api::ResponseReaderError>(&read))
        {
            const auto converted = javelin::jmap::operationError(*error);
            static_cast<void>(repository.setStatus(
                operation.operationId, MailImportStatus::BlockedUnknown, converted.message));
            co_return std::nullopt;
        }
        const auto& response = std::get<javelin::jmap::api::EmailGetResponse>(read);
        const javelin::jmap::domain::Email* match = nullptr;
        bool multiple = false;
        for (const auto& candidate : response.list)
        {
            if (candidate.blobId != *item.uploadedBlobId ||
                !std::ranges::contains(candidate.mailboxIds, *item.resolvedMailboxId))
                continue;
            if (match != nullptr)
            {
                multiple = true;
                break;
            }
            match = &candidate;
        }
        if (match != nullptr && !multiple)
        {
            if (const auto error =
                    repository.transitionItem(item.itemId, {.phase = MailImportItemPhase::Created,
                                                            .createdEmailId = match->id}))
                co_return javelin::jmap::operationError(*error);
            static_cast<void>(
                repository.setStatus(operation.operationId, MailImportStatus::Running));
            co_return std::nullopt;
        }
        const auto message =
            i18n("The server may contain the imported message, but its creation cannot be "
                 "correlated uniquely enough to continue automatically.");
        static_cast<void>(
            repository.setStatus(operation.operationId, MailImportStatus::BlockedUnknown, message));
        co_return std::nullopt;
    }

    void
    MailImportService::requestOperationSynchronization(const MailImportOperationRecord& operation)
    {
        if (!m_requestMailboxResync)
            return;

        MailImportRepository repository{m_databaseConnection};
        const auto mailboxIds = repository.resolvedMailboxIds(operation.operationId);
        if (const auto* error = std::get_if<DatabaseError>(&mailboxIds))
        {
            qWarning().noquote() << "Could not enumerate mail import destination mailboxes"
                                 << error->message;
            return;
        }
        for (const auto& mailboxId : std::get<std::vector<std::string>>(mailboxIds))
            m_requestMailboxResync(operation.accountId, mailboxId);
    }

    std::optional<OperationError>
    MailImportService::finalizeImport(const MailImportOperationRecord& operation,
                                      const std::string_view jobId)
    {
        MailImportRepository repository{m_databaseConnection};
        const auto progressResult = repository.progress(operation.operationId);
        if (const auto* error = std::get_if<DatabaseError>(&progressResult))
            return javelin::jmap::operationError(*error);
        const auto& progress = std::get<MailImportProgressSnapshot>(progressResult);
        if (progress.unknownItems > 0)
        {
            const auto message =
                i18n("Some imported messages still have an uncertain server outcome.");
            if (const auto error = repository.setStatus(operation.operationId,
                                                        MailImportStatus::BlockedUnknown, message))
                return javelin::jmap::operationError(*error);
            return std::nullopt;
        }
        const auto terminal =
            progress.failedItems > 0 ? MailImportStatus::Partial : MailImportStatus::Complete;
        if (const auto error = repository.setStatus(operation.operationId, terminal))
            return javelin::jmap::operationError(*error);
        static_cast<void>(jobId);
        return std::nullopt;
    }
} // namespace javelin::app
