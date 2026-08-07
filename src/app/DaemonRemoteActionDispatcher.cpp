#include "app/DaemonRemoteActionDispatcher.h"

#include "app/AccountApplicationPorts.h"
#include "app/AccountRefreshApplicationPorts.h"
#include "app/CalendarApplicationPorts.h"
#include "app/ComposeApplicationPorts.h"
#include "app/ContactApplicationPorts.h"
#include "app/DaemonServices.h"
#include "app/DeveloperDiagnostics.h"
#include "app/DeveloperMaintenance.h"
#include "app/IdentityApplicationPorts.h"
#include "app/MailApplicationPorts.h"
#include "app/MailApplicationService.h"
#include "app/MessageContentApplicationPorts.h"
#include "app/OnboardingTypes.h"
#include "app/PerformanceMetrics.h"
#include "app/RemoteCodec.h"
#include "app/SieveApplicationPorts.h"
#include "app/UndoApplicationPorts.h"
#include "app/WorkScheduler.h"

#include "jmap/auth/AccountOnboardingService.h"

#include <QCoroTask>

#include <QCryptographicHash>
#include <QTimer>

#include <algorithm>
#include <type_traits>
#include <utility>

namespace javelin::app
{
    namespace
    {
        template <typename... Values>
        [[nodiscard]] std::variant<std::tuple<Values...>, QString>
        decodeArguments(const QByteArray& payload)
        {
            auto decoded = remote::decode<Values...>(payload);
            if (const auto* error = std::get_if<remote::CodecError>(&decoded))
                return error->message;
            return std::get<std::tuple<Values...>>(std::move(decoded));
        }

        template <typename... Values, typename Reject, typename Apply>
        [[nodiscard]] javelin::protocol::CommandReply decodeAndApply(const QByteArray& payload,
                                                                     Reject&& reject, Apply&& apply)
        {
            auto decoded = decodeArguments<Values...>(payload);
            if (const auto* error = std::get_if<QString>(&decoded))
                return std::forward<Reject>(reject)(*error);
            return std::apply(std::forward<Apply>(apply),
                              std::get<std::tuple<Values...>>(std::move(decoded)));
        }

        template <typename Value>
        [[nodiscard]] std::variant<QByteArray, QString> encodeResult(const Value& value)
        {
            auto encoded = remote::encode(value);
            if (const auto* error = std::get_if<remote::CodecError>(&encoded))
                return error->message;
            return std::get<QByteArray>(std::move(encoded));
        }

        [[nodiscard]] std::variant<QByteArray, QString> encodeEmptyResult()
        {
            return encodeResult(std::monostate{});
        }

        constexpr std::size_t maximumReplayEntries = 4096;
        constexpr std::size_t maximumReplayResultBytes = 32 * 1024 * 1024;
        constexpr auto replayResultLifetime = std::chrono::minutes{10};
        constexpr auto repeatableReplayLifetime = std::chrono::hours{1};

        [[nodiscard]] QByteArray
        commandDigest(const javelin::protocol::RemoteActionCommand& command)
        {
            return QCryptographicHash::hash(command.payload, QCryptographicHash::Sha256);
        }

