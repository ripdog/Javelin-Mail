#pragma once

#include "app/undo/HistoryTypes.h"
#include "jmap/cache/Database.h"
#include "jmap/sync/MutationJournal.h"

#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::app::undo
{

    class HistoryRepository
    {
      public:
        explicit HistoryRepository(javelin::jmap::cache::DatabaseConnection& connection);

        [[nodiscard]] std::variant<std::vector<HistoryEntry>, javelin::jmap::cache::DatabaseError>
        load() const;
        [[nodiscard]] std::variant<std::optional<HistoryEntry>, javelin::jmap::cache::DatabaseError>
        find(const QString& entryId) const;
        [[nodiscard]] std::variant<bool, javelin::jmap::cache::DatabaseError>
        hasMutationGroup(const QString& operationGroupId) const;
        [[nodiscard]] std::variant<std::vector<javelin::jmap::sync::MutationRecord>,
                                   javelin::jmap::cache::DatabaseError>
        mutationGroup(std::string_view accountId, std::string_view dataType,
                      const QString& operationGroupId) const;

        [[nodiscard]] std::variant<HistoryEntry, javelin::jmap::cache::DatabaseError>
        insertPreparing(HistoryEntry entry);
        [[nodiscard]] std::variant<HistoryEntry, javelin::jmap::cache::DatabaseError>
        pushUndoClearingRedo(HistoryEntry entry);
        [[nodiscard]] std::variant<HistoryEntry, javelin::jmap::cache::DatabaseError>
        markPreparedReady(HistoryEntry entry);
        [[nodiscard]] std::variant<HistoryEntry, javelin::jmap::cache::DatabaseError>
        markPreparedImpossible(HistoryEntry entry);
        [[nodiscard]] std::variant<HistoryEntry, javelin::jmap::cache::DatabaseError>
        move(const HistoryEntry& entry, HistoryStack destination, HistoryEntryStatus status);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        update(const HistoryEntry& entry);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        remove(const QString& entryId);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        removeAndClearRedo(const QString& entryId);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError> clearRedo();

      private:
        [[nodiscard]] std::variant<std::int64_t, javelin::jmap::cache::DatabaseError> nextOrder();
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        insert(const HistoryEntry& entry);
        [[nodiscard]] std::variant<HistoryEntry, javelin::jmap::cache::DatabaseError>
        markPrepared(HistoryEntry entry, HistoryEntryStatus status);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        updateStored(const HistoryEntry& entry);

        javelin::jmap::cache::DatabaseConnection& m_connection;
    };

} // namespace javelin::app::undo
