#include "app/CommandDispatcher.h"

#include <type_traits>
#include <utility>

namespace javelin::app
{
    CommandDispatcher::CommandDispatcher(AccountRefreshPort& refreshPort,
                                         protocol::BoundaryLimits limits)
        : m_refreshPort(refreshPort), m_limits(limits)
    {
    }

    protocol::CommandReply CommandDispatcher::dispatch(protocol::CommandRequest request)
    {
        const auto commandId = request.id;
        if (const auto error = protocol::validate(protocol::ClientRequest{request}, m_limits))
            return reject(commandId, *error);

        const auto key = replayKey(commandId);
        if (const auto replay = m_replays.find(key); replay != m_replays.end())
        {
            if (sameCommand(replay->second.request.command, request.command))
                return replay->second.reply;

            return reject(commandId, {.code = protocol::BoundaryErrorCode::InvalidRequest,
                                      .field = QStringLiteral("command.id"),
                                      .detail = QStringLiteral(
                                          "command id was reused for a different command")});
        }

        const auto* refresh = std::get_if<protocol::RefreshAccountCommand>(&request.command);
        if (refresh == nullptr)
        {
            return reject(commandId, {.code = protocol::BoundaryErrorCode::UnsupportedOperation,
                                      .field = QStringLiteral("command"),
                                      .detail = QStringLiteral(
                                          "command is not supported by this dispatcher")});
        }

        if (!m_refreshPort.requestAccountSynchronization(refresh->accountId.toStdString()))
        {
            return reject(commandId,
                          {.code = protocol::BoundaryErrorCode::NoUsableAccountConfiguration,
                           .field = QStringLiteral("command.accountId"),
                           .detail = QStringLiteral("account synchronization is not configured")});
        }

        ++m_epoch.value;
        const auto reply = protocol::CommandAccepted{
            .id = commandId,
            .operation = protocol::OperationId{.value = QUuid::createUuid()},
            .epoch = m_epoch,
            .changedDomains = {protocol::ChangedDomain::MailboxTree,
                               protocol::ChangedDomain::MailQueryWindows,
                               protocol::ChangedDomain::MessageMetadata},
            .affectedKeys = {refresh->accountId},
            .immediateResult = std::nullopt,
        };
        m_replays.emplace(key, ReplayEntry{.request = std::move(request), .reply = reply});
        m_replayOrder.push_back(key);
        constexpr std::size_t maximumReplayEntries = 512;
        while (m_replays.size() > maximumReplayEntries && !m_replayOrder.empty())
        {
            m_replays.erase(m_replayOrder.front());
            m_replayOrder.pop_front();
        }
        return reply;
    }

    protocol::InvalidationEpoch CommandDispatcher::currentEpoch() const
    {
        return m_epoch;
    }

    void CommandDispatcher::setEventSink(protocol::BoundaryEventSink* sink) noexcept
    {
        m_eventSink = sink;
    }

    void CommandDispatcher::publishOperationFailure(protocol::OperationId operation,
                                                    protocol::BoundaryError error) const
    {
        if (m_eventSink == nullptr)
            return;
        m_eventSink->onBoundaryEvent(
            protocol::OperationFailed{.operation = operation, .error = std::move(error)});
    }

    bool CommandDispatcher::sameCommand(const protocol::ApplicationCommand& left,
                                        const protocol::ApplicationCommand& right)
    {
        return std::visit(
            [](const auto& leftValue, const auto& rightValue)
            {
                using Left = std::decay_t<decltype(leftValue)>;
                using Right = std::decay_t<decltype(rightValue)>;
                if constexpr (std::is_same_v<Left, Right>)
                    return leftValue == rightValue;
                else
                    return false;
            },
            left, right);
    }

    QString CommandDispatcher::replayKey(const protocol::CommandId& id)
    {
        return id.value.toString(QUuid::WithoutBraces);
    }

    protocol::CommandReply CommandDispatcher::reject(const protocol::CommandId& id,
                                                     protocol::BoundaryError error) const
    {
        return protocol::CommandRejected{.id = id, .error = std::move(error)};
    }
} // namespace javelin::app
