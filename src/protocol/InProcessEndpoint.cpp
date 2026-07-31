#include "protocol/InProcessEndpoint.h"

#include <utility>

namespace javelin::protocol
{

    InProcessEndpoint::InProcessEndpoint(DaemonRequestHandler& handler, BoundaryLimits limits)
        : m_handler(handler), m_limits(limits)
    {
    }

    std::optional<BoundaryError> InProcessEndpoint::attachEventSink(BoundaryEventSink& sink)
    {
        if (m_eventSink != nullptr && m_eventSink != &sink)
        {
            return BoundaryError{.code = BoundaryErrorCode::Busy,
                                 .field = QStringLiteral("eventSink"),
                                 .detail = QStringLiteral("an event sink is already attached")};
        }
        m_eventSink = &sink;
        return std::nullopt;
    }

    void InProcessEndpoint::detachEventSink(BoundaryEventSink& sink)
    {
        if (m_eventSink == &sink)
            m_eventSink = nullptr;
    }

    void InProcessEndpoint::publishEvent(const BoundaryEvent& event)
    {
        if (m_eventSink != nullptr)
            m_eventSink->onBoundaryEvent(event);
    }

    CommandReply InProcessEndpoint::submitCommand(CommandRequest request)
    {
        const CommandId commandId = request.id;
        if (const auto error = validate(ClientRequest{request}, m_limits))
            return CommandRejected{.id = commandId, .error = *error};
        return m_handler.handleCommand(std::move(request));
    }

    MaterializationReply InProcessEndpoint::requestMaterialization(MaterializationRequest request)
    {
        const RequestId requestId = request.id;
        if (const auto error = validate(ClientRequest{request}, m_limits))
            return MaterializationRejected{.id = requestId, .error = *error};
        return m_handler.handleMaterialization(std::move(request));
    }

    void InProcessEndpoint::cancelMaterializationScope(const ScopeId scope)
    {
        if (validate(ClientRequest{CancelMaterializationScopeRequest{.scope = scope}}, m_limits))
            return;
        m_handler.handleCancelMaterializationScope({.scope = scope});
    }

    SettingsReadReply InProcessEndpoint::getSettings()
    {
        return m_handler.handleGetSettings({});
    }

    SettingsUpdateReply InProcessEndpoint::updateSettings(UpdateSettingsRequest request)
    {
        if (const auto error = validate(ClientRequest{request}, m_limits))
        {
            return SettingsUpdateRejected{.currentRevision = request.baseRevision, .error = *error};
        }
        return m_handler.handleUpdateSettings(std::move(request));
    }

    HandshakeReply InProcessEndpoint::hello(HelloRequest request)
    {
        if (const auto error = validate(ClientRequest{request}, m_limits))
            return HandshakeRejected{.error = *error};
        return m_handler.handleHello(request);
    }

    std::optional<BoundaryError> InProcessEndpoint::ping()
    {
        return m_handler.handlePing({});
    }

    std::optional<BoundaryError> InProcessEndpoint::readyForActivation()
    {
        return m_handler.handleGuiReadyForActivation();
    }

    std::optional<BoundaryError> InProcessEndpoint::acknowledgeCacheAccessSuspended(
        CacheAccessSuspendedAcknowledgement acknowledgement)
    {
        if (const auto error = validate(ClientRequest{acknowledgement}, m_limits))
            return error;
        return m_handler.handleCacheAccessSuspended(acknowledgement);
    }

} // namespace javelin::protocol
