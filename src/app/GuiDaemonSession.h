#pragma once

#include "app/CacheAccessBarrier.h"
#include "app/CacheLocationProvider.h"
#include "jmap/cache/Database.h"
#include "protocol/SocketTransport.h"

#include <QObject>

#include <memory>
#include <optional>

#include <cstdint>

class QProcess;

namespace javelin::app
{
    enum class GuiBootstrapErrorCode
    {
        DaemonUnavailable,
        DaemonStartFailed,
        IncompatibleDaemon,
        SettingsUnavailable,
        CacheUnavailable,
        CacheBarrierFailed,
    };

    struct GuiBootstrapError
    {
        GuiBootstrapErrorCode code = GuiBootstrapErrorCode::DaemonUnavailable;
        QString detail;
    };

    struct GuiDaemonSessionOptions
    {
        QString runtimeDirectory;
        QString socketPath;
        QString daemonExecutable;
        javelin::protocol::ProtocolVersion protocol;
        javelin::protocol::BuildIdentity build;
        int startTimeoutMilliseconds = 5000;
        bool startDaemonIfMissing = true;
    };

    class GuiDaemonSession final : public QObject, public javelin::protocol::BoundaryEventSink
    {
        Q_OBJECT

      public:
        explicit GuiDaemonSession(GuiDaemonSessionOptions options, QObject* parent = nullptr);
        ~GuiDaemonSession() override;

        GuiDaemonSession(const GuiDaemonSession&) = delete;
        GuiDaemonSession& operator=(const GuiDaemonSession&) = delete;
        GuiDaemonSession(GuiDaemonSession&&) = delete;
        GuiDaemonSession& operator=(GuiDaemonSession&&) = delete;

        [[nodiscard]] std::optional<GuiBootstrapError> start();
        [[nodiscard]] std::optional<GuiBootstrapError> reconnect();
        void stop();

        [[nodiscard]] bool isReady() const;
        [[nodiscard]] bool isInRecovery() const;
        [[nodiscard]] const javelin::protocol::ReadyReply& readyReply() const;
        [[nodiscard]] const javelin::protocol::SettingsSnapshot& settings() const;
        [[nodiscard]] std::optional<GuiBootstrapError>
        updateSettings(javelin::protocol::SettingsUpdate update);
        [[nodiscard]] std::optional<javelin::protocol::BoundaryError>
        requestAccountRefresh(const QString& accountId);
        [[nodiscard]] std::optional<javelin::protocol::BoundaryError>
        requestMailboxWindow(const QString& accountId, const QString& mailboxId,
                             std::uint64_t offset = 0, std::uint32_t limit = 100);
        [[nodiscard]] const QString& databasePath() const;
        [[nodiscard]] bool readConnectionOpen() const;

        void onBoundaryEvent(const javelin::protocol::BoundaryEvent& event) override;

      Q_SIGNALS:
        void ready();
        void recoveryStarted(const QString& detail);
        void recoveryFinished();
        void cacheChanged();
        void settingsChanged();
        void activationRequested(const javelin::protocol::ActivationRoute& route);
        void daemonShutdownRequested();

      private:
        [[nodiscard]] std::optional<GuiBootstrapError> connectAndHandshake(bool allowStart);
        [[nodiscard]] std::optional<GuiBootstrapError> loadSettingsAndCache();
        [[nodiscard]] std::optional<GuiBootstrapError> openReadConnection();
        [[nodiscard]] std::optional<GuiBootstrapError> suspendReadAccess();
        [[nodiscard]] std::optional<GuiBootstrapError> resumeReadAccess();
        [[nodiscard]] std::optional<GuiBootstrapError>
        mapBoundaryError(const javelin::protocol::BoundaryError& error,
                         GuiBootstrapErrorCode fallback) const;
        void onDaemonDisconnected(javelin::protocol::SocketDisconnectReason reason,
                                  const QString& detail);
        void acknowledgeCacheSuspend(javelin::protocol::CacheAccessSuspendRequested request);

        GuiDaemonSessionOptions m_options;
        std::unique_ptr<javelin::protocol::SocketDaemonClient> m_client;
        std::optional<javelin::protocol::ReadyReply> m_readyReply;
        javelin::protocol::SettingsSnapshot m_settings;
        CacheLocation m_cacheLocation;
        QString m_databasePath;
        javelin::jmap::cache::ReadOnlyDatabaseConnection m_readConnection;
        CacheAccessBarrier m_cacheAccessBarrier;
        CacheAccessBarrier::ParticipantId m_cacheParticipant = 0;
        std::uint64_t m_currentEpoch = 0;
        std::optional<javelin::protocol::ScopeId> m_materializationScope;
        bool m_inRecovery = false;
    };
} // namespace javelin::app
