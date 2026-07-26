#pragma once

#include "app/AccountConnectionProvider.h"
#include "app/DeferredSendRepository.h"
#include "app/undo/HistoryCommandExecutor.h"
#include "jmap/submission/ComposeService.h"

#include <QObject>

#include <QTimer>

#include <chrono>
#include <functional>

namespace javelin::app::undo
{
    class UndoManager;
}

namespace javelin::app
{
    class DeferredSendService final : public QObject,
                                      public javelin::app::undo::HistoryCommandExecutor
    {
        Q_OBJECT

      public:
        DeferredSendService(DeferredSendRepository& repository,
                            javelin::jmap::submission::ComposeService& composeService,
                            AccountConnectionProvider& connectionProvider,
                            javelin::app::undo::UndoManager& undoManager,
                            std::function<QDateTime()> clock = {}, QObject* parent = nullptr);

        void start();
        [[nodiscard]] QCoro::Task<
            std::variant<javelin::jmap::submission::SendSummary, javelin::jmap::OperationError>>
        schedule(std::string connectionId, javelin::jmap::submission::PreparedSend prepared,
                 std::chrono::seconds delay);
        [[nodiscard]] std::variant<bool, javelin::jmap::OperationError>
        cancelTargeted(const QString& sendId);

        [[nodiscard]] QCoro::Task<javelin::app::undo::HistoryExecutionResult>
        execute(javelin::app::undo::HistoryEntry entry,
                javelin::app::undo::HistoryExecutionDirection direction) override;

      Q_SIGNALS:
        void undoableSendScheduled(const QString& sendId, const QString& title,
                                   const QString& message, int timeoutMs);
        void undoableSendWaiting(const QString& sendId, const QString& title,
                                 const QString& message);
        void undoableSendClosed(const QString& sendId);
        void draftRestoreRequested(const QString& accountId, const QString& draftEmailId,
                                   const QString& composeSessionId);
        void sendFailed(const QString& sendId, const QString& message);

      private:
        void scheduleNext();
        void dispatchDue();
        [[nodiscard]] QCoro::Task<void> dispatch(PendingSend send);
        [[nodiscard]] QDateTime now() const;

        DeferredSendRepository& m_repository;
        javelin::jmap::submission::ComposeService& m_composeService;
        AccountConnectionProvider& m_connectionProvider;
        javelin::app::undo::UndoManager& m_undoManager;
        std::function<QDateTime()> m_clock;
        QTimer m_timer;
        bool m_dispatchRunning = false;
    };
} // namespace javelin::app
