#include "app/DaemonRemoteActionDispatcher.h"

#include "app/AccountApplicationPorts.h"
#include "app/AccountRefreshApplicationPorts.h"
#include "app/CalendarApplicationPorts.h"
#include "app/ComposeApplicationPorts.h"
#include "app/ContactApplicationPorts.h"
#include "app/DaemonServices.h"
#include "app/MailApplicationPorts.h"
#include "app/MailApplicationService.h"
#include "app/MessageContentApplicationPorts.h"
#include "app/RemoteCodec.h"
#include "app/SieveApplicationPorts.h"
#include "app/TranslationApplicationPorts.h"
#include "app/TranslationService.h"
#include "app/UndoApplicationPorts.h"
#include "app/WorkScheduler.h"

#include <QCoroTask>

#include <QTimer>

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
    } // namespace

    DaemonRemoteActionDispatcher::DaemonRemoteActionDispatcher(
        DaemonServices& services, javelin::protocol::BoundaryEventSink& eventSink,
        std::function<std::optional<javelin::protocol::BoundaryError>()> reloadSettings,
        QObject* parent)
        : QObject(parent), m_services(services), m_eventSink(eventSink),
          m_reloadSettings(std::move(reloadSettings))
    {
    }

    DaemonRemoteActionDispatcher::~DaemonRemoteActionDispatcher() = default;

    void DaemonRemoteActionDispatcher::releaseGuiResources()
    {
        m_mailboxObservations.clear();
        m_replays.clear();
    }

    javelin::protocol::CommandReply
    DaemonRemoteActionDispatcher::dispatch(javelin::protocol::CommandRequest request)
    {
        const auto id = request.id;
        const auto* remoteCommand =
            std::get_if<javelin::protocol::RemoteActionCommand>(&request.command);
        if (remoteCommand == nullptr)
            return reject(id, QStringLiteral("The request is not a remote action."),
                          javelin::protocol::BoundaryErrorCode::UnsupportedOperation);

        const auto key = replayKey(id);
        if (const auto replay = m_replays.find(key); replay != m_replays.end())
        {
            if (replay->second.command == *remoteCommand)
                return replay->second.reply;
            return reject(id, QStringLiteral("The command identifier was reused."));
        }

        auto reply = dispatchRemote(id, *remoteCommand);
        m_replays.emplace(key, ReplayEntry{.command = *remoteCommand, .reply = reply});
        return reply;
    }

    javelin::protocol::CommandReply DaemonRemoteActionDispatcher::dispatchRemote(
        const javelin::protocol::CommandId& id,
        const javelin::protocol::RemoteActionCommand& command)
    {
        const auto immediate =
            [this, &id]<typename Value>(Value&& value) -> javelin::protocol::CommandReply
        {
            const auto encoded = encodeResult(value);
            if (const auto* error = std::get_if<QString>(&encoded))
                return reject(id, *error);
            return acceptImmediate(id, std::get<QByteArray>(encoded));
        };
        const auto empty = [this, &id]() -> javelin::protocol::CommandReply
        {
            const auto encoded = encodeEmptyResult();
            if (const auto* error = std::get_if<QString>(&encoded))
                return reject(id, *error);
            return acceptImmediate(id, std::get<QByteArray>(encoded));
        };
        const auto launch = [this, &id]<typename Result>(
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
            return acceptAsync(id, operation);
        };
        const auto invalidPayload = [this, &id](const QString& detail)
        { return reject(id, detail); };

        using Kind = javelin::protocol::RemoteActionKind;
        switch (command.kind)
        {
        case Kind::RemoveConfiguredAccount:
        {
            auto arguments = decodeArguments<QString, QString, QStringList>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [loginEmail, sessionUrl, accountIds] =
                std::get<std::tuple<QString, QString, QStringList>>(std::move(arguments));
            return immediate(m_services.accountCommandPort().removeConfiguredAccount(
                loginEmail, sessionUrl, accountIds));
        }
        case Kind::CalendarReadCached:
        {
            auto arguments = decodeArguments<std::string, javelin::jmap::calendar::VisibleInterval,
                                             javelin::jmap::calendar::TimeZoneId>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [accountId, interval, timeZone] =
                std::get<std::tuple<std::string, javelin::jmap::calendar::VisibleInterval,
                                    javelin::jmap::calendar::TimeZoneId>>(std::move(arguments));
            return immediate(
                m_services.calendarService().loadCached(accountId, interval, timeZone));
        }
        case Kind::CalendarReadAccounts:
            return immediate(m_services.calendarService().accounts());
        case Kind::CalendarReadCalendars:
        {
            auto arguments = decodeArguments<std::string>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [accountId] = std::get<std::tuple<std::string>>(std::move(arguments));
            return immediate(m_services.calendarService().calendars(accountId));
        }
        case Kind::CalendarRequestRange:
        {
            auto arguments = decodeArguments<std::string, javelin::jmap::calendar::VisibleInterval,
                                             javelin::jmap::calendar::TimeZoneId>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [ownerAccountId, interval, timeZone] =
                std::get<std::tuple<std::string, javelin::jmap::calendar::VisibleInterval,
                                    javelin::jmap::calendar::TimeZoneId>>(std::move(arguments));
            return launch(m_services.calendarCommandPort().requestCalendarRange(
                std::move(ownerAccountId), std::move(interval), std::move(timeZone)));
        }
        case Kind::CalendarCreateEvent:
        {
            auto arguments =
                decodeArguments<std::string, javelin::jmap::calendar::CreateEventCommand,
                                undo::CommandOrigin>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [ownerAccountId, eventCommand, origin] =
                std::get<std::tuple<std::string, javelin::jmap::calendar::CreateEventCommand,
                                    undo::CommandOrigin>>(std::move(arguments));
            return launch(m_services.calendarCommandPort().createCalendarEvent(
                std::move(ownerAccountId), std::move(eventCommand), origin));
        }
        case Kind::CalendarUpdateEvent:
        {
            auto arguments =
                decodeArguments<std::string, javelin::jmap::calendar::UpdateEventCommand,
                                undo::CommandOrigin>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [ownerAccountId, eventCommand, origin] =
                std::get<std::tuple<std::string, javelin::jmap::calendar::UpdateEventCommand,
                                    undo::CommandOrigin>>(std::move(arguments));
            return launch(m_services.calendarCommandPort().updateCalendarEvent(
                std::move(ownerAccountId), std::move(eventCommand), origin));
        }
        case Kind::CalendarDeleteEvent:
        {
            auto arguments =
                decodeArguments<std::string, javelin::jmap::calendar::DeleteEventCommand,
                                undo::CommandOrigin>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [ownerAccountId, eventCommand, origin] =
                std::get<std::tuple<std::string, javelin::jmap::calendar::DeleteEventCommand,
                                    undo::CommandOrigin>>(std::move(arguments));
            return launch(m_services.calendarCommandPort().deleteCalendarEvent(
                std::move(ownerAccountId), std::move(eventCommand), origin));
        }
        case Kind::CalendarSetDefault:
        {
            auto arguments =
                decodeArguments<std::string, std::string, std::string, undo::CommandOrigin>(
                    command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [ownerAccountId, accountId, calendarId, origin] =
                std::get<std::tuple<std::string, std::string, std::string, undo::CommandOrigin>>(
                    std::move(arguments));
            return launch(m_services.calendarCommandPort().setDefaultCalendar(
                std::move(ownerAccountId), std::move(accountId), std::move(calendarId), origin));
        }
        case Kind::CalendarCreate:
        {
            auto arguments =
                decodeArguments<std::string, javelin::jmap::calendar::CreateCalendarCommand>(
                    command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [ownerAccountId, calendarCommand] =
                std::get<std::tuple<std::string, javelin::jmap::calendar::CreateCalendarCommand>>(
                    std::move(arguments));
            return launch(m_services.calendarCommandPort().createCalendar(
                std::move(ownerAccountId), std::move(calendarCommand)));
        }
        case Kind::CalendarDelete:
        {
            auto arguments =
                decodeArguments<std::string, javelin::jmap::calendar::DeleteCalendarCommand>(
                    command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [ownerAccountId, calendarCommand] =
                std::get<std::tuple<std::string, javelin::jmap::calendar::DeleteCalendarCommand>>(
                    std::move(arguments));
            return launch(m_services.calendarCommandPort().deleteCalendar(
                std::move(ownerAccountId), std::move(calendarCommand)));
        }
        case Kind::CalendarSetVisible:
        {
            auto arguments = decodeArguments<std::string, std::string, bool, undo::CommandOrigin>(
                command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [accountId, calendarId, visible, origin] =
                std::get<std::tuple<std::string, std::string, bool, undo::CommandOrigin>>(
                    std::move(arguments));
            return immediate(m_services.calendarCommandPort().setCalendarVisible(
                std::move(accountId), std::move(calendarId), visible, origin));
        }
        case Kind::ComposeOpen:
        {
            auto arguments =
                decodeArguments<AccountConnectionSettings,
                                javelin::jmap::submission::OpenComposeRequest>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [settings, request] =
                std::get<std::tuple<AccountConnectionSettings,
                                    javelin::jmap::submission::OpenComposeRequest>>(
                    std::move(arguments));
            return launch(
                m_services.composeCommandPort().open(std::move(settings), std::move(request)));
        }
        case Kind::ComposeLoadSenderIdentities:
        {
            auto arguments =
                decodeArguments<AccountConnectionSettings, std::string>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [settings, accountId] =
                std::get<std::tuple<AccountConnectionSettings, std::string>>(std::move(arguments));
            return launch(m_services.composeCommandPort().loadSenderIdentities(
                std::move(settings), std::move(accountId)));
        }
        case Kind::ComposeSaveDraft:
        {
            auto arguments =
                decodeArguments<AccountConnectionSettings,
                                javelin::jmap::submission::DraftSnapshot>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [settings, snapshot] = std::get<
                std::tuple<AccountConnectionSettings, javelin::jmap::submission::DraftSnapshot>>(
                std::move(arguments));
            return launch(m_services.composeCommandPort().saveDraft(std::move(settings),
                                                                    std::move(snapshot)));
        }
        case Kind::ComposeSend:
        {
            auto arguments =
                decodeArguments<AccountConnectionSettings,
                                javelin::jmap::submission::DraftSnapshot>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [settings, snapshot] = std::get<
                std::tuple<AccountConnectionSettings, javelin::jmap::submission::DraftSnapshot>>(
                std::move(arguments));
            return launch(
                m_services.composeCommandPort().send(std::move(settings), std::move(snapshot)));
        }
        case Kind::ComposeLoadWorkingCopy:
        {
            auto arguments = decodeArguments<std::string>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [sessionId] = std::get<std::tuple<std::string>>(std::move(arguments));
            return immediate(m_services.composeCommandPort().loadWorkingCopy(sessionId));
        }
        case Kind::ComposeStoreWorkingCopy:
        {
            auto arguments =
                decodeArguments<javelin::jmap::submission::DraftSnapshot>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [snapshot] = std::get<std::tuple<javelin::jmap::submission::DraftSnapshot>>(
                std::move(arguments));
            return immediate(m_services.composeCommandPort().storeWorkingCopy(snapshot));
        }
        case Kind::ComposeDiscard:
        {
            auto arguments = decodeArguments<std::string>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [sessionId] = std::get<std::tuple<std::string>>(std::move(arguments));
            return immediate(m_services.composeCommandPort().discard(sessionId));
        }
        case Kind::ContactRequestRefresh:
        {
            auto arguments = decodeArguments<std::string>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [ownerAccountId] = std::get<std::tuple<std::string>>(std::move(arguments));
            return launch(
                m_services.accountRefreshPort().requestContacts(std::move(ownerAccountId)));
        }
        case Kind::ContactMutateAddressBook:
        {
            auto arguments = decodeArguments<std::string, AddressBookCommand>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [ownerAccountId, action] =
                std::get<std::tuple<std::string, AddressBookCommand>>(std::move(arguments));
            return launch(m_services.contactCommandPort().mutateAddressBook(
                std::move(ownerAccountId), std::move(action)));
        }
        case Kind::ContactSave:
        {
            auto arguments = decodeArguments<std::string, SaveContactCommand>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [ownerAccountId, action] =
                std::get<std::tuple<std::string, SaveContactCommand>>(std::move(arguments));
            return launch(m_services.contactCommandPort().saveContact(std::move(ownerAccountId),
                                                                      std::move(action)));
        }
        case Kind::ContactSetStarred:
        {
            auto arguments =
                decodeArguments<std::string, SetContactsStarredCommand>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [ownerAccountId, action] =
                std::get<std::tuple<std::string, SetContactsStarredCommand>>(std::move(arguments));
            return launch(m_services.contactCommandPort().setContactsStarred(
                std::move(ownerAccountId), std::move(action)));
        }
        case Kind::ContactDelete:
        {
            auto arguments = decodeArguments<std::string, DeleteContactsCommand>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [ownerAccountId, action] =
                std::get<std::tuple<std::string, DeleteContactsCommand>>(std::move(arguments));
            return launch(m_services.contactCommandPort().deleteContacts(std::move(ownerAccountId),
                                                                         std::move(action)));
        }
        case Kind::ContactCreateGroup:
        {
            auto arguments =
                decodeArguments<std::string, CreateContactGroupCommand>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [ownerAccountId, action] =
                std::get<std::tuple<std::string, CreateContactGroupCommand>>(std::move(arguments));
            return launch(m_services.contactCommandPort().createContactGroup(
                std::move(ownerAccountId), std::move(action)));
        }
        case Kind::ContactDeleteGroup:
        {
            auto arguments =
                decodeArguments<std::string, DeleteContactGroupCommand>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [ownerAccountId, action] =
                std::get<std::tuple<std::string, DeleteContactGroupCommand>>(std::move(arguments));
            return launch(m_services.contactCommandPort().deleteContactGroup(
                std::move(ownerAccountId), std::move(action)));
        }
        case Kind::ContactSetGroupMembership:
        {
            auto arguments =
                decodeArguments<std::string, SetContactGroupMembershipCommand>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [ownerAccountId, action] =
                std::get<std::tuple<std::string, SetContactGroupMembershipCommand>>(
                    std::move(arguments));
            return launch(m_services.contactCommandPort().setContactGroupMembership(
                std::move(ownerAccountId), std::move(action)));
        }
        case Kind::ContactCopy:
        {
            auto arguments = decodeArguments<std::string, CopyContactCommand>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [ownerAccountId, action] =
                std::get<std::tuple<std::string, CopyContactCommand>>(std::move(arguments));
            return launch(m_services.contactCommandPort().copyContact(std::move(ownerAccountId),
                                                                      std::move(action)));
        }
        case Kind::ContactImport:
        {
            auto arguments = decodeArguments<std::string, ImportContactsCommand>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [ownerAccountId, action] =
                std::get<std::tuple<std::string, ImportContactsCommand>>(std::move(arguments));
            return launch(m_services.contactCommandPort().importContacts(std::move(ownerAccountId),
                                                                         std::move(action)));
        }
        case Kind::ContactMerge:
        {
            auto arguments = decodeArguments<std::string, MergeContactsCommand>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [ownerAccountId, action] =
                std::get<std::tuple<std::string, MergeContactsCommand>>(std::move(arguments));
            return launch(m_services.contactCommandPort().mergeContacts(std::move(ownerAccountId),
                                                                        std::move(action)));
        }
        case Kind::ContactUploadMedia:
        {
            auto arguments =
                decodeArguments<std::string, std::string, QByteArray, std::string>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [ownerAccountId, accountId, payload, mediaType] =
                std::get<std::tuple<std::string, std::string, QByteArray, std::string>>(
                    std::move(arguments));
            return launch(m_services.contactCommandPort().uploadContactMedia(
                std::move(ownerAccountId), std::move(accountId), std::move(payload),
                std::move(mediaType)));
        }
        case Kind::ContactDownloadMedia:
        {
            auto arguments = decodeArguments<std::string, std::string, std::string, std::string>(
                command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [ownerAccountId, accountId, blobId, mediaType] =
                std::get<std::tuple<std::string, std::string, std::string, std::string>>(
                    std::move(arguments));
            return launch(m_services.contactCommandPort().downloadContactMedia(
                std::move(ownerAccountId), std::move(accountId), std::move(blobId),
                std::move(mediaType)));
        }
        case Kind::MailQueueMailboxMutation:
        {
            auto arguments = decodeArguments<MailboxSelectionMutationIntent>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [intent] =
                std::get<std::tuple<MailboxSelectionMutationIntent>>(std::move(arguments));
            return launch(
                m_services.mailCommandPort().queueMailboxSelectionMutation(std::move(intent)));
        }
        case Kind::MailQueueDestroy:
        case Kind::MailQueueMarkUnread:
        {
            auto arguments =
                decodeArguments<std::string, std::optional<std::string>, MessageSelection>(
                    command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [accountId, mailboxId, selection] =
                std::get<std::tuple<std::string, std::optional<std::string>, MessageSelection>>(
                    std::move(arguments));
            if (command.kind == Kind::MailQueueDestroy)
                return launch(m_services.mailCommandPort().queueDestroyMessages(
                    std::move(accountId), std::move(mailboxId), std::move(selection)));
            return launch(m_services.mailCommandPort().queueMarkMessagesUnread(
                std::move(accountId), std::move(mailboxId), std::move(selection)));
        }
        case Kind::MailQueueMarkRead:
        {
            auto arguments = decodeArguments<std::string, std::string>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [accountId, emailId] =
                std::get<std::tuple<std::string, std::string>>(std::move(arguments));
            return launch(m_services.mailCommandPort().queueMarkEmailRead(std::move(accountId),
                                                                          std::move(emailId)));
        }
        case Kind::MailQueueSetFlagged:
        {
            auto arguments = decodeArguments<std::string, std::string, bool>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [accountId, emailId, flagged] =
                std::get<std::tuple<std::string, std::string, bool>>(std::move(arguments));
            return launch(m_services.mailCommandPort().queueSetEmailFlagged(
                std::move(accountId), std::move(emailId), flagged));
        }
        case Kind::MailSubmitPending:
        {
            auto arguments =
                decodeArguments<std::string, std::optional<std::string>>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [accountId, operationGroupId] =
                std::get<std::tuple<std::string, std::optional<std::string>>>(std::move(arguments));
            return launch(m_services.mailCommandPort().submitPendingEmailMutations(
                std::move(accountId), std::move(operationGroupId)));
        }
        case Kind::SieveList:
        {
            auto arguments = decodeArguments<std::string>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [accountId] = std::get<std::tuple<std::string>>(std::move(arguments));
            return launch(m_services.sieveCommandPort().requestSieveScripts(std::move(accountId)));
        }
        case Kind::SieveGet:
        {
            auto arguments =
                decodeArguments<std::string, javelin::jmap::sieve::SieveScript>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [accountId, script] =
                std::get<std::tuple<std::string, javelin::jmap::sieve::SieveScript>>(
                    std::move(arguments));
            return launch(m_services.sieveCommandPort().requestSieveScript(std::move(accountId),
                                                                           std::move(script)));
        }
        case Kind::SieveValidate:
        {
            auto arguments = decodeArguments<std::string, QByteArray>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [accountId, content] =
                std::get<std::tuple<std::string, QByteArray>>(std::move(arguments));
            return launch(m_services.sieveCommandPort().validateSieveScript(std::move(accountId),
                                                                            std::move(content)));
        }
        case Kind::SieveSave:
        {
            auto arguments = decodeArguments<std::string, javelin::jmap::sieve::SieveScript,
                                             QByteArray, undo::CommandOrigin>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [accountId, script, content, origin] =
                std::get<std::tuple<std::string, javelin::jmap::sieve::SieveScript, QByteArray,
                                    undo::CommandOrigin>>(std::move(arguments));
            return launch(m_services.sieveCommandPort().saveSieveScript(
                std::move(accountId), std::move(script), std::move(content), origin));
        }
        case Kind::SieveDelete:
        {
            auto arguments = decodeArguments<std::string, javelin::jmap::sieve::SieveScript,
                                             undo::CommandOrigin>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [accountId, script, origin] = std::get<
                std::tuple<std::string, javelin::jmap::sieve::SieveScript, undo::CommandOrigin>>(
                std::move(arguments));
            return launch(m_services.sieveCommandPort().deleteSieveScript(
                std::move(accountId), std::move(script), origin));
        }
        case Kind::SieveActivate:
        {
            auto arguments = decodeArguments<std::string, javelin::jmap::sieve::SieveScript, bool,
                                             undo::CommandOrigin>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [accountId, script, active, origin] =
                std::get<std::tuple<std::string, javelin::jmap::sieve::SieveScript, bool,
                                    undo::CommandOrigin>>(std::move(arguments));
            return launch(m_services.sieveCommandPort().setSieveScriptActive(
                std::move(accountId), std::move(script), active, origin));
        }
        case Kind::AccountBootstrap:
        {
            auto arguments = decodeArguments<AccountBootstrapIntent>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [intent] = std::get<std::tuple<AccountBootstrapIntent>>(std::move(arguments));
            return launch(m_services.accountRefreshPort().bootstrapAccount(std::move(intent)));
        }
        case Kind::MessageContent:
        {
            auto arguments = decodeArguments<std::string, std::string>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [accountId, emailId] =
                std::get<std::tuple<std::string, std::string>>(std::move(arguments));
            return launch(m_services.messageContentPort().requestMessageContent(
                std::move(accountId), std::move(emailId)));
        }
        case Kind::AttachmentDownload:
        {
            auto arguments =
                decodeArguments<std::string, std::string, std::string>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [accountId, emailId, partId] =
                std::get<std::tuple<std::string, std::string, std::string>>(std::move(arguments));
            return launch(m_services.messageContentPort().requestAttachment(
                std::move(accountId), std::move(emailId), std::move(partId)));
        }
        case Kind::MessageSource:
        {
            auto arguments = decodeArguments<std::string, std::string>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [accountId, emailId] =
                std::get<std::tuple<std::string, std::string>>(std::move(arguments));
            return launch(m_services.messageContentPort().requestMessageSource(std::move(accountId),
                                                                               std::move(emailId)));
        }
        case Kind::MailboxObserve:
        {
            auto arguments = decodeArguments<QString, std::string, std::string>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [observationId, accountId, mailboxId] =
                std::get<std::tuple<QString, std::string, std::string>>(std::move(arguments));
            if (observationId.isEmpty() || m_mailboxObservations.contains(observationId))
                return reject(id, QStringLiteral("The mailbox observation identifier is invalid."));
            m_mailboxObservations.emplace(
                observationId,
                std::make_unique<MailboxObservation>(m_services.mailService().observeMailbox(
                    std::move(accountId), std::move(mailboxId))));
            return empty();
        }
        case Kind::MailboxUnobserve:
        {
            auto arguments = decodeArguments<QString>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [observationId] = std::get<std::tuple<QString>>(std::move(arguments));
            m_mailboxObservations.erase(observationId);
            return empty();
        }
        case Kind::MailboxWindow:
        {
            auto arguments = decodeArguments<MailboxWindowIntent>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [intent] = std::get<std::tuple<MailboxWindowIntent>>(std::move(arguments));
            return launch(m_services.mailService().requestMailboxWindow(std::move(intent)));
        }
        case Kind::SearchWindow:
        {
            auto arguments = decodeArguments<SearchWindowIntent>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [intent] = std::get<std::tuple<SearchWindowIntent>>(std::move(arguments));
            return launch(m_services.mailService().requestSearchWindow(std::move(intent)));
        }
        case Kind::SearchRetire:
        {
            auto arguments = decodeArguments<std::string, std::string>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [accountId, windowKey] =
                std::get<std::tuple<std::string, std::string>>(std::move(arguments));
            m_services.mailService().retireSearchWindow(std::move(accountId), std::move(windowKey));
            return empty();
        }
        case Kind::TranslationSetAutoSender:
        case Kind::TranslationSetAutoDomain:
        {
            auto arguments = decodeArguments<QString, bool>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [value, enabled] = std::get<std::tuple<QString, bool>>(std::move(arguments));
            if (command.kind == Kind::TranslationSetAutoSender)
                m_services.translationService().setAutoTranslateSender(std::move(value), enabled);
            else
                m_services.translationService().setAutoTranslateDomain(std::move(value), enabled);
            return empty();
        }
        case Kind::TranslationTranslate:
        {
            auto arguments = decodeArguments<TranslationChunks, QString, bool>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [chunks, sourceLanguage, allowNetwork] =
                std::get<std::tuple<TranslationChunks, QString, bool>>(std::move(arguments));
            return launch(m_services.translationService().translate(
                std::move(chunks), std::move(sourceLanguage), allowNetwork));
        }
        case Kind::Undo:
            return launch(performUndo(false));
        case Kind::Redo:
            return launch(performUndo(true));
        case Kind::UndoAcknowledgeRemove:
        case Kind::UndoForget:
        {
            auto arguments = decodeArguments<QString>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [entryId] = std::get<std::tuple<QString>>(std::move(arguments));
            if (command.kind == Kind::UndoAcknowledgeRemove)
                return immediate(m_services.undoCommandPort().acknowledgeAndRemove(entryId));
            return immediate(m_services.undoCommandPort().forget(entryId));
        }
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
        {
            auto arguments = decodeArguments<std::string>(command.payload);
            if (const auto* error = std::get_if<QString>(&arguments))
                return invalidPayload(*error);
            auto [jobId] = std::get<std::tuple<std::string>>(std::move(arguments));
            if (command.kind == Kind::WorkPause)
                return immediate(m_services.workScheduler().pause(jobId));
            if (command.kind == Kind::WorkResume)
                return immediate(m_services.workScheduler().resume(jobId));
            return immediate(m_services.workScheduler().retry(jobId));
        }
        case Kind::WorkList:
            return immediate(m_services.workScheduler().list());
        case Kind::WorkSummary:
            return immediate(m_services.workScheduler().summary());
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
                                                  QByteArray result) const
    {
        return javelin::protocol::CommandAccepted{
            .id = id,
            .operation = std::nullopt,
            .epoch = {},
            .changedDomains = {},
            .affectedKeys = {},
            .immediateResult = std::move(result),
        };
    }

    javelin::protocol::CommandReply
    DaemonRemoteActionDispatcher::acceptAsync(const javelin::protocol::CommandId& id,
                                              const javelin::protocol::OperationId& operation) const
    {
        return javelin::protocol::CommandAccepted{
            .id = id,
            .operation = operation,
            .epoch = {},
            .changedDomains = {},
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
        m_eventSink.onBoundaryEvent(javelin::protocol::OperationCompleted{
            .operation = operation,
            .result = std::move(result),
        });
    }

    void DaemonRemoteActionDispatcher::fail(const javelin::protocol::OperationId& operation,
                                            QString detail)
    {
        m_eventSink.onBoundaryEvent(javelin::protocol::OperationFailed{
            .operation = operation,
            .error = {.code = javelin::protocol::BoundaryErrorCode::ProtocolViolation,
                      .field = QStringLiteral("command.remote.result"),
                      .detail = std::move(detail)},
        });
    }

    QString DaemonRemoteActionDispatcher::replayKey(const javelin::protocol::CommandId& id)
    {
        return id.value.toString(QUuid::WithoutBraces);
    }
} // namespace javelin::app
