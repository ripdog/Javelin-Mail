#pragma once

#include "protocol/SocketTransport.h"

#include <QObject>

#include <cstddef>
#include <deque>
#include <memory>
#include <optional>

namespace javelin::app
{
    class DaemonServices;
    class DaemonBackgroundController;
    class DaemonRemoteActionDispatcher;
    class SettingsRepository;

    struct DaemonProcessOptions
    {
        javelin::protocol::SocketEndpointOptions socket;
        javelin::protocol::ProtocolVersion protocol;
        javelin::protocol::BuildIdentity build;
        QString guiExecutable;
        QString cacheRootPath;
        QString settingsPath;
    };

    enum class DaemonStartupErrorCode
    {
        SettingsMigration,
        CacheLocation,
        CacheOpen,
        SocketListen,
    };

    struct DaemonStartupError
    {
        DaemonStartupErrorCode code = DaemonStartupErrorCode::CacheOpen;
        QString detail;
    };

    class DaemonProcess final : public QObject,
                                public javelin::protocol::DaemonRequestHandler,
                                public javelin::protocol::BoundaryEventSink
    {
        Q_OBJECT

      public:
        explicit DaemonProcess(DaemonProcessOptions options, QObject* parent = nullptr);
        ~DaemonProcess() override;

        DaemonProcess(const DaemonProcess&) = delete;
        DaemonProcess& operator=(const DaemonProcess&) = delete;
        DaemonProcess(DaemonProcess&&) = delete;
        DaemonProcess& operator=(DaemonProcess&&) = delete;

        [[nodiscard]] std::optional<DaemonStartupError> start();
        void stop();

        [[nodiscard]] bool isReady() const;
        [[nodiscard]] bool hasGuiConnection() const;
        void enqueueActivation(javelin::protocol::ActivationRoute route);
        [[nodiscard]] std::size_t pendingActivationCount() const;
        [[nodiscard]] const QString& databasePath() const;
        [[nodiscard]] const javelin::protocol::SettingsSnapshot& settings() const;
        [[nodiscard]] const javelin::protocol::DaemonInstanceId& instanceId() const;
        [[nodiscard]] javelin::protocol::CacheIdentity cacheIdentity() const;
        [[nodiscard]] javelin::protocol::InvalidationEpoch currentEpoch() const;

        // The daemon calls this before any cache migration or replacement. The connected GUI
        // must close every read connection and acknowledge the request before the operation may
        // continue.
        [[nodiscard]] std::optional<javelin::protocol::BoundaryError> requestCacheAccessSuspend(
            javelin::protocol::CacheSuspendReason reason,
            std::optional<javelin::protocol::CacheSchemaVersion> targetSchema);
        [[nodiscard]] std::optional<javelin::protocol::BoundaryError> completeCacheAccessResume();

        [[nodiscard]] javelin::protocol::HandshakeReply
        handleHello(const javelin::protocol::HelloRequest& request) override;
        [[nodiscard]] javelin::protocol::CommandReply
        handleCommand(javelin::protocol::CommandRequest request) override;
        [[nodiscard]] javelin::protocol::MaterializationReply
        handleMaterialization(javelin::protocol::MaterializationRequest request) override;
        void handleCancelMaterializationScope(
            const javelin::protocol::CancelMaterializationScopeRequest& request) override;
        [[nodiscard]] javelin::protocol::SettingsReadReply
        handleGetSettings(const javelin::protocol::GetSettingsRequest& request) override;
        [[nodiscard]] javelin::protocol::SettingsUpdateReply
        handleUpdateSettings(javelin::protocol::UpdateSettingsRequest request) override;
        [[nodiscard]] std::optional<javelin::protocol::BoundaryError> handleCacheAccessSuspended(
            const javelin::protocol::CacheAccessSuspendedAcknowledgement& acknowledgement) override;
        [[nodiscard]] std::optional<javelin::protocol::BoundaryError>
        handlePing(const javelin::protocol::PingRequest& request) override;
        [[nodiscard]] std::optional<javelin::protocol::BoundaryError>
        handleGuiReadyForActivation() override;
        [[nodiscard]] std::optional<javelin::protocol::BoundaryError>
        handleGuiActivation(const javelin::protocol::ActivationRoute& route) override;

        void onBoundaryEvent(const javelin::protocol::BoundaryEvent& event) override;

      Q_SIGNALS:
        void ready();
        void guiConnected();
        void guiDisconnected();
        void shutdownRequested();

      private:
        [[nodiscard]] std::optional<DaemonStartupError> fail(DaemonStartupErrorCode code,
                                                             QString detail);
        [[nodiscard]] javelin::protocol::BoundaryError notReadyError() const;
        [[nodiscard]] javelin::protocol::DaemonStatus daemonStatus() const;
        void publishStatus();
        void applySettings();
        [[nodiscard]] std::optional<javelin::protocol::BoundaryError> reloadSettings();
        void connectOperationalEvents();
        void flushPendingActivations();
        void launchGuiIfNeeded();
        void requestShutdown();
        void onSocketConnectionClosed(javelin::protocol::SocketDisconnectReason reason,
                                      const QString& detail);

        DaemonProcessOptions m_options;
        std::unique_ptr<SettingsRepository> m_settingsRepository;
        std::unique_ptr<DaemonServices> m_services;
        std::unique_ptr<DaemonRemoteActionDispatcher> m_remoteActions;
        std::unique_ptr<DaemonBackgroundController> m_background;
        std::unique_ptr<javelin::protocol::SocketDaemonEndpoint> m_endpoint;
        std::unique_ptr<javelin::protocol::SocketActivationEndpoint> m_activationEndpoint;
        javelin::protocol::SettingsSnapshot m_settingsSnapshot;
        javelin::protocol::DaemonInstanceId m_instanceId{.value = QUuid::createUuid()};
        javelin::protocol::InvalidationEpoch m_epoch;
        std::optional<javelin::protocol::CacheAccessSuspendRequested> m_cacheSuspend;
        bool m_cacheAccessAcknowledged = false;
        javelin::protocol::DaemonLifecycle m_lifecycle =
            javelin::protocol::DaemonLifecycle::Starting;
        bool m_guiConnected = false;
        bool m_guiReady = false;
        bool m_guiLaunchRequested = false;
        std::deque<javelin::protocol::ActivationRoute> m_pendingActivations;
    };
} // namespace javelin::app
