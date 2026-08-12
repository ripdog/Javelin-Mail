#pragma once

#include "storage/sqlite/DatabaseConnection.h"

#include "jmap/cache/CalendarRepository.h"
#include "jmap/sync/MutationJournal.h"

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::calendar
{
    enum class CalendarMutationKind
    {
        Create,
        Update,
        Destroy,
    };

    struct CalendarMutationRecord
    {
        std::string mutationId;
        std::optional<std::string> operationGroupId;
        std::string accountId;
        std::string objectId;
        std::optional<std::string> creationId;
        CalendarMutationKind kind = CalendarMutationKind::Update;
        sync::MutationStatus status = sync::MutationStatus::Pending;
        std::string requestedDocument;
        std::optional<std::string> baseDocument;
        std::optional<std::string> projectedDocument;
        std::optional<std::string> baseState;
        std::optional<std::string> acceptedState;
        std::optional<std::string> errorJson;
    };

    class CalendarMutationJournal
    {
      public:
        CalendarMutationJournal(cache::DatabaseConnection& connection,
                                cache::CalendarRepository& calendar);

        [[nodiscard]] std::optional<cache::DatabaseError>
        queue(const std::vector<CalendarMutationRecord>& records, std::string_view eventState,
              const std::vector<CalendarEvent>& projectedEvents,
              const std::vector<Occurrence>& nonRecurringOccurrences,
              std::span<const std::string> destroyedIds);
        [[nodiscard]] std::variant<std::vector<CalendarMutationRecord>, cache::DatabaseError>
        listForEvent(std::string_view accountId, std::string_view eventId) const;
        [[nodiscard]] std::variant<std::vector<CalendarMutationRecord>, cache::DatabaseError>
        listActive(std::string_view accountId) const;
        [[nodiscard]] std::optional<cache::DatabaseError>
        transition(const std::vector<CalendarMutationRecord>& records, sync::MutationStatus status,
                   std::optional<std::string_view> acceptedState = std::nullopt,
                   std::optional<std::string_view> errorJson = std::nullopt);

      private:
        cache::DatabaseConnection& m_connection;
        cache::CalendarRepository& m_calendar;
        sync::MutationJournalRepository m_journal;
    };
} // namespace javelin::jmap::calendar
