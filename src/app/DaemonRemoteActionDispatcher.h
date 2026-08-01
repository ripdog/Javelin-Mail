#pragma once

#include "app/RemoteActionTypes.h"
#include "protocol/ProcessBoundary.h"

#include <QCoroTask>

#include <QObject>

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>

namespace javelin::app
{
    class DaemonServices;
    class MailboxObservation;

    class DaemonRemoteActionDispatcher final : public QObject
    {
        Q_OBJECT

      public:
        DaemonRemoteActionDispatcher(
            DaemonServices& services, javelin::protocol::BoundaryEventSink& eventSink,
            std::function<std::optional<javelin::protocol::BoundaryError>()> reloadSettings,
            QObject* parent = nullptr);
        ~DaemonRemoteActionDispatcher() override;

        [[nodiscard]] javelin::protocol::CommandReply
        dispatch(javelin::protocol::CommandRequest request);
        void releaseGuiResources();

      private:
        struct ReplayEntry
        {
            javelin::protocol::RemoteActionCommand command;
            javelin::protocol::CommandReply reply;
            bool pending = false;
        };

        [[nodiscard]] javelin::protocol::CommandReply
        dispatchRemote(const javelin::protocol::CommandId& id,
                       const javelin::protocol::RemoteActionCommand& command);
        [[nodiscard]] javelin::protocol::CommandReply
        reject(const javelin::protocol::CommandId& id, QString detail,
               javelin::protocol::BoundaryErrorCode code =
                   javelin::protocol::BoundaryErrorCode::InvalidRequest) const;
        [[nodiscard]] javelin::protocol::CommandReply
        acceptImmediate(const javelin::protocol::CommandId& id, QByteArray result) const;
        [[nodiscard]] javelin::protocol::CommandReply
        acceptAsync(const javelin::protocol::CommandId& id,
                    const javelin::protocol::OperationId& operation) const;
        [[nodiscard]] QCoro::Task<RemoteUndoExecutionResult> performUndo(bool redo);
        void complete(const javelin::protocol::OperationId& operation, QByteArray result);
        void fail(const javelin::protocol::OperationId& operation, QString detail);
        void trimReplays();

        [[nodiscard]] static QString replayKey(const javelin::protocol::CommandId& id);

        DaemonServices& m_services;
        javelin::protocol::BoundaryEventSink& m_eventSink;
        std::function<std::optional<javelin::protocol::BoundaryError>()> m_reloadSettings;
        std::unordered_map<QString, ReplayEntry> m_replays;
        std::deque<QString> m_replayOrder;
        std::unordered_map<QString, std::unique_ptr<MailboxObservation>> m_mailboxObservations;
    };
} // namespace javelin::app