        [[nodiscard]] bool canRepeatForReplay(const javelin::protocol::RemoteActionKind kind)
        {
            using Kind = javelin::protocol::RemoteActionKind;
            switch (kind)
            {
            case Kind::CalendarReadCached:
            case Kind::CalendarReadAccounts:
            case Kind::CalendarReadCalendars:
            case Kind::CalendarRequestRange:
            case Kind::ComposeLoadSenderIdentities:
            case Kind::ComposeLoadWorkingCopy:
            case Kind::ComposeStoreWorkingCopy:
            case Kind::ContactRequestRefresh:
            case Kind::ContactDownloadMedia:
            case Kind::SieveList:
            case Kind::SieveGet:
            case Kind::SieveValidate:
            case Kind::IdentityList:
            case Kind::AccountBootstrap:
            case Kind::MessageContent:
            case Kind::AttachmentDownload:
            case Kind::MessageSource:
            case Kind::MailboxWindow:
            case Kind::SearchWindow:
            case Kind::SearchRetire:
            case Kind::UndoSnapshot:
            case Kind::WorkList:
            case Kind::WorkSummary:
            case Kind::OnboardingDiscover:
            case Kind::DeveloperDiagnosticsSnapshot:
                return true;
            default:
                return false;
            }
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
        admissionDomains(const javelin::protocol::RemoteActionKind kind)
        {
            using Domain = javelin::protocol::ChangedDomain;
            using Kind = javelin::protocol::RemoteActionKind;
            switch (kind)
            {
            case Kind::RemoveConfiguredAccount:
                return {Domain::MailboxTree,    Domain::MailQueryWindows, Domain::MessageMetadata,
                        Domain::MessageContent, Domain::Contacts,         Domain::Calendars,
                        Domain::History,        Domain::BackgroundJobs};
            case Kind::CalendarRequestRange:
                return {Domain::Calendars};
            case Kind::CalendarCreateEvent:
            case Kind::CalendarUpdateEvent:
            case Kind::CalendarDeleteEvent:
            case Kind::CalendarSetSubscribed:
            case Kind::CalendarSetDefault:
            case Kind::CalendarCreate:
            case Kind::CalendarDelete:
            case Kind::CalendarSetVisible:
                return {Domain::Calendars, Domain::History};
            case Kind::ComposeOpen:
            case Kind::ComposeStoreWorkingCopy:
            case Kind::ComposeDiscard:
                return {Domain::MessageContent};
            case Kind::ComposeSaveDraft:
            case Kind::ComposeSend:
                return {Domain::MailQueryWindows, Domain::MessageMetadata, Domain::MessageContent,
                        Domain::History};
            case Kind::ContactRequestRefresh:
                return {Domain::Contacts};
            case Kind::ContactMutateAddressBook:
            case Kind::ContactSave:
            case Kind::ContactSetStarred:
            case Kind::ContactDelete:
            case Kind::ContactCreateGroup:
            case Kind::ContactDeleteGroup:
            case Kind::ContactSetGroupMembership:
            case Kind::ContactCopy:
            case Kind::ContactImport:
            case Kind::ContactMerge:
                return {Domain::Contacts, Domain::History};
            case Kind::MailQueueMailboxMutation:
            case Kind::MailQueueDestroy:
            case Kind::MailQueueMarkUnread:
            case Kind::MailQueueMarkRead:
            case Kind::MailQueueSetFlagged:
            case Kind::MailSubmitPending:
                return {Domain::MailQueryWindows, Domain::MessageMetadata, Domain::History};
            case Kind::SieveSave:
            case Kind::SieveDelete:
            case Kind::SieveActivate:
                return {Domain::History};
            case Kind::IdentitySave:
            case Kind::IdentityDelete:
                return {Domain::SenderIdentities};
            case Kind::AccountBootstrap:
                return {Domain::MailboxTree, Domain::MailQueryWindows, Domain::MessageMetadata,
                        Domain::Contacts, Domain::Calendars};
            case Kind::MessageContent:
            case Kind::AttachmentDownload:
            case Kind::MessageSource:
                return {Domain::MessageContent};
            case Kind::MailboxWindow:
            case Kind::SearchWindow:
            case Kind::SearchRetire:
                return {Domain::MailQueryWindows, Domain::MessageMetadata};
            case Kind::Undo:
            case Kind::Redo:
                return {Domain::MailboxTree,    Domain::MailQueryWindows, Domain::MessageMetadata,
                        Domain::MessageContent, Domain::Contacts,         Domain::Calendars,
                        Domain::History};
            case Kind::UndoAcknowledgeRemove:
            case Kind::UndoForget:
                return {Domain::History};
            case Kind::ReloadSettings:
                return {Domain::MailboxTree, Domain::BackgroundJobs};
            case Kind::WorkPause:
            case Kind::WorkResume:
            case Kind::WorkRetry:
                return {Domain::BackgroundJobs};
            case Kind::DeveloperMailboxClear:
                return {Domain::MailQueryWindows, Domain::MessageMetadata, Domain::MessageContent};
            case Kind::CalendarReadCached:
            case Kind::CalendarReadAccounts:
            case Kind::CalendarReadCalendars:
            case Kind::ComposeLoadSenderIdentities:
            case Kind::ComposeLoadWorkingCopy:
            case Kind::ContactUploadMedia:
            case Kind::ContactDownloadMedia:
            case Kind::SieveList:
            case Kind::SieveGet:
            case Kind::SieveValidate:
            case Kind::IdentityList:
            case Kind::MailboxObserve:
            case Kind::MailboxUnobserve:
            case Kind::UndoSnapshot:
            case Kind::WorkList:
            case Kind::WorkSummary:
            case Kind::OnboardingDiscover:
            case Kind::OnboardingStartOAuth:
            case Kind::OnboardingFinishOAuth:
            case Kind::OnboardingAuthenticateManually:
            case Kind::OnboardingRevokeOAuth:
            case Kind::OnboardingCancelOAuth:
            case Kind::DeveloperDiagnosticsSnapshot:
            case Kind::AcknowledgeRemoteActionResult:
                return {};
            }
            return {};
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
    }

    DaemonRemoteActionDispatcher::~DaemonRemoteActionDispatcher() = default;

    void DaemonRemoteActionDispatcher::releaseGuiResources()
    {
        m_mailboxObservations.clear();
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

        using Kind = javelin::protocol::RemoteActionKind;
        if (remoteCommand->kind == Kind::AcknowledgeRemoteActionResult)
        {
            auto decoded = decodeArguments<QString>(remoteCommand->payload);
            if (const auto* error = std::get_if<QString>(&decoded))
                return reject(id, *error);
            const auto targetKey = std::get<0>(std::get<std::tuple<QString>>(std::move(decoded)));
            if (const auto replay = m_replays.find(targetKey); replay != m_replays.end())
            {
                if (replay->second.pending)
                    return reject(id, QStringLiteral("The remote action is still pending."),
                                  javelin::protocol::BoundaryErrorCode::Busy);
                eraseReplay(targetKey);
            }
            const auto encoded = encodeEmptyResult();
            if (const auto* error = std::get_if<QString>(&encoded))
                return reject(id, *error);
            return acceptImmediate(id, remoteCommand->kind, std::get<QByteArray>(encoded));
        }

        const auto key = replayKey(id);
        const auto digest = commandDigest(*remoteCommand);
        if (const auto replay = m_replays.find(key); replay != m_replays.end())
        {
            if (replay->second.kind != remoteCommand->kind ||
                replay->second.payloadDigest != digest)
                return reject(id, QStringLiteral("The command identifier was reused."));

            if (replay->second.reply.has_value())
            {
                PerformanceMetrics::recordEvent(
                    QStringLiteral("daemon"), QStringLiteral("remote_action_admission"),
                    QStringLiteral("replay"),
                    QStringLiteral("kind=%1").arg(
                        PerformanceMetrics::remoteActionName(remoteCommand->kind)));
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
                    PerformanceMetrics::remoteActionName(remoteCommand->kind)));
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
                .arg(PerformanceMetrics::remoteActionName(remoteCommand->kind))
                .arg(pending)
                .arg(remoteCommand->payload.size()));
        auto [inserted, wasInserted] = m_replays.emplace(
            key,
            ReplayEntry{.id = id,
                        .kind = remoteCommand->kind,
                        .payloadDigest = digest,
                        .reply = std::nullopt,
                        .pending = pending,
                        .repeatable = canRepeatForReplay(remoteCommand->kind),
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
        const auto immediate = [this, &id, kind = command.kind]<typename Value>(
                                   Value&& value) -> javelin::protocol::CommandReply
        {
            const auto encoded = encodeResult(value);
            if (const auto* error = std::get_if<QString>(&encoded))
                return reject(id, *error);
            return acceptImmediate(id, kind, std::get<QByteArray>(encoded));
        };
        const auto empty = [this, &id, kind = command.kind]() -> javelin::protocol::CommandReply
        {
            const auto encoded = encodeEmptyResult();
            if (const auto* error = std::get_if<QString>(&encoded))
                return reject(id, *error);
            return acceptImmediate(id, kind, std::get<QByteArray>(encoded));
        };
        const auto launch = [this, &id, kind = command.kind]<typename Result>(
                                QCoro::Task<Result> task) -> javelin::protocol::CommandReply
        {
            const javelin::protocol::OperationId operation{.value = id.value};
            QTimer::singleShot(0, this,
                               [this, operation, task = std::move(task)]() mutable
                               {
                                   QCoro::connect(
                                       std::move(task), this,
                                       [this, operation](Result result)
                                       {
                                           const auto encoded = encodeResult(result);
                                           if (const auto* error = std::get_if<QString>(&encoded))
                                               fail(operation, *error);
                                           else
                                               complete(operation, std::get<QByteArray>(encoded));
                                       });
                               });
            return acceptAsync(id, kind, operation);
        };
        const auto invalidPayload = [this, &id](const QString& detail)
        { return reject(id, detail); };

        using Kind = javelin::protocol::RemoteActionKind;
        switch (command.kind)
        {
        case Kind::OnboardingDiscover:
            return decodeAndApply<AccountDiscoveryRequest>(
                command.payload, invalidPayload, [&](AccountDiscoveryRequest request)
                { return launch(m_services.onboardingService().discover(std::move(request))); });
        case Kind::OnboardingStartOAuth:
            return decodeAndApply<OAuthStartRequest>(
                command.payload, invalidPayload, [&](OAuthStartRequest request)
                { return launch(m_services.onboardingService().startOAuth(std::move(request))); });
        case Kind::OnboardingFinishOAuth:
            return decodeAndApply<OAuthFinishRequest>(
                command.payload, invalidPayload,
                [&](OAuthFinishRequest request)
                {
                    auto task = [this, request = std::move(request)]() mutable
                        -> QCoro::Task<AccountAuthenticationResult>
                    {
                        auto result =
                            co_await m_services.onboardingService().finishOAuth(std::move(request));
                        co_return m_authenticationResultFilter(std::move(result));
                    }();
                    return launch(std::move(task));
                });
        case Kind::OnboardingAuthenticateManually:
            return decodeAndApply<ManualAuthenticationRequest>(
                command.payload, invalidPayload,
                [&](ManualAuthenticationRequest request)
                {
                    auto task = [this, request = std::move(request)]() mutable
                        -> QCoro::Task<AccountAuthenticationResult>
                    {
                        auto result = co_await m_services.onboardingService().authenticateManually(
                            std::move(request));
                        co_return m_authenticationResultFilter(std::move(result));
                    }();
                    return launch(std::move(task));
                });
        case Kind::OnboardingRevokeOAuth:
            return decodeAndApply<OAuthRevocationRequest>(
                command.payload, invalidPayload,
                [&](OAuthRevocationRequest request)
                {
                    auto hydrated = m_revocationRequestHydrator(std::move(request));
                    if (const auto* error = std::get_if<QString>(&hydrated))
                        return reject(id, *error);
                    return launch(m_services.onboardingService().revokeOAuth(
                        std::get<OAuthRevocationRequest>(std::move(hydrated))));
                });
        case Kind::OnboardingCancelOAuth:
            return decodeAndApply<OAuthCancelRequest>(
                command.payload, invalidPayload, [&](OAuthCancelRequest request)
                { return launch(m_services.onboardingService().cancelOAuth(std::move(request))); });
        case Kind::RemoveConfiguredAccount:
            return decodeAndApply<QString, QString, QStringList>(
                command.payload, invalidPayload,
                [&](const QString& loginEmail, const QString& sessionUrl,
                    const QStringList& accountIds)
                {
                    return immediate(m_services.accountCommandPort().removeConfiguredAccount(
                        loginEmail, sessionUrl, accountIds));
                });
        case Kind::CalendarReadCached:
            return decodeAndApply<std::string, javelin::jmap::calendar::VisibleInterval,
                                  javelin::jmap::calendar::TimeZoneId>(
                command.payload, invalidPayload,
                [&](const std::string& accountId,
                    const javelin::jmap::calendar::VisibleInterval& interval,
                    const javelin::jmap::calendar::TimeZoneId& timeZone)
                {
                    return immediate(
                        m_services.calendarService().loadCached(accountId, interval, timeZone));
                });
        case Kind::CalendarReadAccounts:
            return immediate(m_services.calendarService().accounts());
        case Kind::CalendarReadCalendars:
            return decodeAndApply<std::string>(
                command.payload, invalidPayload, [&](const std::string& accountId)
                { return immediate(m_services.calendarService().calendars(accountId)); });
        case Kind::CalendarRequestRange:
            return decodeAndApply<std::string, javelin::jmap::calendar::VisibleInterval,
                                  javelin::jmap::calendar::TimeZoneId>(
                command.payload, invalidPayload,
                [&](std::string ownerAccountId, javelin::jmap::calendar::VisibleInterval interval,
                    javelin::jmap::calendar::TimeZoneId timeZone)
                {
                    return launch(m_services.calendarCommandPort().requestCalendarRange(
                        std::move(ownerAccountId), std::move(interval), std::move(timeZone)));
                });
        case Kind::CalendarCreateEvent:
            return decodeAndApply<std::string, javelin::jmap::calendar::CreateEventCommand,
                                  undo::CommandOrigin>(
                command.payload, invalidPayload,
                [&](std::string ownerAccountId,
                    javelin::jmap::calendar::CreateEventCommand eventCommand,
                    const undo::CommandOrigin origin)
                {
                    return launch(m_services.calendarCommandPort().createCalendarEvent(
                        std::move(ownerAccountId), std::move(eventCommand), origin));
                });
        case Kind::CalendarUpdateEvent:
            return decodeAndApply<std::string, javelin::jmap::calendar::UpdateEventCommand,
                                  undo::CommandOrigin>(
                command.payload, invalidPayload,
                [&](std::string ownerAccountId,
                    javelin::jmap::calendar::UpdateEventCommand eventCommand,
                    const undo::CommandOrigin origin)
                {
                    return launch(m_services.calendarCommandPort().updateCalendarEvent(
                        std::move(ownerAccountId), std::move(eventCommand), origin));
                });
        case Kind::CalendarDeleteEvent:
            return decodeAndApply<std::string, javelin::jmap::calendar::DeleteEventCommand,
                                  undo::CommandOrigin>(
                command.payload, invalidPayload,
                [&](std::string ownerAccountId,
                    javelin::jmap::calendar::DeleteEventCommand eventCommand,
                    const undo::CommandOrigin origin)
                {
                    return launch(m_services.calendarCommandPort().deleteCalendarEvent(
                        std::move(ownerAccountId), std::move(eventCommand), origin));
                });
        case Kind::CalendarSetSubscribed:
            return decodeAndApply<std::string, std::string, std::string, bool>(
                command.payload, invalidPayload,
                [&](std::string ownerAccountId, std::string accountId, std::string calendarId,
                    const bool subscribed)
                {
                    return launch(m_services.calendarCommandPort().setCalendarSubscribed(
                        std::move(ownerAccountId), std::move(accountId), std::move(calendarId),
                        subscribed));
                });
        case Kind::CalendarSetDefault:
            return decodeAndApply<std::string, std::string, std::string, undo::CommandOrigin>(
                command.payload, invalidPayload,
                [&](std::string ownerAccountId, std::string accountId, std::string calendarId,
                    const undo::CommandOrigin origin)
                {
                    return launch(m_services.calendarCommandPort().setDefaultCalendar(
                        std::move(ownerAccountId), std::move(accountId), std::move(calendarId),
                        origin));
                });
        case Kind::CalendarCreate:
            return decodeAndApply<std::string, javelin::jmap::calendar::CreateCalendarCommand>(
                command.payload, invalidPayload,
                [&](std::string ownerAccountId,
                    javelin::jmap::calendar::CreateCalendarCommand calendarCommand)
                {
                    return launch(m_services.calendarCommandPort().createCalendar(
                        std::move(ownerAccountId), std::move(calendarCommand)));
                });
        case Kind::CalendarDelete:
            return decodeAndApply<std::string, javelin::jmap::calendar::DeleteCalendarCommand>(
                command.payload, invalidPayload,
                [&](std::string ownerAccountId,
                    javelin::jmap::calendar::DeleteCalendarCommand calendarCommand)
                {
                    return launch(m_services.calendarCommandPort().deleteCalendar(
                        std::move(ownerAccountId), std::move(calendarCommand)));
                });
        case Kind::CalendarSetVisible:
            return decodeAndApply<std::string, std::string, bool, undo::CommandOrigin>(
                command.payload, invalidPayload,
                [&](std::string accountId, std::string calendarId, const bool visible,
                    const undo::CommandOrigin origin)
                {
                    return immediate(m_services.calendarCommandPort().setCalendarVisible(
                        std::move(accountId), std::move(calendarId), visible, origin));
                });
        case Kind::ComposeOpen:
            return decodeAndApply<AccountConnectionSettings,
                                  javelin::jmap::submission::OpenComposeRequest>(
                command.payload, invalidPayload,
                [&](AccountConnectionSettings settings,
                    javelin::jmap::submission::OpenComposeRequest request)
                {
                    auto hydrated = m_connectionSettingsHydrator(std::move(settings));
                    if (const auto* error = std::get_if<QString>(&hydrated))
                        return reject(id, *error);
                    return launch(m_services.composeCommandPort().open(
                        std::get<AccountConnectionSettings>(std::move(hydrated)),
                        std::move(request)));
                });
        case Kind::ComposeLoadSenderIdentities:
            return decodeAndApply<AccountConnectionSettings, std::string>(
                command.payload, invalidPayload,
                [&](AccountConnectionSettings settings, std::string accountId)
                {
                    auto hydrated = m_connectionSettingsHydrator(std::move(settings));
                    if (const auto* error = std::get_if<QString>(&hydrated))
                        return reject(id, *error);
                    return launch(m_services.composeCommandPort().loadSenderIdentities(
                        std::get<AccountConnectionSettings>(std::move(hydrated)),
                        std::move(accountId)));
                });
        case Kind::ComposeSaveDraft:
            return decodeAndApply<AccountConnectionSettings,
                                  javelin::jmap::submission::DraftSnapshot>(
                command.payload, invalidPayload,
                [&](AccountConnectionSettings settings,
                    javelin::jmap::submission::DraftSnapshot snapshot)
                {
                    auto hydrated = m_connectionSettingsHydrator(std::move(settings));
                    if (const auto* error = std::get_if<QString>(&hydrated))
                        return reject(id, *error);
                    return launch(m_services.composeCommandPort().saveDraft(
                        std::get<AccountConnectionSettings>(std::move(hydrated)),
                        std::move(snapshot)));
                });
        case Kind::ComposeSend:
            return decodeAndApply<AccountConnectionSettings,
                                  javelin::jmap::submission::DraftSnapshot>(
                command.payload, invalidPayload,
                [&](AccountConnectionSettings settings,
                    javelin::jmap::submission::DraftSnapshot snapshot)
                {
                    auto hydrated = m_connectionSettingsHydrator(std::move(settings));
                    if (const auto* error = std::get_if<QString>(&hydrated))
                        return reject(id, *error);
                    return launch(m_services.composeCommandPort().send(
                        std::get<AccountConnectionSettings>(std::move(hydrated)),
                        std::move(snapshot)));
                });
        case Kind::ComposeLoadWorkingCopy:
            return decodeAndApply<std::string>(
                command.payload, invalidPayload, [&](const std::string& sessionId)
                { return immediate(m_services.composeCommandPort().loadWorkingCopy(sessionId)); });
        case Kind::ComposeStoreWorkingCopy:
            return decodeAndApply<javelin::jmap::submission::DraftSnapshot>(
                command.payload, invalidPayload,
                [&](const javelin::jmap::submission::DraftSnapshot& snapshot)
                { return immediate(m_services.composeCommandPort().storeWorkingCopy(snapshot)); });
        case Kind::ComposeDiscard:
            return decodeAndApply<std::string>(
                command.payload, invalidPayload, [&](const std::string& sessionId)
                { return immediate(m_services.composeCommandPort().discard(sessionId)); });
        case Kind::ContactRequestRefresh:
            return decodeAndApply<std::string>(
                command.payload, invalidPayload,
                [&](std::string ownerAccountId)
                {
                    return launch(
                        m_services.accountRefreshPort().requestContacts(std::move(ownerAccountId)));
                });
        case Kind::ContactMutateAddressBook:
            return decodeAndApply<std::string, AddressBookCommand>(
                command.payload, invalidPayload,
                [&](std::string ownerAccountId, AddressBookCommand action)
                {
                    return launch(m_services.contactCommandPort().mutateAddressBook(
                        std::move(ownerAccountId), std::move(action)));
                });
        case Kind::ContactSave:
            return decodeAndApply<std::string, SaveContactCommand>(
                command.payload, invalidPayload,
                [&](std::string ownerAccountId, SaveContactCommand action)
                {
                    return launch(m_services.contactCommandPort().saveContact(
                        std::move(ownerAccountId), std::move(action)));
                });
        case Kind::ContactSetStarred:
            return decodeAndApply<std::string, SetContactsStarredCommand>(
                command.payload, invalidPayload,
                [&](std::string ownerAccountId, SetContactsStarredCommand action)
                {
                    return launch(m_services.contactCommandPort().setContactsStarred(
                        std::move(ownerAccountId), std::move(action)));
                });
        case Kind::ContactDelete:
            return decodeAndApply<std::string, DeleteContactsCommand>(
                command.payload, invalidPayload,
                [&](std::string ownerAccountId, DeleteContactsCommand action)
                {
                    return launch(m_services.contactCommandPort().deleteContacts(
                        std::move(ownerAccountId), std::move(action)));
                });
        case Kind::ContactCreateGroup:
            return decodeAndApply<std::string, CreateContactGroupCommand>(
                command.payload, invalidPayload,
                [&](std::string ownerAccountId, CreateContactGroupCommand action)
                {
                    return launch(m_services.contactCommandPort().createContactGroup(
                        std::move(ownerAccountId), std::move(action)));
                });
        case Kind::ContactDeleteGroup:
            return decodeAndApply<std::string, DeleteContactGroupCommand>(
                command.payload, invalidPayload,
                [&](std::string ownerAccountId, DeleteContactGroupCommand action)
                {
                    return launch(m_services.contactCommandPort().deleteContactGroup(
                        std::move(ownerAccountId), std::move(action)));
                });
        case Kind::ContactSetGroupMembership:
            return decodeAndApply<std::string, SetContactGroupMembershipCommand>(
                command.payload, invalidPayload,
                [&](std::string ownerAccountId, SetContactGroupMembershipCommand action)
                {
                    return launch(m_services.contactCommandPort().setContactGroupMembership(
                        std::move(ownerAccountId), std::move(action)));
                });
        case Kind::ContactCopy:
            return decodeAndApply<std::string, CopyContactCommand>(
                command.payload, invalidPayload,
                [&](std::string ownerAccountId, CopyContactCommand action)
                {
                    return launch(m_services.contactCommandPort().copyContact(
                        std::move(ownerAccountId), std::move(action)));
                });
        case Kind::ContactImport:
            return decodeAndApply<std::string, ImportContactsCommand>(
                command.payload, invalidPayload,
                [&](std::string ownerAccountId, ImportContactsCommand action)
                {
                    return launch(m_services.contactCommandPort().importContacts(
                        std::move(ownerAccountId), std::move(action)));
                });
        case Kind::ContactMerge:
            return decodeAndApply<std::string, MergeContactsCommand>(
                command.payload, invalidPayload,
                [&](std::string ownerAccountId, MergeContactsCommand action)
                {
                    return launch(m_services.contactCommandPort().mergeContacts(
                        std::move(ownerAccountId), std::move(action)));
                });
        case Kind::ContactUploadMedia:
            return decodeAndApply<std::string, std::string, QByteArray, std::string>(
                command.payload, invalidPayload,
                [&](std::string ownerAccountId, std::string accountId, QByteArray payload,
                    std::string mediaType)
                {
                    return launch(m_services.contactCommandPort().uploadContactMedia(
                        std::move(ownerAccountId), std::move(accountId), std::move(payload),
                        std::move(mediaType)));
                });
        case Kind::ContactDownloadMedia:
            return decodeAndApply<std::string, std::string, std::string, std::string>(
                command.payload, invalidPayload,
                [&](std::string ownerAccountId, std::string accountId, std::string blobId,
                    std::string mediaType)
                {
                    return launch(m_services.contactCommandPort().downloadContactMedia(
                        std::move(ownerAccountId), std::move(accountId), std::move(blobId),
                        std::move(mediaType)));
                });
        case Kind::MailQueueMailboxMutation:
            return decodeAndApply<MailboxSelectionMutationIntent>(
                command.payload, invalidPayload,
                [&](MailboxSelectionMutationIntent intent)
                {
                    return launch(m_services.mailCommandPort().queueMailboxSelectionMutation(
                        std::move(intent)));
                });
        case Kind::MailQueueDestroy:
        case Kind::MailQueueMarkUnread:
            return decodeAndApply<std::string, std::optional<std::string>, MessageSelection>(
                command.payload, invalidPayload,
                [&](std::string accountId, std::optional<std::string> mailboxId,
                    MessageSelection selection)
                {
                    if (command.kind == Kind::MailQueueDestroy)
                        return launch(m_services.mailCommandPort().queueDestroyMessages(
                            std::move(accountId), std::move(mailboxId), std::move(selection)));
                    return launch(m_services.mailCommandPort().queueMarkMessagesUnread(
                        std::move(accountId), std::move(mailboxId), std::move(selection)));
                });
        case Kind::MailQueueMarkRead:
            return decodeAndApply<std::string, std::string>(
                command.payload, invalidPayload,
                [&](std::string accountId, std::string emailId)
                {
                    return launch(m_services.mailCommandPort().queueMarkEmailRead(
                        std::move(accountId), std::move(emailId)));
                });
        case Kind::MailQueueSetFlagged:
            return decodeAndApply<std::string, std::string, bool>(
                command.payload, invalidPayload,
                [&](std::string accountId, std::string emailId, const bool flagged)
                {
                    return launch(m_services.mailCommandPort().queueSetEmailFlagged(
                        std::move(accountId), std::move(emailId), flagged));
                });
        case Kind::MailSubmitPending:
            return decodeAndApply<std::string, std::optional<std::string>>(
                command.payload, invalidPayload,
                [&](std::string accountId, std::optional<std::string> operationGroupId)
                {
                    return launch(m_services.mailCommandPort().submitPendingEmailMutations(
                        std::move(accountId), std::move(operationGroupId)));
                });
        case Kind::SieveList:
            return decodeAndApply<std::string>(
                command.payload, invalidPayload,
                [&](std::string accountId)
                {
                    return launch(
                        m_services.sieveCommandPort().requestSieveScripts(std::move(accountId)));
                });
        case Kind::SieveGet:
            return decodeAndApply<std::string, javelin::jmap::sieve::SieveScript>(
                command.payload, invalidPayload,
                [&](std::string accountId, javelin::jmap::sieve::SieveScript script)
                {
                    return launch(m_services.sieveCommandPort().requestSieveScript(
                        std::move(accountId), std::move(script)));
                });
        case Kind::SieveValidate:
            return decodeAndApply<std::string, QByteArray>(
                command.payload, invalidPayload,
                [&](std::string accountId, QByteArray content)
                {
                    return launch(m_services.sieveCommandPort().validateSieveScript(
                        std::move(accountId), std::move(content)));
                });
        case Kind::SieveSave:
            return decodeAndApply<std::string, javelin::jmap::sieve::SieveScript, QByteArray,
                                  undo::CommandOrigin>(
                command.payload, invalidPayload,
                [&](std::string accountId, javelin::jmap::sieve::SieveScript script,
                    QByteArray content, const undo::CommandOrigin origin)
                {
                    return launch(m_services.sieveCommandPort().saveSieveScript(
                        std::move(accountId), std::move(script), std::move(content), origin));
                });
        case Kind::SieveDelete:
            return decodeAndApply<std::string, javelin::jmap::sieve::SieveScript,
                                  undo::CommandOrigin>(
                command.payload, invalidPayload,
                [&](std::string accountId, javelin::jmap::sieve::SieveScript script,
                    const undo::CommandOrigin origin)
                {
                    return launch(m_services.sieveCommandPort().deleteSieveScript(
                        std::move(accountId), std::move(script), origin));
                });
        case Kind::SieveActivate:
            return decodeAndApply<std::string, javelin::jmap::sieve::SieveScript, bool,
                                  undo::CommandOrigin>(
                command.payload, invalidPayload,
                [&](std::string accountId, javelin::jmap::sieve::SieveScript script,
                    const bool active, const undo::CommandOrigin origin)
                {
                    return launch(m_services.sieveCommandPort().setSieveScriptActive(
                        std::move(accountId), std::move(script), active, origin));
                });
        case Kind::IdentityList:
            return decodeAndApply<std::string>(
                command.payload, invalidPayload,
                [&](std::string accountId)
                {
                    return launch(m_services.identityCommandPort().requestSenderIdentities(
                        std::move(accountId)));
                });
        case Kind::IdentitySave:
            return decodeAndApply<std::string, javelin::jmap::domain::Identity>(
                command.payload, invalidPayload,
                [&](std::string accountId, javelin::jmap::domain::Identity identity)
                {
                    return launch(m_services.identityCommandPort().saveSenderIdentity(
                        std::move(accountId), std::move(identity)));
                });
        case Kind::IdentityDelete:
            return decodeAndApply<std::string, std::string>(
                command.payload, invalidPayload,
                [&](std::string accountId, std::string identityId)
                {
                    return launch(m_services.identityCommandPort().deleteSenderIdentity(
                        std::move(accountId), std::move(identityId)));
                });
        case Kind::AccountBootstrap:
            return decodeAndApply<AccountBootstrapIntent>(
                command.payload, invalidPayload,
                [&](AccountBootstrapIntent intent)
                {
                    return launch(
                        m_services.accountRefreshPort().bootstrapAccount(std::move(intent)));
                });
        case Kind::MessageContent:
            return decodeAndApply<std::string, std::string>(
                command.payload, invalidPayload,
                [&](std::string accountId, std::string emailId)
                {
                    return launch(m_services.messageContentPort().requestMessageContent(
                        std::move(accountId), std::move(emailId)));
                });
        case Kind::AttachmentDownload:
            return decodeAndApply<std::string, std::string, std::string>(
                command.payload, invalidPayload,
                [&](std::string accountId, std::string emailId, std::string partId)
                {
                    return launch(m_services.messageContentPort().requestAttachment(
                        std::move(accountId), std::move(emailId), std::move(partId)));
                });
        case Kind::MessageSource:
            return decodeAndApply<std::string, std::string>(
                command.payload, invalidPayload,
                [&](std::string accountId, std::string emailId)
                {
                    return launch(m_services.messageContentPort().requestMessageSource(
                        std::move(accountId), std::move(emailId)));
                });
        case Kind::MailboxObserve:
            return decodeAndApply<QString, std::string, std::string>(
                command.payload, invalidPayload,
                [&](QString observationId, std::string accountId, std::string mailboxId)
                {
                    if (observationId.isEmpty() || m_mailboxObservations.contains(observationId))
                    {
                        return reject(
                            id, QStringLiteral("The mailbox observation identifier is invalid."));
                    }
                    m_mailboxObservations.emplace(
                        observationId, std::make_unique<MailboxObservation>(
                                           m_services.mailService().observeMailbox(
                                               std::move(accountId), std::move(mailboxId))));
                    return empty();
                });
        case Kind::MailboxUnobserve:
            return decodeAndApply<QString>(command.payload, invalidPayload,
                                           [&](const QString& observationId)
                                           {
                                               m_mailboxObservations.erase(observationId);
                                               return empty();
                                           });
        case Kind::MailboxWindow:
            return decodeAndApply<MailboxWindowIntent>(
                command.payload, invalidPayload,
                [&](MailboxWindowIntent intent)
                {
                    return launch(m_services.mailService().requestMailboxWindow(std::move(intent)));
                });
        case Kind::SearchWindow:
            return decodeAndApply<SearchWindowIntent>(
                command.payload, invalidPayload,
                [&](SearchWindowIntent intent)
                {
                    return launch(m_services.mailService().requestSearchWindow(std::move(intent)));
                });
        case Kind::SearchRetire:
            return decodeAndApply<std::string, std::string>(
                command.payload, invalidPayload,
                [&](std::string accountId, std::string windowKey)
                {
                    m_services.mailService().retireSearchWindow(std::move(accountId),
                                                                std::move(windowKey));
                    return empty();
                });
        case Kind::Undo:
            return launch(performUndo(false));
        case Kind::Redo:
            return launch(performUndo(true));
        case Kind::UndoAcknowledgeRemove:
        case Kind::UndoForget:
            return decodeAndApply<QString>(
                command.payload, invalidPayload,
                [&](const QString& entryId)
                {
                    if (command.kind == Kind::UndoAcknowledgeRemove)
                    {
                        return immediate(
                            m_services.undoCommandPort().acknowledgeAndRemove(entryId));
                    }
                    return immediate(m_services.undoCommandPort().forget(entryId));
                });
        case Kind::UndoSnapshot:
            return immediate(std::tuple{m_services.undoCommandPort().state(),
                                        m_services.undoCommandPort().entries()});
        case Kind::ReloadSettings:
        {
            if (const auto error = m_reloadSettings())
                return reject(id, error->detail, error->code);
            return empty();
        }
        case Kind::WorkPause:
        case Kind::WorkResume:
        case Kind::WorkRetry:
            return decodeAndApply<std::string>(
                command.payload, invalidPayload,
                [&](const std::string& jobId)
                {
                    if (command.kind == Kind::WorkPause)
                        return immediate(m_services.workScheduler().pause(jobId));
                    if (command.kind == Kind::WorkResume)
                        return immediate(m_services.workScheduler().resume(jobId));
                    return immediate(m_services.workScheduler().retry(jobId));
                });
        case Kind::WorkList:
            return immediate(m_services.workScheduler().list());
        case Kind::WorkSummary:
            return immediate(m_services.workScheduler().summary());
        case Kind::DeveloperDiagnosticsSnapshot:
            return launch(m_services.developerDiagnosticsPort().snapshot());
        case Kind::DeveloperMailboxClear:
            return decodeAndApply<DeveloperMailboxClearCommand>(
                command.payload, invalidPayload,
                [&](DeveloperMailboxClearCommand clearCommand)
                {
                    return launch(m_services.developerMaintenancePort().clearMailboxCache(
                        std::move(clearCommand)));
                });
        case Kind::AcknowledgeRemoteActionResult:
            return reject(id,
                          QStringLiteral("Remote action acknowledgements are handled directly."),
                          javelin::protocol::BoundaryErrorCode::UnsupportedOperation);
        }
        return reject(id, QStringLiteral("The remote action is unsupported."),
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
                                                  const javelin::protocol::RemoteActionKind kind,
                                                  QByteArray result) const
    {
        return javelin::protocol::CommandAccepted{
            .id = id,
            .operation = std::nullopt,
            .epoch = m_currentEpoch(),
            .changedDomains = admissionDomains(kind),
            .affectedKeys = {},
            .immediateResult = std::move(result),
        };
    }

    javelin::protocol::CommandReply
    DaemonRemoteActionDispatcher::acceptAsync(const javelin::protocol::CommandId& id,
                                              const javelin::protocol::RemoteActionKind kind,
                                              const javelin::protocol::OperationId& operation) const
    {
        return javelin::protocol::CommandAccepted{
            .id = id,
            .operation = operation,
            .epoch = m_currentEpoch(),
            .changedDomains = admissionDomains(kind),
            .affectedKeys = {},
            .immediateResult = std::nullopt,
        };
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
                    .arg(PerformanceMetrics::remoteActionName(replay->second.kind))
                    .arg(result.size()));
            if (replay->second.reply.has_value())
            {
                auto updated = *replay->second.reply;
                if (auto* accepted = std::get_if<javelin::protocol::CommandAccepted>(&updated))
                {
                    accepted->operation = std::nullopt;
                    accepted->epoch = m_currentEpoch();
                    accepted->changedDomains = admissionDomains(replay->second.kind);
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
                    .arg(PerformanceMetrics::remoteActionName(replay->second.kind))
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
