#pragma once

#include "protocol/ProtocolTypes.h"

#include <QString>

#include <cstdint>
#include <optional>
#include <variant>

namespace javelin::protocol
{
    struct OpenMailboxRoute
    {
        QString accountId;
        QString mailboxId;
        QString activationToken;
    };

    struct OpenMessageRoute
    {
        QString accountId;
        QString mailboxId;
        QString mailboxName;
        QString threadId;
        QString emailId;
        QString activationToken;
    };

    struct OpenComposeRoute
    {
        QString composeSessionId;
        QString activationToken;
    };

    struct OpenSettingsRoute
    {
        QString connectionId;
        QString activationToken;
    };

    struct OpenCalendarEventRoute
    {
        QString calendarAccountId;
        QString eventId;
        std::optional<QString> recurrenceId;
        QString navigationDate;
        QString activationToken;
    };

    struct RestoreDraftRoute
    {
        QString accountId;
        QString draftEmailId;
        QString composeSessionId;
        QString activationToken;
    };

    enum class WorkspaceSection : std::uint8_t
    {
        Inbox,
        Contacts,
        Calendar,
    };

    struct OpenWorkspaceRoute
    {
        WorkspaceSection section = WorkspaceSection::Inbox;
        QString activationToken;
    };

    struct NewMessageRoute
    {
        QString activationToken;
    };

    struct OpenTaskCenterRoute
    {
        QString activationToken;
    };

    struct RaiseGuiRoute
    {
        QString activationToken;
    };

    struct OpenMailtoRoute
    {
        QString uri;
        QString activationToken;
    };

    struct ShowUndoSendDialogRoute
    {
        QString sendId;
        QString title;
        QString message;
        qint64 deadlineEpochMilliseconds = 0;
    };

    struct CloseUndoSendDialogRoute
    {
        QString sendId;
    };

    using ActivationRoute =
        std::variant<OpenMailboxRoute, OpenMessageRoute, OpenComposeRoute, RaiseGuiRoute,
                     OpenSettingsRoute, OpenCalendarEventRoute, RestoreDraftRoute,
                     OpenTaskCenterRoute, OpenMailtoRoute, ShowUndoSendDialogRoute,
                     CloseUndoSendDialogRoute, OpenWorkspaceRoute, NewMessageRoute>;

    struct ActivationRequested
    {
        ActivationRoute route;
    };

    class ActivationRequestHandler
    {
      public:
        virtual ~ActivationRequestHandler() = default;
        [[nodiscard]] virtual std::optional<BoundaryError>
        handleGuiActivation(const ActivationRoute& route) = 0;
    };

    class ActivationClient
    {
      public:
        virtual ~ActivationClient() = default;
        [[nodiscard]] virtual std::optional<BoundaryError> readyForActivation() = 0;
    };
} // namespace javelin::protocol
