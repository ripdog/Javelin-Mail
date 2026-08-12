#include "app/DaemonRemoteActionDispatcher.h"

#include "app/DaemonServices.h"
#include "app/LogStore.h"
#include "app/MailApplicationService.h"
#include "app/PerformanceMetrics.h"
#include "app/UndoApplicationPorts.h"

#include "jmap/auth/AccountOnboardingService.h"

#include <QCryptographicHash>

#include <algorithm>
#include <utility>

namespace javelin::app
{
    namespace
    {
        constexpr std::size_t maximumReplayEntries = 4096;
        constexpr std::size_t maximumReplayResultBytes = 32 * 1024 * 1024;
        constexpr auto replayResultLifetime = std::chrono::minutes{10};
        constexpr auto repeatableReplayLifetime = std::chrono::hours{1};

        [[nodiscard]] QString boundedLogText(const QString& value, const qsizetype maximumBytes)
        {
            const QByteArray utf8 = value.toUtf8();
            if (utf8.size() <= maximumBytes)
                return value;
            return QString::fromUtf8(utf8.first(maximumBytes - 8)) + QStringLiteral("…");
        }

        [[nodiscard]] javelin::protocol::DiagnosticLogEntry
        diagnosticLogEntry(const LogEntry& entry)
        {
            return {
                .timestampMilliseconds =
                    static_cast<std::uint64_t>(entry.timestamp.toMSecsSinceEpoch()),
                .level = static_cast<std::uint8_t>(entry.level),
                .subsystem = boundedLogText(entry.subsystem, 256),
                .message = boundedLogText(entry.message, 4000),
            };
        }

        [[nodiscard]] QByteArray
        commandDigest(const javelin::protocol::RemoteActionCommand& command)
        {
            return QCryptographicHash::hash(command.payload, QCryptographicHash::Sha256);
        }

        [[nodiscard]] std::size_t
        replayResultBytes(const std::optional<javelin::protocol::CommandReply>& reply)
        {
            if (!reply.has_value())
                return 0;
            const auto* accepted = std::get_if<javelin::protocol::CommandAccepted>(&*reply);
            if (accepted == nullptr || !accepted->immediateResult.has_value())
                return 0;
            return static_cast<std::size_t>(accepted->immediateResult->size());
        }

        [[nodiscard]] std::vector<javelin::protocol::ChangedDomain>
        admissionDomains(const javelin::protocol::ActionId action)
        {
            const auto metadata = javelin::protocol::actions::findActionMetadata(action);
            if (!metadata.has_value())
                return {};
            return javelin::protocol::actions::expandChangedDomains(metadata->changedDomains);
        }
    } // namespace

    DaemonRemoteActionDispatcher::DaemonRemoteActionDispatcher(
        DaemonServices& services, javelin::protocol::BoundaryEventSink& eventSink,
        std::function<javelin::protocol::InvalidationEpoch()> currentEpoch,
        std::function<std::optional<javelin::protocol::BoundaryError>()> reloadSettings,
        AuthenticationResultFilter authenticationResultFilter,
        ConnectionSettingsHydrator connectionSettingsHydrator,
        RevocationRequestHydrator revocationRequestHydrator, QObject* parent)
        : QObject(parent), m_services(services), m_eventSink(eventSink),
          m_currentEpoch(std::move(currentEpoch)), m_reloadSettings(std::move(reloadSettings)),
          m_authenticationResultFilter(std::move(authenticationResultFilter)),
          m_connectionSettingsHydrator(std::move(connectionSettingsHydrator)),
          m_revocationRequestHydrator(std::move(revocationRequestHydrator))
    {
        connect(&LogStore::instance(), &LogStore::entryAdded, this,
                [this](const LogEntry& entry)
                {
                    if (!m_daemonLogSubscribed)
                        return;
                    m_eventSink.onBoundaryEvent(javelin::protocol::DaemonLogEntries{
                        .entries = {diagnosticLogEntry(entry)},
                    });
                });
    }

    DaemonRemoteActionDispatcher::~DaemonRemoteActionDispatcher() = default;

    void DaemonRemoteActionDispatcher::releaseGuiResources()
    {
        m_mailboxObservations.clear();
        m_daemonLogSubscribed = false;
    }

