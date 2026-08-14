#pragma once

#include "app/undo/HistoryCommandExecutor.h"
#include "storage/DatabaseError.h"

#include <QCoroTask>

#include <QObject>

namespace javelin::app
{

    class UndoCommandPort : public QObject
    {
        Q_OBJECT

      public:
        using QObject::QObject;
        ~UndoCommandPort() override = default;

        [[nodiscard]] virtual QCoro::Task<bool> undo() = 0;
        [[nodiscard]] virtual QCoro::Task<bool> redo() = 0;
        [[nodiscard]] virtual std::optional<javelin::jmap::cache::DatabaseError>
        acknowledgeAndRemove(const QString& entryId) = 0;
        [[nodiscard]] virtual std::optional<javelin::jmap::cache::DatabaseError>
        forget(const QString& entryId) = 0;
        [[nodiscard]] virtual const undo::HistoryState& state() const = 0;
        [[nodiscard]] virtual const std::vector<undo::HistoryEntry>& entries() const = 0;

      Q_SIGNALS:
        void historyStateChanged(javelin::app::undo::HistoryState state);
        void executionCompleted(QString entryId, javelin::app::undo::HistoryRefreshScope scope);
        void executionFailed(javelin::app::undo::HistoryFailure failure);
    };

} // namespace javelin::app
