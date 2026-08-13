#pragma once

#include "app/AccountConnectionProvider.h"
#include "app/DeferredSendRepository.h"
#include "app/undo/HistoryCommandExecutor.h"
#include "jmap/OperationError.h"
#include "jmap/api/LiveConnectionSettings.h"
#include "jmap/submission/ComposeTypes.h"

#include <QCoroTask>

#include <QObject>

#include <QSet>
#include <QTimer>

#include <chrono>
#include <functional>

namespace javelin::app::undo
{
    class UndoManager;
}

namespace javelin::jmap::submission
{
    class ComposeService;
}

namespace javelin::app
{
    using DeferredSendSubmitResult =
        std::variant<javelin::jmap::submission::SendSummary, javelin::jmap::OperationError>;

    class DeferredSendSubmitter
    {
      public:
        virtual ~DeferredSendSubmitter() = default;

        [[nodiscard]] virtual QCoro::Task<DeferredSendSubmitResult>
        submit(javelin::jmap::LiveConnectionSettings settings,
               javelin::jmap::submission::PreparedSend prepared,
               std::function<void()> dispatched) = 0;
    };

    class ComposeDeferredSendSubmitter final : public DeferredSendSubmitter
    {
      public:
        explicit ComposeDeferredSendSubmitter(javelin::jmap::submission::ComposeService& service);

        [[nodiscard]] QCoro::Task<DeferredSendSubmitResult>
        submit(javelin::jmap::LiveConnectionSettings settings,
               javelin::jmap::submission::PreparedSend prepared,
               std::function<void()> dispatched) override;

      private:
        javelin::jmap::submission::ComposeService& m_service;
    };

    class DeferredSendService final : public QObject,
                                      public javelin::app::undo::HistoryCommandExecutor
    {
        Q_OBJECT

      public:
        DeferredSendService(DeferredSendRepository& repository, DeferredSendSubmitter& submitter,
                            AccountConnectionProvider& connectionProvider,
                            javelin::app::undo::UndoManager& undoManager,
                            std::function<QDateTime()> clock = {}, QObject* parent = nullptr);

        void start();
        [[nodiscard]] QCoro::Task<DeferredSendSubmitResult>
        schedule(std::string connectionId, javelin::jmap::submission::PreparedSend prepared,
                 std::chrono::seconds delay);
        [[nodiscard]] std::variant<bool, javelin::jmap::OperationError>
        cancelTargeted(const QString& sendId);
        void notificationWindowPresented(const QString& sendId);
        void notificationWindowEnded(const QString& sendId);

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

      private Q_SLOTS:
        void dispatchDue();

      private:
        void scheduleNext();
        [[nodiscard]] QCoro::Task<void> dispatch(PendingSend send);
        void clearNotificationGate(const QString& sendId);
        [[nodiscard]] QDateTime now() const;

        DeferredSendRepository& m_repository;
        DeferredSendSubmitter& m_submitter;
        AccountConnectionProvider& m_connectionProvider;
        javelin::app::undo::UndoManager& m_undoManager;
        std::function<QDateTime()> m_clock;
        QTimer m_timer;
        QSet<QString> m_notificationGatedSendIds;
        bool m_dispatchRunning = false;
    };
} // namespace javelin::app