    javelin::protocol::CommandReply
    DaemonRemoteActionDispatcher::dispatch(javelin::protocol::CommandRequest request)
    {
        const auto startedAt = std::chrono::steady_clock::now();
        const auto id = request.id;
        const auto* remoteCommand =
            std::get_if<javelin::protocol::RemoteActionCommand>(&request.command);
        if (remoteCommand == nullptr)
            return reject(id, QStringLiteral("The request is not a remote action."),
                          javelin::protocol::BoundaryErrorCode::UnsupportedOperation);

        const auto actionMetadata =
            javelin::protocol::actions::findActionMetadata(remoteCommand->action);
        if (!actionMetadata.has_value())
            return reject(id, QStringLiteral("The remote action is unsupported."),
                          javelin::protocol::BoundaryErrorCode::UnsupportedOperation);
        if (static_cast<std::size_t>(remoteCommand->payload.size()) >
            actionMetadata->maximumPayloadBytes)
            return reject(id, QStringLiteral("The remote action payload is too large."),
                          javelin::protocol::BoundaryErrorCode::ValueTooLarge);

        if (remoteCommand->action == javelin::protocol::actions::AcknowledgeRemoteActionResult::id)
        {
            using Action = javelin::protocol::actions::AcknowledgeRemoteActionResult;
            auto decoded =
                remote::decodeVersionedTuple<Action::requestSchemaVersion,
                                             typename Action::Request>(remoteCommand->payload);
            if (const auto* error = std::get_if<remote::CodecError>(&decoded))
                return reject(id, error->message);
            const auto targetKey =
                std::get<0>(std::get<typename Action::Request>(std::move(decoded)));
            if (const auto replay = m_replays.find(targetKey); replay != m_replays.end())
            {
                if (replay->second.pending)
                    return reject(id, QStringLiteral("The remote action is still pending."),
                                  javelin::protocol::BoundaryErrorCode::Busy);
                eraseReplay(targetKey);
            }
            return acceptEmpty<Action>(id);
        }

        const auto key = replayKey(id);
        const auto digest = commandDigest(*remoteCommand);
        if (const auto replay = m_replays.find(key); replay != m_replays.end())
        {
            if (replay->second.action != remoteCommand->action ||
                replay->second.payloadDigest != digest)
                return reject(id, QStringLiteral("The command identifier was reused."));

            if (replay->second.reply.has_value())
            {
                PerformanceMetrics::recordEvent(
                    QStringLiteral("daemon"), QStringLiteral("remote_action_admission"),
                    QStringLiteral("replay"),
                    QStringLiteral("kind=%1").arg(
                        PerformanceMetrics::remoteActionName(remoteCommand->action)));
                return *replay->second.reply;
            }

            if (!replay->second.repeatable)
                return reject(id,
                              QStringLiteral("The command replay result is no longer available."),
                              javelin::protocol::BoundaryErrorCode::InvalidRequest);

            auto reply = dispatchRemote(id, *remoteCommand);
            const bool pending =
                std::holds_alternative<javelin::protocol::CommandAccepted>(reply) &&
                std::get<javelin::protocol::CommandAccepted>(reply).operation.has_value() &&
                !std::get<javelin::protocol::CommandAccepted>(reply).immediateResult.has_value();
            replay->second.startedAt = startedAt;
            replay->second.pending = pending;
            replay->second.completedAt =
                pending ? std::nullopt : std::optional{std::chrono::steady_clock::now()};
            replay->second.terminalResultExpired = false;
            setReplayReply(replay->second, reply);
            PerformanceMetrics::recordEvent(
                QStringLiteral("daemon"), QStringLiteral("remote_action_admission"),
                QStringLiteral("replay_reexecuted"),
                QStringLiteral("kind=%1").arg(
                    PerformanceMetrics::remoteActionName(remoteCommand->action)));
            trimReplays();
            return reply;
        }

        trimReplays();
        if (m_replays.size() >= maximumReplayEntries)
        {
            return reject(id,
                          QStringLiteral("Too many remote actions are awaiting replay cleanup."),
                          javelin::protocol::BoundaryErrorCode::Busy);
        }

        auto reply = dispatchRemote(id, *remoteCommand);
        const bool pending =
            std::holds_alternative<javelin::protocol::CommandAccepted>(reply) &&
            std::get<javelin::protocol::CommandAccepted>(reply).operation.has_value() &&
            !std::get<javelin::protocol::CommandAccepted>(reply).immediateResult.has_value();
        PerformanceMetrics::recordDuration(
            QStringLiteral("daemon"), QStringLiteral("remote_action_admission"),
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                  startedAt),
            std::holds_alternative<javelin::protocol::CommandAccepted>(reply)
                ? QStringLiteral("accepted")
                : QStringLiteral("rejected"),
            QStringLiteral("kind=%1 pending=%2 payload_bytes=%3")
                .arg(PerformanceMetrics::remoteActionName(remoteCommand->action))
                .arg(pending)
                .arg(remoteCommand->payload.size()));
        auto [inserted, wasInserted] = m_replays.emplace(
            key,
            ReplayEntry{.id = id,
                        .action = remoteCommand->action,
                        .payloadDigest = digest,
                        .reply = std::nullopt,
                        .pending = pending,
                        .repeatable = actionMetadata->replay ==
                                      javelin::protocol::actions::ReplayPolicy::Reexecute,
                        .terminalResultExpired = false,
                        .retainedResultBytes = 0,
                        .startedAt = startedAt,
                        .completedAt = pending ? std::nullopt
                                               : std::optional{std::chrono::steady_clock::now()}});
        Q_ASSERT(wasInserted);
        setReplayReply(inserted->second, reply);
        m_replayOrder.push_back(key);
        trimReplays();
        return reply;
    }

    javelin::protocol::CommandReply DaemonRemoteActionDispatcher::dispatchRemote(
        const javelin::protocol::CommandId& id,
        const javelin::protocol::RemoteActionCommand& command)
    {
        const auto metadata = javelin::protocol::actions::findActionMetadata(command.action);
        if (!metadata.has_value())
            return reject(id, QStringLiteral("The remote action is unsupported."),
                          javelin::protocol::BoundaryErrorCode::UnsupportedOperation);

        using Domain = javelin::protocol::actions::ActionDomain;
        switch (metadata->domain)
        {
        case Domain::Account:
            return dispatchAccountAction(id, command);
        case Domain::Calendar:
            return dispatchCalendarAction(id, command);
        case Domain::Compose:
            return dispatchComposeAction(id, command);
        case Domain::Contact:
            return dispatchContactAction(id, command);
        case Domain::Mail:
            return dispatchMailAction(id, command);
        case Domain::Sieve:
            return dispatchSieveAction(id, command);
        case Domain::Identity:
            return dispatchIdentityAction(id, command);
        case Domain::History:
            return dispatchHistoryAction(id, command);
        case Domain::Work:
            return dispatchWorkAction(id, command);
        case Domain::Developer:
            return dispatchDeveloperAction(id, command);
        }
        return reject(id, QStringLiteral("The remote action domain is unsupported."),
                      javelin::protocol::BoundaryErrorCode::UnsupportedOperation);
    }

    javelin::protocol::CommandReply
    DaemonRemoteActionDispatcher::reject(const javelin::protocol::CommandId& id, QString detail,
                                         const javelin::protocol::BoundaryErrorCode code) const
    {
        return javelin::protocol::CommandRejected{
            .id = id,
            .error = {.code = code,
                      .field = QStringLiteral("command.remote"),
                      .detail = std::move(detail)},
        };
    }

    javelin::protocol::CommandReply
    DaemonRemoteActionDispatcher::acceptImmediate(const javelin::protocol::CommandId& id,
                                                  const javelin::protocol::ActionId action,
                                                  QByteArray result) const
    {
        return javelin::protocol::CommandAccepted{
            .id = id,
            .operation = std::nullopt,
            .epoch = m_currentEpoch(),
            .changedDomains = admissionDomains(action),
            .affectedKeys = {},
            .immediateResult = std::move(result),
        };
    }

    javelin::protocol::CommandReply
    DaemonRemoteActionDispatcher::acceptAsync(const javelin::protocol::CommandId& id,
                                              const javelin::protocol::ActionId action,
                                              const javelin::protocol::OperationId& operation) const
    {
        return javelin::protocol::CommandAccepted{
            .id = id,
            .operation = operation,
            .epoch = m_currentEpoch(),
            .changedDomains = admissionDomains(action),
            .affectedKeys = {},
            .immediateResult = std::nullopt,
        };
    }

    QCoro::Task<AccountAuthenticationResult>
    DaemonRemoteActionDispatcher::finishOAuthAndFilter(OAuthFinishRequest request)
    {
        auto result = co_await m_services.onboardingService().finishOAuth(std::move(request));
        co_return m_authenticationResultFilter(std::move(result));
    }

    QCoro::Task<AccountAuthenticationResult>
    DaemonRemoteActionDispatcher::authenticateManuallyAndFilter(ManualAuthenticationRequest request)
    {
        auto result =
            co_await m_services.onboardingService().authenticateManually(std::move(request));
        co_return m_authenticationResultFilter(std::move(result));
    }

    QCoro::Task<RemoteUndoExecutionResult>
    DaemonRemoteActionDispatcher::performUndo(const bool redo)
    {
        RemoteUndoExecutionResult result;
        auto& port = m_services.undoCommandPort();
        const auto completed = connect(&port, &UndoCommandPort::executionCompleted, this,
                                       [&result](QString entryId, undo::HistoryRefreshScope scope)
                                       {
                                           result.completedEntryId = std::move(entryId);
                                           result.refreshScope = std::move(scope);
                                       });
        const auto failed = connect(&port, &UndoCommandPort::executionFailed, this,
                                    [&result](undo::HistoryFailure failure)
                                    { result.failure = std::move(failure); });
        result.succeeded = redo ? co_await port.redo() : co_await port.undo();
        disconnect(completed);
        disconnect(failed);
        co_return result;
    }

    void DaemonRemoteActionDispatcher::complete(const javelin::protocol::OperationId& operation,
                                                QByteArray result)
    {
        const auto replay = m_replays.find(replayKey({.value = operation.value}));
        if (replay != m_replays.end())
        {
            PerformanceMetrics::recordDuration(
                QStringLiteral("daemon"), QStringLiteral("remote_action_execution"),
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - replay->second.startedAt),
                QStringLiteral("completed"),
                QStringLiteral("kind=%1 result_bytes=%2")
                    .arg(PerformanceMetrics::remoteActionName(replay->second.action))
                    .arg(result.size()));
            if (replay->second.reply.has_value())
            {
                auto updated = *replay->second.reply;
                if (auto* accepted = std::get_if<javelin::protocol::CommandAccepted>(&updated))
                {
                    accepted->operation = std::nullopt;
                    accepted->epoch = m_currentEpoch();
                    accepted->changedDomains = admissionDomains(replay->second.action);
                    accepted->immediateResult = result;
                    setReplayReply(replay->second, std::move(updated));
                }
            }
            replay->second.pending = false;
            replay->second.completedAt = std::chrono::steady_clock::now();
            trimReplays();
        }
        m_eventSink.onBoundaryEvent(javelin::protocol::OperationCompleted{
            .operation = operation,
            .result = std::move(result),
        });
    }

    void DaemonRemoteActionDispatcher::fail(const javelin::protocol::OperationId& operation,
                                            QString detail)
    {
        const javelin::protocol::BoundaryError error{
            .code = javelin::protocol::BoundaryErrorCode::ProtocolViolation,
            .field = QStringLiteral("command.remote.result"),
            .detail = std::move(detail),
        };
        const auto replay = m_replays.find(replayKey({.value = operation.value}));
        if (replay != m_replays.end())
        {
            PerformanceMetrics::recordDuration(
                QStringLiteral("daemon"), QStringLiteral("remote_action_execution"),
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - replay->second.startedAt),
                QStringLiteral("failed"),
                QStringLiteral("kind=%1 code=%2")
                    .arg(PerformanceMetrics::remoteActionName(replay->second.action))
                    .arg(static_cast<int>(error.code)));
            setReplayReply(replay->second, javelin::protocol::CommandRejected{
                                               .id = {.value = operation.value},
                                               .error = error,
                                           });
            replay->second.pending = false;
            replay->second.completedAt = std::chrono::steady_clock::now();
            trimReplays();
        }
        m_eventSink.onBoundaryEvent(javelin::protocol::OperationFailed{
            .operation = operation,
            .error = error,
        });
    }

    void DaemonRemoteActionDispatcher::setReplayReply(
        ReplayEntry& entry, std::optional<javelin::protocol::CommandReply> reply)
    {
        Q_ASSERT(m_replayResultBytes >= entry.retainedResultBytes);
        m_replayResultBytes -= entry.retainedResultBytes;
        entry.reply = std::move(reply);
        entry.retainedResultBytes = replayResultBytes(entry.reply);
        m_replayResultBytes += entry.retainedResultBytes;
    }

    void DaemonRemoteActionDispatcher::eraseReplay(const QString& key)
    {
        const auto replay = m_replays.find(key);
        if (replay == m_replays.end())
            return;
        Q_ASSERT(m_replayResultBytes >= replay->second.retainedResultBytes);
        m_replayResultBytes -= replay->second.retainedResultBytes;
        m_replays.erase(replay);
        std::erase(m_replayOrder, key);
    }

    void DaemonRemoteActionDispatcher::expireReplayResult(ReplayEntry& entry)
    {
        if (entry.pending)
            return;
        entry.terminalResultExpired = true;
        setReplayReply(
            entry,
            javelin::protocol::CommandRejected{
                .id = entry.id,
                .error = {.code = javelin::protocol::BoundaryErrorCode::InvalidRequest,
                          .field = QStringLiteral("command.remote.result"),
                          .detail = QStringLiteral("The command completed, but its replay result "
                                                   "was released before acknowledgement. The "
                                                   "command was not repeated.")},
            });
    }

    void DaemonRemoteActionDispatcher::trimReplays()
    {
        const auto now = std::chrono::steady_clock::now();
        std::vector<QString> eraseKeys;
        eraseKeys.reserve(m_replayOrder.size());
        for (const auto& key : m_replayOrder)
        {
            const auto replay = m_replays.find(key);
            if (replay == m_replays.end() || replay->second.pending ||
                !replay->second.completedAt.has_value())
                continue;

            const auto age = now - *replay->second.completedAt;
            if (replay->second.terminalResultExpired)
                continue;
            if (replay->second.repeatable && age >= repeatableReplayLifetime)
            {
                eraseKeys.push_back(key);
                continue;
            }
            if (age < replayResultLifetime)
                continue;

            if (replay->second.reply.has_value() &&
                std::holds_alternative<javelin::protocol::CommandRejected>(*replay->second.reply))
            {
                eraseKeys.push_back(key);
            }
            else if (replay->second.repeatable)
            {
                setReplayReply(replay->second, std::nullopt);
            }
            else
            {
                expireReplayResult(replay->second);
            }
        }
        for (const auto& key : eraseKeys)
            eraseReplay(key);

        while (m_replayResultBytes > maximumReplayResultBytes)
        {
            bool released = false;
            for (const auto& key : m_replayOrder)
            {
                const auto replay = m_replays.find(key);
                if (replay == m_replays.end() || replay->second.pending ||
                    replay->second.retainedResultBytes == 0)
                    continue;
                if (replay->second.repeatable)
                    setReplayReply(replay->second, std::nullopt);
                else
                    expireReplayResult(replay->second);
                released = true;
                break;
            }
            if (!released)
                break;
        }

        while (m_replays.size() >= maximumReplayEntries)
        {
            std::optional<QString> candidate;
            for (const auto& key : m_replayOrder)
            {
                const auto replay = m_replays.find(key);
                if (replay == m_replays.end() || replay->second.pending)
                    continue;
                const bool rejected = replay->second.reply.has_value() &&
                                      std::holds_alternative<javelin::protocol::CommandRejected>(
                                          *replay->second.reply);
                if (replay->second.repeatable ||
                    (rejected && !replay->second.terminalResultExpired))
                {
                    candidate = key;
                    break;
                }
            }
            if (!candidate.has_value())
                break;
            eraseReplay(*candidate);
        }
    }

    QString DaemonRemoteActionDispatcher::replayKey(const javelin::protocol::CommandId& id)
    {
        return id.value.toString(QUuid::WithoutBraces);
    }
} // namespace javelin::app
