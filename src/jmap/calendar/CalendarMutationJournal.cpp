#include "jmap/calendar/CalendarMutationJournal.h"

#include <glaze/glaze.hpp>

#include <algorithm>

namespace
{
    struct RawCalendarMutation
    {
        std::string kind;
        std::optional<std::string> creationId;
        std::string requestedDocument;
        std::optional<std::string> baseDocument;
        std::optional<std::string> projectedDocument;
    };
} // namespace

template <> struct glz::meta<RawCalendarMutation>
{
    using T = RawCalendarMutation;
    static constexpr auto value = glz::object(
        "kind", &T::kind, "creationId", &T::creationId, "requestedDocument", &T::requestedDocument,
        "baseDocument", &T::baseDocument, "projectedDocument", &T::projectedDocument);
};

namespace javelin::jmap::calendar
{
    namespace
    {
        [[nodiscard]] std::string_view kindName(const CalendarMutationKind kind)
        {
            switch (kind)
            {
            case CalendarMutationKind::Create:
                return "create";
            case CalendarMutationKind::Update:
                return "update";
            case CalendarMutationKind::Destroy:
                return "destroy";
            }
            return "update";
        }

        [[nodiscard]] std::optional<CalendarMutationKind> parseKind(const std::string_view kind)
        {
            if (kind == "create")
                return CalendarMutationKind::Create;
            if (kind == "update")
                return CalendarMutationKind::Update;
            if (kind == "destroy")
                return CalendarMutationKind::Destroy;
            return std::nullopt;
        }

        [[nodiscard]] std::variant<sync::MutationRecord, cache::DatabaseError>
        genericRecord(const CalendarMutationRecord& record)
        {
            std::string payload;
            if (glz::write_json(
                    RawCalendarMutation{
                        .kind = std::string{kindName(record.kind)},
                        .creationId = record.creationId,
                        .requestedDocument = record.requestedDocument,
                        .baseDocument = record.baseDocument,
                        .projectedDocument = record.projectedDocument,
                    },
                    payload))
            {
                return cache::DatabaseError{
                    .code = cache::DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("Unable to serialize a CalendarEvent mutation."),
                };
            }
            return sync::MutationRecord{
                .mutationId = record.mutationId,
                .operationGroupId = record.operationGroupId,
                .domain = {.accountId = record.accountId, .dataType = "CalendarEvent"},
                .objectId = record.objectId,
                .mutationKind = "calendar_event_set",
                .status = record.status,
                .payloadJson = std::move(payload),
                .baseState = record.baseState,
                .acceptedState = record.acceptedState,
                .errorJson = record.errorJson,
            };
        }

        [[nodiscard]] std::variant<CalendarMutationRecord, cache::DatabaseError>
        typedRecord(sync::MutationRecord record)
        {
            RawCalendarMutation payload;
            if (record.mutationKind != "calendar_event_set" ||
                glz::read<glz::opts{.error_on_unknown_keys = false}>(payload, record.payloadJson))
            {
                return cache::DatabaseError{
                    .code = cache::DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("Invalid CalendarEvent mutation journal payload."),
                };
            }
            const auto kind = parseKind(payload.kind);
            if (!kind.has_value())
            {
                return cache::DatabaseError{
                    .code = cache::DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("Invalid CalendarEvent mutation kind."),
                };
            }
            return CalendarMutationRecord{
                .mutationId = std::move(record.mutationId),
                .operationGroupId = std::move(record.operationGroupId),
                .accountId = std::move(record.domain.accountId),
                .objectId = std::move(record.objectId),
                .creationId = std::move(payload.creationId),
                .kind = *kind,
                .status = record.status,
                .requestedDocument = std::move(payload.requestedDocument),
                .baseDocument = std::move(payload.baseDocument),
                .projectedDocument = std::move(payload.projectedDocument),
                .baseState = std::move(record.baseState),
                .acceptedState = std::move(record.acceptedState),
                .errorJson = std::move(record.errorJson),
            };
        }
    } // namespace

    CalendarMutationJournal::CalendarMutationJournal(cache::DatabaseConnection& connection,
                                                     cache::CalendarRepository& calendar)
        : m_connection(connection), m_calendar(calendar), m_journal(connection)
    {
    }

