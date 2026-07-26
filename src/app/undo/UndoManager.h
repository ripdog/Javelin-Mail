#pragma once

#include "app/undo/HistoryCommandExecutor.h"
#include "app/undo/HistoryRepository.h"

#include <QCoroTask>

#include <QObject>

#include <array>
#include <optional>
#include <variant>
#include <vector>

namespace javelin::app::undo
{

    class UndoManager final : public QObject
    {
        Q_OBJECT

      public:
        explicit UndoManager(HistoryRepository& repository, QObject* parent = nullptr);

        void setExecutor(HistoryDomain domain, HistoryCommandExecutor* executor);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError> load();

        [[nodiscard]] std::variant<std::optional<HistoryEntry>, javelin::jmap::cache::DatabaseError>
        prepareNormal(QString label, HistoryDomain domain, HistoryPayload payload,
                      std::optional<QString> operationGroupId,
                      std::optional<QDateTime> expiresAt = std::nullopt,
                      CommandOrigin origin = CommandOrigin::User);
        [[nodiscard]] std::variant<HistoryEntry, javelin::jmap::cache::DatabaseError>
        commitNormal(HistoryEntry entry);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        discardNormal(const QString& entryId);
        [[nodiscard]] std::variant<HistoryEntry, javelin::jmap::cache::DatabaseError>
        recordImpossible(QString label, HistoryDomain domain, QString explanation,
                         std::optional<QString> operationGroupId = std::nullopt);

        [[nodiscard]] QCoro::Task<bool> undo();
        [[nodiscard]] QCoro::Task<bool> redo();
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        acknowledgeAndRemove(const QString& entryId);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        forget(const QString& entryId);

        [[nodiscard]] const HistoryState& state() const;
        [[nodiscard]] const std::vector<HistoryEntry>& entries() const;

      Q_SIGNALS:
        void historyStateChanged(javelin::app::undo::HistoryState state);
        void executionStarted(QString entryId);
        void executionCompleted(QString entryId,
                                javelin::app::undo::HistoryRefreshScope refreshScope);
        void executionFailed(javelin::app::undo::HistoryFailure failure);
        void entryExpired(QString entryId);

      private:
        [[nodiscard]] QCoro::Task<bool> executeTop(HistoryStack stack);
        [[nodiscard]] HistoryCommandExecutor* executorFor(HistoryDomain domain) const;
        [[nodiscard]] std::optional<HistoryEntry> top(HistoryStack stack) const;
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError> reload();
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        recoverInterruptedEntries();
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError> prune();
        void publishState();
        void publishRepositoryFailure(const HistoryEntry& entry, const QString& operation,
                                      const javelin::jmap::cache::DatabaseError& error);
        void publishExecutionFailure(const HistoryEntry& entry,
                                     const HistoryExecutionResult& result,
                                     bool acknowledgeAndRemove = false);

        static constexpr std::size_t maxEntryCount = 200;
        static constexpr qsizetype maxPayloadBytes = 32 * 1024 * 1024;

        HistoryRepository& m_repository;
        std::array<HistoryCommandExecutor*, 5> m_executors{};
        std::vector<HistoryEntry> m_entries;
        HistoryState m_state;
        bool m_executing = false;
    };

} // namespace javelin::app::undo

Q_DECLARE_METATYPE(javelin::app::undo::HistoryState)
Q_DECLARE_METATYPE(javelin::app::undo::HistoryFailure)
Q_DECLARE_METATYPE(javelin::app::undo::HistoryRefreshScope)
