#pragma once

#include "app/undo/HistoryTypes.h"
#include "jmap/cache/Database.h"

#include <optional>
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

        [[nodiscard]] std::variant<HistoryEntry, javelin::jmap::cache::DatabaseError>
        insertPreparing(HistoryEntry entry);
        [[nodiscard]] std::variant<HistoryEntry, javelin::jmap::cache::DatabaseError>
        pushUndoClearingRedo(HistoryEntry entry);
        [[nodiscard]] std::variant<HistoryEntry, javelin::jmap::cache::DatabaseError>
        markPreparedReady(HistoryEntry entry);
        [[nodiscard]] std::variant<HistoryEntry, javelin::jmap::cache::DatabaseError>
        move(const HistoryEntry& entry, HistoryStack destination, HistoryEntryStatus status);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        update(const HistoryEntry& entry);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        remove(const QString& entryId);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError> clearRedo();

      private:
        [[nodiscard]] std::variant<std::int64_t, javelin::jmap::cache::DatabaseError> nextOrder();
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        insert(const HistoryEntry& entry);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        updateStored(const HistoryEntry& entry);

        javelin::jmap::cache::DatabaseConnection& m_connection;
    };

} // namespace javelin::app::undo