    std::optional<cache::DatabaseError>
    CalendarMutationJournal::queue(const std::vector<CalendarMutationRecord>& records,
                                   const std::string_view eventState,
                                   const std::vector<CalendarEvent>& projectedEvents,
                                   const std::vector<Occurrence>& nonRecurringOccurrences,
                                   const std::span<const std::string> destroyedIds)
    {
        if (records.empty())
        {
            return cache::DatabaseError{
                .code = cache::DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("A CalendarEvent projection requires mutation records."),
            };
        }
        const auto& accountId = records.front().accountId;
        if (accountId.empty() ||
            std::ranges::any_of(records, [&accountId](const CalendarMutationRecord& record)
                                { return record.accountId != accountId; }))
        {
            return cache::DatabaseError{
                .code = cache::DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("CalendarEvent projections must belong to one account."),
            };
        }

        auto transactionResult = sync::MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Project CalendarEvent mutations"));
        if (const auto* error = std::get_if<cache::DatabaseError>(&transactionResult))
            return *error;
        auto transaction =
            std::get<sync::MutationProjectionTransaction>(std::move(transactionResult));
        for (const auto& record : records)
        {
            const auto generic = genericRecord(record);
            if (const auto* error = std::get_if<cache::DatabaseError>(&generic))
                return *error;
            if (const auto error = transaction.append(std::get<sync::MutationRecord>(generic)))
                return error;
        }
        // Destroying an event removes only that event's cached occurrences and window
        // memberships. Keep the owning query windows so unrelated events remain renderable while
        // the mutation is in flight.
        if (const auto error =
                m_calendar.projectEvents(transaction.cacheTransaction(), accountId, eventState,
                                         projectedEvents, nonRecurringOccurrences, destroyedIds))
            return error;
        return transaction.commit();
    }

    std::variant<std::vector<CalendarMutationRecord>, cache::DatabaseError>
    CalendarMutationJournal::listForEvent(const std::string_view accountId,
                                          const std::string_view eventId) const
    {
        auto result = m_journal.listForObject(
            {.accountId = std::string{accountId}, .dataType = "CalendarEvent"}, eventId);
        if (const auto* error = std::get_if<cache::DatabaseError>(&result))
            return *error;
        std::vector<CalendarMutationRecord> records;
        for (auto& generic : std::get<std::vector<sync::MutationRecord>>(result))
        {
            auto typed = typedRecord(std::move(generic));
            if (const auto* error = std::get_if<cache::DatabaseError>(&typed))
                return *error;
            records.push_back(std::get<CalendarMutationRecord>(std::move(typed)));
        }
        return records;
    }

    std::variant<std::vector<CalendarMutationRecord>, cache::DatabaseError>
    CalendarMutationJournal::listActive(const std::string_view accountId) const
    {
        auto result = m_journal.listActive(
            {.accountId = std::string{accountId}, .dataType = "CalendarEvent"});
        if (const auto* error = std::get_if<cache::DatabaseError>(&result))
            return *error;
        std::vector<CalendarMutationRecord> records;
        for (auto& generic : std::get<std::vector<sync::MutationRecord>>(result))
        {
            auto typed = typedRecord(std::move(generic));
            if (const auto* error = std::get_if<cache::DatabaseError>(&typed))
                return *error;
            records.push_back(std::get<CalendarMutationRecord>(std::move(typed)));
        }
        return records;
    }

    std::optional<cache::DatabaseError>
    CalendarMutationJournal::transition(const std::vector<CalendarMutationRecord>& records,
                                        const sync::MutationStatus status,
                                        const std::optional<std::string_view> acceptedState,
                                        const std::optional<std::string_view> errorJson)
    {
        if (records.empty())
            return std::nullopt;
        auto transactionResult = sync::MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Transition CalendarEvent mutations"));
        if (const auto* error = std::get_if<cache::DatabaseError>(&transactionResult))
            return *error;
        auto transaction =
            std::get<sync::MutationProjectionTransaction>(std::move(transactionResult));
        for (const auto& record : records)
        {
            if (const auto error =
                    transaction.transition(record.mutationId, status, acceptedState, errorJson))
                return error;
        }
        return transaction.commit();
    }
} // namespace javelin::jmap::calendar
