#include "jmap/calendar/CalendarService.h"

#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/MethodCaller.h"
#include "jmap/api/RequestBuilder.h"
#include "jmap/api/ResponseReader.h"
#include "jmap/auth/Auth.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/calendar/CalendarMutationJournal.h"
#include "jmap/sync/ConsistencyDomain.h"
#include "jmap/sync/MutationJournal.h"

#include <QDateTime>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QUuid>

#include <algorithm>
#include <array>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace javelin::jmap::calendar
{
    Q_LOGGING_CATEGORY(logCalendarService, "jmap.calendar")

    namespace
    {
        [[nodiscard]] OperationError error(OperationErrorCode code, QString message);

        [[nodiscard]] std::variant<javelin::jmap::sync::RefreshFence, OperationError>
        captureFence(cache::DatabaseConnection& connection, const std::string_view accountId,
                     const std::string_view dataType)
        {
            javelin::jmap::sync::ConsistencyDomainRepository repository{connection};
            const auto result = repository.captureRefresh(
                {.accountId = std::string{accountId}, .dataType = std::string{dataType}});
            if (const auto* cacheError = std::get_if<cache::DatabaseError>(&result))
                return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            return std::get<javelin::jmap::sync::RefreshFence>(result);
        }

        [[nodiscard]] std::variant<bool, OperationError>
        fenceIsCurrent(cache::DatabaseConnection& connection,
                       const javelin::jmap::sync::RefreshFence& fence)
        {
            javelin::jmap::sync::ConsistencyDomainRepository repository{connection};
            const auto result = repository.canCommitRefresh(fence);
            if (const auto* cacheError = std::get_if<cache::DatabaseError>(&result))
                return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            return std::get<bool>(result);
        }

        [[nodiscard]] std::variant<bool, OperationError>
        fenceGenerationIsCurrent(cache::DatabaseConnection& connection,
                                 const javelin::jmap::sync::RefreshFence& fence)
        {
            javelin::jmap::sync::ConsistencyDomainRepository repository{connection};
            const auto result = repository.isCurrent(fence);
            if (const auto* cacheError = std::get_if<cache::DatabaseError>(&result))
                return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            return std::get<bool>(result);
        }

        using SessionResult = std::variant<api::Session, OperationError>;

        struct SupersededRefresh
        {
        };

        using BatchedCalendarEvents =
            std::variant<api::CalendarEventGetResponse, OperationError, SupersededRefresh>;

        OperationError error(const OperationErrorCode code, QString message)
        {
            qCWarning(logCalendarService).noquote()
                << "calendar error" << QString::fromUtf8(toString(code).data()) << message;
            return {.code = code, .message = std::move(message)};
        }

        SessionResult loadSession(cache::DatabaseConnection& connection,
                                  const std::string_view ownerAccountId)
        {
            cache::SessionRepository repository{connection};
            const auto loaded = repository.load(ownerAccountId);
            if (const auto* databaseError = std::get_if<cache::DatabaseError>(&loaded))
                return error(OperationErrorCode::LocalStorageFailure, databaseError->message);
            const auto& session = std::get<std::optional<api::Session>>(loaded);
            if (!session)
                return error(OperationErrorCode::UnsupportedCapability,
                             QStringLiteral("No cached JMAP session is available."));
            if (!session->capabilities.calendars)
                return error(
                    OperationErrorCode::UnsupportedCapability,
                    QStringLiteral("The server does not advertise JMAP Calendars draft-26."));
            return *session;
        }

        api::ApiRequestContext context(const LiveConnectionSettings& settings,
                                       const api::Session& session, std::string accountId)
        {
            return {.credentials = {.accountId = std::move(accountId),
                                    .emailAddress = settings.loginEmail,
                                    .sessionUrl = settings.sessionUrl,
                                    .token = {.accessToken = settings.apiKey,
                                              .refreshToken = std::nullopt,
                                              .expiry = std::nullopt}},
                    .apiUrl = session.apiUrl};
        }

        OperationError callError(const api::MethodCallerResult& result)
        {
            if (const auto* value = std::get_if<api::TransportError>(&result))
                return operationError(*value);
            if (const auto* value = std::get_if<api::AuthError>(&result))
                return operationError(*value);
            if (const auto* value = std::get_if<api::ProtocolError>(&result))
                return operationError(*value);
            return error(OperationErrorCode::ProtocolViolation,
                         QStringLiteral("The calendar request failed."));
        }

        OperationError responseError(const api::ResponseReaderError& value)
        {
            return operationError(value);
        }

        const std::vector<std::string>& calendarEventReadProperties()
        {
            static const std::vector<std::string> properties{
                "id",           "baseEventId", "recurrenceId",     "uid",
                "calendarIds",  "title",       "description",      "locations",
                "start",        "duration",    "timeZone",         "showWithoutTime",
                "isDraft",      "isOrigin",    "useDefaultAlerts", "alerts",
                "utcStart",     "utcEnd",      "recurrenceRule",   "recurrenceOverrides",
                "participants",
            };
            return properties;
        }

        template <typename IsCurrent>
        QCoro::Task<BatchedCalendarEvents>
        getCalendarEventsBatched(api::MethodCaller& caller, const LiveConnectionSettings& settings,
                                 const api::Session& session, const std::string& accountId,
                                 const std::vector<std::string>& ids,
                                 const TimeZoneId& displayTimeZone, const std::size_t batchLimit,
                                 const std::string_view callId, IsCurrent isCurrent)
        {
            if (batchLimit == 0)
                co_return error(OperationErrorCode::UnsupportedCapability,
                                QStringLiteral("The server advertises maxObjectsInGet as zero."));

            api::CalendarEventGetResponse combined{
                .accountId = accountId, .state = {}, .list = {}, .notFound = {}};
            std::size_t offset = 0;
            bool firstBatch = true;
            do
            {
                const auto count = std::min(batchLimit, ids.size() - offset);
                std::vector<std::string> batch{ids.begin() + static_cast<std::ptrdiff_t>(offset),
                                               ids.begin() +
                                                   static_cast<std::ptrdiff_t>(offset + count)};
                const auto request =
                    api::calendarEventGet({.accountId = accountId,
                                           .ids = std::move(batch),
                                           .idsReference = std::nullopt,
                                           .properties = calendarEventReadProperties(),
                                           .recurrenceOverridesBefore = std::nullopt,
                                           .recurrenceOverridesAfter = std::nullopt,
                                           .reduceParticipants = false,
                                           .timeZone = displayTimeZone});
                if (!request)
                    co_return error(OperationErrorCode::InvalidRequest,
                                    QStringLiteral("Unable to serialize the calendar event "
                                                   "request."));
                api::RequestBuilder builder;
                builder.useCore().useCapability(std::string{api::calendarsCapabilityUri});
                const auto handle = builder.call(*request, std::string{callId});
                const auto result =
                    co_await caller.call(context(settings, session, accountId), builder);
                if (!isCurrent())
                    co_return SupersededRefresh{};
                const auto* envelope = std::get_if<api::ResponseEnvelope>(&result);
                if (!envelope)
                    co_return callError(result);
                const auto read = api::ResponseReader{*envelope}.require(handle);
                if (const auto* readError = std::get_if<api::ResponseReaderError>(&read))
                    co_return responseError(*readError);
                auto response = std::get<api::CalendarEventGetResponse>(read);
                if (!firstBatch && response.state != combined.state)
                    co_return error(
                        OperationErrorCode::ProtocolViolation,
                        QStringLiteral("Calendar event batches returned inconsistent states."));
                if (firstBatch)
                    combined.state = response.state;
                combined.list.insert(combined.list.end(),
                                     std::make_move_iterator(response.list.begin()),
                                     std::make_move_iterator(response.list.end()));
                combined.notFound.insert(combined.notFound.end(),
                                         std::make_move_iterator(response.notFound.begin()),
                                         std::make_move_iterator(response.notFound.end()));
                firstBatch = false;
                offset += count;
            } while (offset < ids.size());
            co_return combined;
        }

        bool writable(const std::vector<calendar::Calendar>& calendars,
                      const std::vector<std::string>& calendarIds)
        {
            std::unordered_set<std::string> required{calendarIds.begin(), calendarIds.end()};
            for (const auto& item : calendars)
            {
                if (!required.erase(item.id))
                    continue;
                if (!item.myRights.mayWriteAll && !item.myRights.mayWriteOwn)
                    return false;
            }
            return required.empty();
        }

        std::variant<std::string, OperationError>
        currentEventState(cache::DatabaseConnection& connection, const std::string_view accountId)
        {
            cache::CalendarRepository repository{connection};
            const auto loaded = repository.stateToken(accountId, "CalendarEvent");
            if (const auto* databaseError = std::get_if<cache::DatabaseError>(&loaded))
                return error(OperationErrorCode::LocalStorageFailure, databaseError->message);
            const auto& state = std::get<std::optional<std::string>>(loaded);
            if (!state)
                return error(OperationErrorCode::InvalidRequest,
                             QStringLiteral("Refresh the calendar before modifying an event."));
            return *state;
        }

        LocalDateTime localEnd(const CalendarEvent& event)
        {
            const auto start =
                QDateTime::fromString(QString::fromStdString(event.start.value), Qt::ISODate);
            static const QRegularExpression durationPattern{QStringLiteral(
                R"(^P(?:(\d+)D)?(?:T(?:(\d+)H)?(?:(\d+)M)?(?:(\d+(?:\.\d+)?)S)?)?$)")};
            const auto match = durationPattern.match(QString::fromStdString(event.duration.value));
            if (!start.isValid() || !match.hasMatch())
                return event.start;
            const auto seconds =
                match.captured(1).toLongLong() * 86400 + match.captured(2).toLongLong() * 3600 +
                match.captured(3).toLongLong() * 60 + qRound64(match.captured(4).toDouble());
            return {.value = start.addSecs(seconds).toString(Qt::ISODate).toStdString()};
        }

        [[nodiscard]] Occurrence projectedOccurrence(const CalendarEvent& event)
        {
            return Occurrence{
                .accountId = event.accountId,
                .id = event.id,
                .eventId = event.id,
                .recurrenceId = std::nullopt,
                .localStart = event.start,
                .localEnd = localEnd(event),
                .utcStart = event.utcStart,
                .utcEnd = event.utcEnd,
                .allDay = event.showWithoutTime,
            };
        }

        struct PreparedCalendarMutations
        {
            std::vector<CalendarMutationRecord> records;
            std::vector<CalendarEvent> projectedEvents;
            std::vector<Occurrence> projectedOccurrences;
            std::vector<std::string> destroyedIds;
        };

        [[nodiscard]] std::variant<PreparedCalendarMutations, OperationError>
        prepareCalendarMutations(cache::CalendarRepository& repository,
                                 const api::CalendarEventSetRequest& request,
                                 const std::optional<std::string>& operationGroupId)
        {
            PreparedCalendarMutations prepared;
            prepared.records.reserve(request.create.size() + request.update.size() +
                                     request.destroy.size());
            for (const auto& [creationId, requested] : request.create)
            {
                auto projected = requested;
                const auto mutationId =
                    QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
                projected.accountId = request.accountId;
                projected.id = "local-" + mutationId;
                const auto requestedDocument = api::serializeCalendarEventDocument(requested);
                const auto projectedDocument = api::serializeCalendarEventDocument(projected);
                if (!requestedDocument.has_value() || !projectedDocument.has_value())
                    return error(OperationErrorCode::InvalidRequest,
                                 QStringLiteral("Unable to serialize the calendar change."));
                prepared.records.push_back({
                    .mutationId = mutationId,
                    .operationGroupId = operationGroupId,
                    .accountId = request.accountId,
                    .objectId = projected.id,
                    .creationId = creationId,
                    .kind = CalendarMutationKind::Create,
                    .status = javelin::jmap::sync::MutationStatus::Pending,
                    .requestedDocument = *requestedDocument,
                    .baseDocument = std::nullopt,
                    .projectedDocument = *projectedDocument,
                    .baseState = request.ifInState,
                    .acceptedState = std::nullopt,
                    .errorJson = std::nullopt,
                });
                prepared.projectedOccurrences.push_back(projectedOccurrence(projected));
                prepared.projectedEvents.push_back(std::move(projected));
            }
            for (const auto& [eventId, requested] : request.update)
            {
                auto projected = requested.event;
                projected.accountId = request.accountId;
                projected.id = eventId;
                const auto cached = repository.findEvent(request.accountId, eventId);
                if (const auto* cacheError = std::get_if<cache::DatabaseError>(&cached))
                    return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
                const auto& base = std::get<std::optional<CalendarEvent>>(cached);
                const auto requestedDocument = api::serializeCalendarEventDocument(projected);
                if (!requestedDocument.has_value())
                    return error(OperationErrorCode::InvalidRequest,
                                 QStringLiteral("Unable to serialize the calendar change."));
                prepared.records.push_back({
                    .mutationId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),
                    .operationGroupId = operationGroupId,
                    .accountId = request.accountId,
                    .objectId = eventId,
                    .creationId = std::nullopt,
                    .kind = CalendarMutationKind::Update,
                    .status = javelin::jmap::sync::MutationStatus::Pending,
                    .requestedDocument = *requestedDocument,
                    .baseDocument = base.has_value() ? api::serializeCalendarEventDocument(*base)
                                                     : std::nullopt,
                    .projectedDocument = *requestedDocument,
                    .baseState = request.ifInState,
                    .acceptedState = std::nullopt,
                    .errorJson = std::nullopt,
                });
                prepared.projectedOccurrences.push_back(projectedOccurrence(projected));
                prepared.projectedEvents.push_back(std::move(projected));
            }
            for (const auto& eventId : request.destroy)
            {
                const auto cached = repository.findEvent(request.accountId, eventId);
                if (const auto* cacheError = std::get_if<cache::DatabaseError>(&cached))
                    return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
                const auto& base = std::get<std::optional<CalendarEvent>>(cached);
                prepared.records.push_back({
                    .mutationId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),
                    .operationGroupId = operationGroupId,
                    .accountId = request.accountId,
                    .objectId = eventId,
                    .creationId = std::nullopt,
                    .kind = CalendarMutationKind::Destroy,
                    .status = javelin::jmap::sync::MutationStatus::Pending,
                    .requestedDocument = "{}",
                    .baseDocument = base.has_value() ? api::serializeCalendarEventDocument(*base)
                                                     : std::nullopt,
                    .projectedDocument = std::nullopt,
                    .baseState = request.ifInState,
                    .acceptedState = std::nullopt,
                    .errorJson = std::nullopt,
                });
                if (base.has_value())
                    prepared.destroyedIds.push_back(eventId);
            }
            return prepared;
        }

        [[nodiscard]] std::variant<CalendarEvent, OperationError>
        eventFromMutationDocument(const CalendarMutationRecord& record,
                                  const std::string_view document, const std::string_view eventId)
        {
            const auto parsed = api::parseCalendarEventDocument(record.accountId, document);
            if (!parsed.ok() || !parsed.value.has_value())
                return error(OperationErrorCode::LocalStorageFailure,
                             QStringLiteral("A CalendarEvent mutation document is invalid."));
            auto event = *parsed.value;
            event.accountId = record.accountId;
            event.id = std::string{eventId};
            return event;
        }

        struct RebasedCalendarEvents
        {
            std::vector<CalendarEvent> events;
            std::vector<const CalendarMutationRecord*> acceptedUnknown;
            std::unordered_set<std::string> suppressedExpansionIds;
            std::unordered_set<std::string> suppressedExpansionUids;
        };

        [[nodiscard]] std::variant<RebasedCalendarEvents, OperationError>
        rebaseCalendarEvents(std::vector<CalendarEvent> serverEvents,
                             const std::vector<CalendarMutationRecord>& mutations)
        {
            RebasedCalendarEvents result{
                .events = std::move(serverEvents),
                .acceptedUnknown = {},
                .suppressedExpansionIds = {},
                .suppressedExpansionUids = {},
            };
            const auto serverSnapshot = result.events;
            const auto sameDocument = [](CalendarEvent left, CalendarEvent right)
            {
                left.id = right.id;
                const auto leftDocument = api::serializeCalendarEventDocument(left);
                const auto rightDocument = api::serializeCalendarEventDocument(right);
                return leftDocument.has_value() && leftDocument == rightDocument;
            };
            for (const auto& mutation : mutations)
            {
                const auto server =
                    std::ranges::find(serverSnapshot, mutation.objectId, &CalendarEvent::id);
                if (mutation.kind == CalendarMutationKind::Destroy)
                {
                    if (mutation.status == javelin::jmap::sync::MutationStatus::Unknown &&
                        server == serverSnapshot.end())
                        result.acceptedUnknown.push_back(&mutation);
                    if (server != serverSnapshot.end())
                    {
                        result.suppressedExpansionIds.insert(mutation.objectId);
                        if (!server->uid.empty())
                            result.suppressedExpansionUids.insert(server->uid);
                    }
                    std::erase_if(result.events, [&mutation](const CalendarEvent& event)
                                  { return event.id == mutation.objectId; });
                    continue;
                }
                if (!mutation.projectedDocument.has_value())
                    continue;
                auto projected = eventFromMutationDocument(mutation, *mutation.projectedDocument,
                                                           mutation.objectId);
                if (const auto* operationError = std::get_if<OperationError>(&projected))
                    return *operationError;
                auto visible = std::get<CalendarEvent>(std::move(projected));
                bool confirmed = false;
                if (mutation.kind == CalendarMutationKind::Create)
                {
                    const auto matched =
                        std::ranges::find(serverSnapshot, visible.uid, &CalendarEvent::uid);
                    if (matched != serverSnapshot.end() && sameDocument(visible, *matched))
                    {
                        visible = *matched;
                        confirmed = true;
                    }
                }
                else if (server != serverSnapshot.end())
                    confirmed = sameDocument(visible, *server);
                if (mutation.status == javelin::jmap::sync::MutationStatus::Unknown && confirmed)
                    result.acceptedUnknown.push_back(&mutation);
                if (!confirmed)
                {
                    result.suppressedExpansionIds.insert(mutation.objectId);
                    if (!visible.uid.empty())
                        result.suppressedExpansionUids.insert(visible.uid);
                }
                std::erase_if(result.events, [&mutation](const CalendarEvent& event)
                              { return event.id == mutation.objectId; });
                result.events.push_back(std::move(visible));
            }
            return result;
        }

        [[nodiscard]] std::optional<OperationError> restoreCalendarMutations(
            cache::DatabaseConnection& connection, cache::CalendarRepository& repository,
            const std::vector<CalendarMutationRecord>& records, const std::string_view eventState,
            const std::optional<std::string_view> errorJson)
        {
            auto transactionResult = javelin::jmap::sync::MutationProjectionTransaction::begin(
                connection, QStringLiteral("Reject CalendarEvent mutations"));
            if (const auto* cacheError = std::get_if<cache::DatabaseError>(&transactionResult))
                return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            auto transaction = std::get<javelin::jmap::sync::MutationProjectionTransaction>(
                std::move(transactionResult));
            std::vector<CalendarEvent> restoredEvents;
            std::vector<Occurrence> restoredOccurrences;
            std::vector<std::string> removedIds;
            for (const auto& record : records)
            {
                if (const auto cacheError = transaction.transition(
                        record.mutationId, javelin::jmap::sync::MutationStatus::Rejected,
                        std::nullopt, errorJson))
                    return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
                if (record.kind == CalendarMutationKind::Create)
                {
                    removedIds.push_back(record.objectId);
                    continue;
                }
                if (!record.baseDocument.has_value())
                {
                    if (record.kind == CalendarMutationKind::Update)
                        removedIds.push_back(record.objectId);
                    continue;
                }
                auto restored =
                    eventFromMutationDocument(record, *record.baseDocument, record.objectId);
                if (const auto* operationError = std::get_if<OperationError>(&restored))
                    return *operationError;
                auto event = std::get<CalendarEvent>(std::move(restored));
                restoredOccurrences.push_back(projectedOccurrence(event));
                restoredEvents.push_back(std::move(event));
            }
            if (const auto cacheError = repository.projectEvents(
                    transaction.cacheTransaction(), records.front().accountId, eventState,
                    restoredEvents, restoredOccurrences, removedIds))
                return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            if (const auto cacheError = transaction.commit())
                return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            return std::nullopt;
        }

        [[nodiscard]] CalendarMutationResult
        acceptCalendarMutations(cache::DatabaseConnection& connection,
                                cache::CalendarRepository& repository,
                                const std::vector<CalendarMutationRecord>& records,
                                const api::CalendarEventSetResponse& response)
        {
            std::vector<CalendarEvent> acceptedEvents;
            std::vector<Occurrence> acceptedOccurrences;
            std::vector<std::string> removedTemporaryIds;
            for (const auto& record : records)
            {
                if (record.kind == CalendarMutationKind::Update)
                {
                    if (!response.updated.contains(record.objectId))
                        return error(
                            OperationErrorCode::ProtocolViolation,
                            QStringLiteral(
                                "The CalendarEvent/set response omitted an updated event."));
                    continue;
                }
                if (record.kind == CalendarMutationKind::Destroy)
                {
                    if (std::ranges::find(response.destroyed, record.objectId) ==
                        response.destroyed.end())
                        return error(
                            OperationErrorCode::ProtocolViolation,
                            QStringLiteral(
                                "The CalendarEvent/set response omitted a destroyed event."));
                    continue;
                }
                if (!record.creationId.has_value())
                    return error(OperationErrorCode::ProtocolViolation,
                                 QStringLiteral("A CalendarEvent creation lost its creation id."));
                const auto created = response.created.find(*record.creationId);
                if (created == response.created.end() || !created->second.id.has_value())
                    return error(
                        OperationErrorCode::ProtocolViolation,
                        QStringLiteral("The CalendarEvent/set response omitted a created id."));
                if (!record.projectedDocument.has_value())
                    return error(OperationErrorCode::LocalStorageFailure,
                                 QStringLiteral("A CalendarEvent creation lost its projection."));
                auto accepted = eventFromMutationDocument(record, *record.projectedDocument,
                                                          *created->second.id);
                if (const auto* operationError = std::get_if<OperationError>(&accepted))
                    return *operationError;
                auto event = std::get<CalendarEvent>(std::move(accepted));
                acceptedOccurrences.push_back(projectedOccurrence(event));
                acceptedEvents.push_back(std::move(event));
                removedTemporaryIds.push_back(record.objectId);
            }

            auto transactionResult = javelin::jmap::sync::MutationProjectionTransaction::begin(
                connection, QStringLiteral("Accept CalendarEvent mutations"));
            if (const auto* cacheError = std::get_if<cache::DatabaseError>(&transactionResult))
                return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            auto transaction = std::get<javelin::jmap::sync::MutationProjectionTransaction>(
                std::move(transactionResult));
            for (const auto& record : records)
            {
                if (const auto cacheError = transaction.transition(
                        record.mutationId, javelin::jmap::sync::MutationStatus::Accepted,
                        response.newState))
                    return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            }
            const std::array domains{javelin::jmap::sync::ConsistencyDomain{
                .accountId = response.accountId,
                .dataType = "CalendarEvent",
            }};
            if (const auto cacheError = transaction.advance(domains))
                return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            if (const auto cacheError = repository.projectEvents(
                    transaction.cacheTransaction(), response.accountId, response.newState,
                    acceptedEvents, acceptedOccurrences, removedTemporaryIds))
                return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            for (const auto& record : records)
            {
                if (const auto cacheError = transaction.remove(record.mutationId))
                    return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            }
            if (const auto cacheError = transaction.commit())
                return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            return CommittedMutation{
                .accountId = response.accountId,
                .newState = response.newState,
                .createdId =
                    response.created.empty() ? std::nullopt : response.created.begin()->second.id,
                .receipt =
                    {
                        .domains =
                            {
                                {
                                    .accountId = response.accountId,
                                    .dataType = "CalendarEvent",
                                    .oldState = response.oldState,
                                    .newState = response.newState,
                                },
                            },
                        .acceptedObjectIds =
                            [&]
                        {
                            std::vector<std::string> ids;
                            ids.reserve(response.updated.size() + response.destroyed.size() +
                                        response.created.size());
                            for (const auto& [id, updated] : response.updated)
                            {
                                static_cast<void>(updated);
                                ids.push_back(id);
                            }
                            ids.insert(ids.end(), response.destroyed.begin(),
                                       response.destroyed.end());
                            for (const auto& [creationId, created] : response.created)
                            {
                                static_cast<void>(creationId);
                                if (created.id.has_value())
                                    ids.push_back(*created.id);
                            }
                            return ids;
                        }(),
                        .rejectedObjectIds = {},
                        .affectedCacheViews = {"calendar"},
                        .incompleteMaterialization = false,
                    },
            };
        }
    } // namespace

    CalendarService::CalendarService(cache::DatabaseConnection& connection,
                                     api::JmapMethodTransport& methodTransport)
        : m_connection(connection), m_methodTransport(methodTransport)
    {
    }

    CalendarLoadResult CalendarService::loadCached(const std::string_view accountId,
                                                   const VisibleInterval& interval,
                                                   const TimeZoneId& displayTimeZone) const
    {
        cache::CalendarRepository repository{m_connection};
        auto loaded =
            repository.loadWindow(accountId, interval.start, interval.end, displayTimeZone);
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&loaded))
            return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        return std::get<std::optional<cache::CalendarWindow>>(std::move(loaded));
    }

    CalendarAccountsResult CalendarService::accounts() const
    {
        cache::CalendarRepository repository{m_connection};
        auto loaded = repository.listAccounts();
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&loaded))
            return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        return std::get<std::vector<cache::CalendarAccount>>(std::move(loaded));
    }

    CalendarListResult CalendarService::calendars(const std::string_view accountId) const
    {
        cache::CalendarRepository repository{m_connection};
        auto loaded = repository.listCalendars(accountId);
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&loaded))
            return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        return std::get<std::vector<Calendar>>(std::move(loaded));
    }

    CalendarPreferenceResult CalendarService::setCalendarVisible(const std::string_view accountId,
                                                                 const std::string_view calendarId,
                                                                 const bool visible)
    {
        cache::CalendarRepository repository{m_connection};
        if (const auto cacheError = repository.setCalendarVisible(accountId, calendarId, visible))
            return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        return std::monostate{};
    }

    QCoro::Task<AuthoritativeCalendarEventResult>
    CalendarService::getAuthoritativeEvent(LiveConnectionSettings settings,
                                           std::string ownerAccountId, std::string accountId,
                                           std::optional<std::string> eventId, std::string uid)
    {
        const auto sessionResult = loadSession(m_connection, ownerAccountId);
        if (const auto* serviceError = std::get_if<OperationError>(&sessionResult))
            co_return *serviceError;
        const auto& session = std::get<api::Session>(sessionResult);
        const auto account = session.accounts.find(accountId);
        if (account == session.accounts.end() || !account->second.accountCapabilities.calendars)
            co_return error(
                OperationErrorCode::UnsupportedCapability,
                QStringLiteral("This account does not support JMAP Calendars draft-26."));
        api::MethodCaller caller{m_methodTransport};
        std::vector<std::string> eventIds;
        if (eventId.has_value())
            eventIds.push_back(*eventId);
        else
        {
            const auto query = api::calendarEventQuery({
                .accountId = accountId,
                .filter =
                    {
                        .inCalendar = std::nullopt,
                        .after = std::nullopt,
                        .before = std::nullopt,
                        .text = std::nullopt,
                        .uid = std::move(uid),
                    },
                .expandRecurrences = false,
                .timeZone = {.value = "Etc/UTC"},
                .position = 0,
                .limit = 2,
                .calculateTotal = true,
            });
            if (!query)
                co_return error(OperationErrorCode::InvalidRequest,
                                QStringLiteral("Unable to serialize the calendar history query."));
            api::RequestBuilder builder;
            builder.useCore().useCapability(std::string{api::calendarsCapabilityUri});
            const auto handle = builder.call(*query, "calendar-history-query");
            const auto result =
                co_await caller.call(context(settings, session, accountId), builder);
            const auto* envelope = std::get_if<api::ResponseEnvelope>(&result);
            if (!envelope)
                co_return callError(result);
            const auto read = api::ResponseReader{*envelope}.require(handle);
            if (const auto* readError = std::get_if<api::ResponseReaderError>(&read))
                co_return responseError(*readError);
            eventIds = std::get<api::CalendarEventQueryResponse>(read).ids;
            if (eventIds.size() > 1)
                co_return error(
                    OperationErrorCode::Conflict,
                    QStringLiteral("Multiple calendar events have the history event UID."));
        }

        const auto batchLimit =
            session.capabilities.coreDetails && session.capabilities.coreDetails->maxObjectsInGet
                ? static_cast<std::size_t>(*session.capabilities.coreDetails->maxObjectsInGet)
                : std::numeric_limits<std::size_t>::max();
        auto result = co_await getCalendarEventsBatched(
            caller, settings, session, accountId, eventIds, TimeZoneId{.value = "Etc/UTC"},
            batchLimit, "calendar-history-get", [] { return true; });
        if (const auto* serviceError = std::get_if<OperationError>(&result))
            co_return *serviceError;
        if (std::holds_alternative<SupersededRefresh>(result))
            co_return error(OperationErrorCode::Conflict,
                            QStringLiteral("The calendar event read was superseded."));
        auto response = std::get<api::CalendarEventGetResponse>(std::move(result));
        co_return AuthoritativeCalendarEvent{
            .state = std::move(response.state),
            .event = response.list.empty()
                         ? std::nullopt
                         : std::optional<CalendarEvent>{std::move(response.list.front())},
        };
    }

    QCoro::Task<CalendarMutationResult>
    CalendarService::setDefaultCalendar(LiveConnectionSettings settings, std::string ownerAccountId,
                                        std::string accountId, std::string calendarId)
    {
        const auto listed = calendars(accountId);
        if (const auto* serviceError = std::get_if<OperationError>(&listed))
            co_return *serviceError;
        const auto& available = std::get<std::vector<Calendar>>(listed);
        const auto selected = std::ranges::find(available, calendarId, &Calendar::id);
        if (selected == available.end())
            co_return error(OperationErrorCode::InvalidRequest,
                            QStringLiteral("The selected calendar is no longer available."));
        if (!selected->myRights.mayWriteAll && !selected->myRights.mayWriteOwn)
            co_return error(OperationErrorCode::PermissionDenied,
                            QStringLiteral("The selected calendar is read-only."));

        const auto sessionResult = loadSession(m_connection, ownerAccountId);
        if (const auto* serviceError = std::get_if<OperationError>(&sessionResult))
            co_return *serviceError;
        const auto& session = std::get<api::Session>(sessionResult);
        const auto account = session.accounts.find(accountId);
        if (account == session.accounts.end() || !account->second.accountCapabilities.calendars)
            co_return error(OperationErrorCode::UnsupportedCapability,
                            QStringLiteral("This account does not support JMAP Calendars."));

        cache::CalendarRepository repository{m_connection};
        const auto stateResult = repository.stateToken(accountId, "Calendar");
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&stateResult))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        const auto& state = std::get<std::optional<std::string>>(stateResult);
        if (!state.has_value())
            co_return error(OperationErrorCode::InvalidRequest,
                            QStringLiteral("Refresh calendars before changing the default."));
        sync::MutationJournalRepository journal{m_connection};
        const sync::ConsistencyDomain domain{.accountId = accountId, .dataType = "Calendar"};
        const auto active = journal.listActive(domain);
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&active))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        if (!std::get<std::vector<sync::MutationRecord>>(active).empty())
            co_return error(OperationErrorCode::Conflict,
                            QStringLiteral("A default-calendar change is already unresolved."));
        if (selected->isDefault)
            co_return CommittedMutation{
                .accountId = std::move(accountId),
                .newState = *state,
                .createdId = std::nullopt,
                .receipt = {},
            };

        const auto method = api::calendarSet(
            {.accountId = accountId, .ifInState = state, .onSuccessSetIsDefault = calendarId});
        if (!method)
            co_return error(OperationErrorCode::InvalidRequest,
                            QStringLiteral("Unable to serialize the default calendar change."));

        const sync::MutationRecord mutation{
            .mutationId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),
            .operationGroupId = std::nullopt,
            .domain = domain,
            .objectId = calendarId,
            .mutationKind = "calendar_set_default",
            .status = sync::MutationStatus::Pending,
            .payloadJson = "{}",
            .baseState = *state,
            .acceptedState = std::nullopt,
            .errorJson = std::nullopt,
        };
        std::unordered_map<std::string, bool> baseDefaults;
        std::unordered_map<std::string, bool> projectedDefaults;
        for (const auto& calendar : available)
        {
            baseDefaults.emplace(calendar.id, calendar.isDefault);
            projectedDefaults.emplace(calendar.id, calendar.id == calendarId);
        }
        auto queueResult = sync::MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Queue default Calendar mutation"));
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&queueResult))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        auto queue = std::get<sync::MutationProjectionTransaction>(std::move(queueResult));
        if (const auto cacheError = queue.append(mutation))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        const std::array domains{domain};
        if (const auto cacheError = queue.advance(domains))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        if (const auto cacheError = repository.applyCalendarDefaults(
                queue.cacheTransaction(), accountId, *state, projectedDefaults))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        if (const auto cacheError = queue.commit())
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);

        const auto transition = [&](const sync::MutationStatus status,
                                    const std::optional<std::string_view> acceptedState =
                                        std::nullopt) -> std::optional<OperationError>
        {
            auto result = sync::MutationProjectionTransaction::begin(
                m_connection, QStringLiteral("Transition default Calendar mutation"));
            if (const auto* cacheError = std::get_if<cache::DatabaseError>(&result))
                return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            auto transaction = std::get<sync::MutationProjectionTransaction>(std::move(result));
            if (const auto cacheError =
                    transaction.transition(mutation.mutationId, status, acceptedState))
                return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            if (status == sync::MutationStatus::Rejected)
            {
                if (const auto cacheError = repository.applyCalendarDefaults(
                        transaction.cacheTransaction(), accountId, *state, baseDefaults))
                    return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            }
            if (const auto cacheError = transaction.commit())
                return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            return std::nullopt;
        };
        if (const auto transitionError = transition(sync::MutationStatus::InFlight))
            co_return *transitionError;

        api::RequestBuilder builder;
        builder.useCore().useCapability(std::string{api::calendarsCapabilityUri});
        const auto handle = builder.call(*method, "calendar-set-default");
        api::MethodCaller caller{m_methodTransport};
        const auto result = co_await caller.call(context(settings, session, accountId), builder);
        const auto* envelope = std::get_if<api::ResponseEnvelope>(&result);
        if (!envelope)
        {
            if (const auto transitionError = transition(sync::MutationStatus::Unknown))
                co_return *transitionError;
            co_return callError(result);
        }
        const auto read = api::ResponseReader{*envelope}.require(handle);
        auto defaults = projectedDefaults;
        std::string acceptedState;
        bool accepted = false;
        if (const auto* readError = std::get_if<api::ResponseReaderError>(&read))
        {
            if (readError->methodError.has_value())
            {
                if (const auto transitionError = transition(sync::MutationStatus::Rejected))
                    co_return *transitionError;
                co_return responseError(*readError);
            }
            qCWarning(logCalendarService).noquote()
                << "Calendar/set response did not prove the default; verify with Calendar/get"
                << QString::fromStdString(accountId) << QString::fromStdString(calendarId);
        }
        else
        {
            const auto& response = std::get<api::CalendarSetResponse>(read);
            const auto selectedUpdate = response.updated.find(calendarId);
            accepted = selectedUpdate != response.updated.end() &&
                       selectedUpdate->second.isDefault.value_or(false);
            if (accepted)
            {
                acceptedState = response.newState;
                for (const auto& [id, update] : response.updated)
                    if (update.isDefault.has_value())
                        defaults.emplace(id, *update.isDefault);
            }
        }
        if (!accepted)
        {
            const auto verification = api::calendarGet({.accountId = accountId,
                                                        .ids = std::vector{calendarId},
                                                        .idsReference = std::nullopt,
                                                        .properties = std::nullopt});
            if (!verification)
            {
                if (const auto transitionError = transition(sync::MutationStatus::Unknown))
                    co_return *transitionError;
                co_return error(OperationErrorCode::InvalidRequest,
                                QStringLiteral("Unable to verify the default calendar change."));
            }
            api::RequestBuilder verifyBuilder;
            verifyBuilder.useCore().useCapability(std::string{api::calendarsCapabilityUri});
            const auto verifyHandle =
                verifyBuilder.call(*verification, "calendar-get-default-verification");
            const auto verifyResult =
                co_await caller.call(context(settings, session, accountId), verifyBuilder);
            const auto* verifyEnvelope = std::get_if<api::ResponseEnvelope>(&verifyResult);
            if (!verifyEnvelope)
            {
                if (const auto transitionError = transition(sync::MutationStatus::Unknown))
                    co_return *transitionError;
                co_return callError(verifyResult);
            }
            const auto verifyRead = api::ResponseReader{*verifyEnvelope}.require(verifyHandle);
            if (const auto* readError = std::get_if<api::ResponseReaderError>(&verifyRead))
            {
                if (const auto transitionError = transition(sync::MutationStatus::Unknown))
                    co_return *transitionError;
                co_return responseError(*readError);
            }
            const auto& verified = std::get<api::CalendarGetResponse>(verifyRead);
            acceptedState = verified.state;
            for (const auto& calendar : verified.list)
                defaults.emplace(calendar.id, calendar.isDefault);
            const auto selectedCalendar =
                std::ranges::find(verified.list, calendarId, &Calendar::id);
            accepted = selectedCalendar != verified.list.end() && selectedCalendar->isDefault;
        }
        if (!accepted)
        {
            if (const auto transitionError = transition(sync::MutationStatus::Rejected))
                co_return *transitionError;
            co_return error(
                OperationErrorCode::PermissionDenied,
                QStringLiteral("The server did not allow changing the default calendar."));
        }

        auto acceptResult = sync::MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Accept default Calendar mutation"));
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&acceptResult))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        auto accept = std::get<sync::MutationProjectionTransaction>(std::move(acceptResult));
        if (const auto cacheError = accept.transition(
                mutation.mutationId, sync::MutationStatus::Accepted, acceptedState))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        if (const auto cacheError = accept.advance(domains))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        if (const auto cacheError = repository.applyCalendarDefaults(
                accept.cacheTransaction(), accountId, acceptedState, defaults))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        if (const auto cacheError = accept.remove(mutation.mutationId))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        if (const auto cacheError = accept.commit())
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        co_return CommittedMutation{
            .accountId = std::move(accountId),
            .newState = std::move(acceptedState),
            .createdId = std::nullopt,
            .receipt = {},
        };
    }

    std::uint64_t CalendarService::beginRefresh(const std::string_view ownerAccountId)
    {
        return ++m_refreshGenerations[std::string{ownerAccountId}];
    }

    bool CalendarService::isCurrentRefresh(const std::string_view ownerAccountId,
                                           const std::uint64_t generation) const
    {
        const auto current = m_refreshGenerations.find(std::string{ownerAccountId});
        return current != m_refreshGenerations.end() && current->second == generation;
    }

    QCoro::Task<CalendarRefreshResult>
    CalendarService::refreshChanged(LiveConnectionSettings settings, std::string ownerAccountId,
                                    VisibleInterval interval, TimeZoneId displayTimeZone)
    {
        const auto generation = beginRefresh(ownerAccountId);
        const auto sessionResult = loadSession(m_connection, ownerAccountId);
        if (const auto* serviceError = std::get_if<OperationError>(&sessionResult))
            co_return *serviceError;
        const auto& session = std::get<api::Session>(sessionResult);
        cache::CalendarRepository repository{m_connection};
        api::MethodCaller caller{m_methodTransport};
        RefreshedRange summary{.interval = interval,
                               .displayTimeZone = displayTimeZone,
                               .accountCount = 0,
                               .eventCount = 0};
        bool fullRefreshRequired = false;
        for (const auto& [accountId, account] : session.accounts)
        {
            if (!account.accountCapabilities.calendars)
                continue;
            sync::MutationJournalRepository genericJournal{m_connection};
            const auto activeCalendarMutations =
                genericJournal.listActive({.accountId = accountId, .dataType = "Calendar"});
            if (const auto* cacheError =
                    std::get_if<cache::DatabaseError>(&activeCalendarMutations))
                co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            if (!std::get<std::vector<sync::MutationRecord>>(activeCalendarMutations).empty())
            {
                fullRefreshRequired = true;
                break;
            }
            CalendarMutationJournal mutationJournal{m_connection, repository};
            const auto activeMutations = mutationJournal.listActive(accountId);
            if (const auto* cacheError = std::get_if<cache::DatabaseError>(&activeMutations))
                co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            if (!std::get<std::vector<CalendarMutationRecord>>(activeMutations).empty())
            {
                fullRefreshRequired = true;
                break;
            }
            const auto calendarFenceResult = captureFence(m_connection, accountId, "Calendar");
            const auto eventFenceResult = captureFence(m_connection, accountId, "CalendarEvent");
            if (const auto* serviceError = std::get_if<OperationError>(&calendarFenceResult))
                co_return *serviceError;
            if (const auto* serviceError = std::get_if<OperationError>(&eventFenceResult))
                co_return *serviceError;
            const auto calendarFence =
                std::get<javelin::jmap::sync::RefreshFence>(calendarFenceResult);
            const auto eventFence = std::get<javelin::jmap::sync::RefreshFence>(eventFenceResult);
            const auto calendarStateResult = repository.stateToken(accountId, "Calendar");
            const auto eventStateResult = repository.stateToken(accountId, "CalendarEvent");
            if (const auto* cacheError = std::get_if<cache::DatabaseError>(&calendarStateResult))
                co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            if (const auto* cacheError = std::get_if<cache::DatabaseError>(&eventStateResult))
                co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            const auto& calendarState = std::get<std::optional<std::string>>(calendarStateResult);
            const auto& eventState = std::get<std::optional<std::string>>(eventStateResult);
            if (!calendarState || !eventState)
            {
                fullRefreshRequired = true;
                break;
            }

            const auto calendarRequest = api::calendarChanges(
                {.accountId = accountId, .sinceState = *calendarState, .maxChanges = std::nullopt});
            const auto eventRequest = api::calendarEventChanges(
                {.accountId = accountId, .sinceState = *eventState, .maxChanges = std::nullopt});
            if (!calendarRequest || !eventRequest)
                co_return error(OperationErrorCode::InvalidRequest,
                                QStringLiteral("Unable to serialize calendar changes."));
            api::RequestBuilder builder;
            builder.useCore().useCapability(std::string{api::calendarsCapabilityUri});
            const auto calendarHandle = builder.call(*calendarRequest, "calendar-changes");
            const auto eventHandle = builder.call(*eventRequest, "calendar-event-changes");
            const auto result =
                co_await caller.call(context(settings, session, accountId), builder);
            if (!isCurrentRefresh(ownerAccountId, generation))
                co_return summary;
            const auto* envelope = std::get_if<api::ResponseEnvelope>(&result);
            if (!envelope)
                co_return callError(result);
            const api::ResponseReader reader{*envelope};
            const auto calendarRead = reader.require(calendarHandle);
            const auto eventRead = reader.require(eventHandle);
            const auto cannotCalculate = [](const auto& read)
            {
                const auto* readError = std::get_if<api::ResponseReaderError>(&read);
                return readError && readError->methodError &&
                       readError->methodError->type == "cannotCalculateChanges";
            };
            if (cannotCalculate(calendarRead) || cannotCalculate(eventRead))
            {
                fullRefreshRequired = true;
                break;
            }
            if (const auto* readError = std::get_if<api::ResponseReaderError>(&calendarRead))
                co_return responseError(*readError);
            if (const auto* readError = std::get_if<api::ResponseReaderError>(&eventRead))
                co_return responseError(*readError);
            const auto& calendarChanges = std::get<api::CalendarChangesResponse>(calendarRead);
            const auto& eventChanges = std::get<api::CalendarEventChangesResponse>(eventRead);
            const auto hasChanges = [](const api::ChangesResponse& changes)
            {
                return changes.hasMoreChanges || !changes.created.empty() ||
                       !changes.updated.empty() || !changes.destroyed.empty();
            };
            if (hasChanges(calendarChanges) || eventChanges.hasMoreChanges)
            {
                fullRefreshRequired = true;
                break;
            }
            std::vector<std::string> changedIds = eventChanges.created;
            changedIds.insert(changedIds.end(), eventChanges.updated.begin(),
                              eventChanges.updated.end());
            std::ranges::sort(changedIds);
            const auto [firstDuplicate, end] = std::ranges::unique(changedIds);
            changedIds.erase(firstDuplicate, end);
            if (session.capabilities.coreDetails &&
                session.capabilities.coreDetails->maxObjectsInGet &&
                changedIds.size() > *session.capabilities.coreDetails->maxObjectsInGet)
            {
                fullRefreshRequired = true;
                break;
            }

            std::vector<CalendarEvent> changedEvents;
            if (!changedIds.empty())
            {
                const auto getRequest =
                    api::calendarEventGet({.accountId = accountId,
                                           .ids = changedIds,
                                           .idsReference = std::nullopt,
                                           .properties = calendarEventReadProperties(),
                                           .recurrenceOverridesBefore = std::nullopt,
                                           .recurrenceOverridesAfter = std::nullopt,
                                           .reduceParticipants = false,
                                           .timeZone = displayTimeZone});
                if (!getRequest)
                    co_return error(OperationErrorCode::InvalidRequest,
                                    QStringLiteral("Unable to serialize changed calendar events."));
                api::RequestBuilder getBuilder;
                getBuilder.useCore().useCapability(std::string{api::calendarsCapabilityUri});
                const auto getHandle = getBuilder.call(*getRequest, "changed-calendar-events");
                const auto getResult =
                    co_await caller.call(context(settings, session, accountId), getBuilder);
                if (!isCurrentRefresh(ownerAccountId, generation))
                    co_return summary;
                const auto* getEnvelope = std::get_if<api::ResponseEnvelope>(&getResult);
                if (!getEnvelope)
                    co_return callError(getResult);
                const auto getRead = api::ResponseReader{*getEnvelope}.require(getHandle);
                if (const auto* readError = std::get_if<api::ResponseReaderError>(&getRead))
                    co_return responseError(*readError);
                auto getResponse = std::get<api::CalendarEventGetResponse>(getRead);
                if (!getResponse.notFound.empty() ||
                    std::ranges::any_of(getResponse.list, [](const auto& event)
                                        { return event.recurrenceRule.has_value(); }))
                {
                    fullRefreshRequired = true;
                    break;
                }
                changedEvents = std::move(getResponse.list);
            }

            std::vector<Occurrence> changedOccurrences;
            changedOccurrences.reserve(changedEvents.size());
            for (const auto& event : changedEvents)
                changedOccurrences.push_back({.accountId = accountId,
                                              .id = event.id,
                                              .eventId = event.id,
                                              .recurrenceId = std::nullopt,
                                              .localStart = event.start,
                                              .localEnd = localEnd(event),
                                              .utcStart = event.utcStart,
                                              .utcEnd = event.utcEnd,
                                              .allDay = event.showWithoutTime});
            const auto calendarCurrent = fenceIsCurrent(m_connection, calendarFence);
            const auto eventCurrent = fenceIsCurrent(m_connection, eventFence);
            if (const auto* serviceError = std::get_if<OperationError>(&calendarCurrent))
                co_return *serviceError;
            if (const auto* serviceError = std::get_if<OperationError>(&eventCurrent))
                co_return *serviceError;
            if (!std::get<bool>(calendarCurrent) || !std::get<bool>(eventCurrent))
                co_return summary;
            if (const auto cacheError = repository.applyEventDelta(
                    accountId, calendarChanges.newState, eventChanges.newState, displayTimeZone,
                    changedEvents, changedOccurrences, eventChanges.destroyed))
                co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            if (!changedEvents.empty() || !eventChanges.destroyed.empty())
            {
                ++summary.accountCount;
                summary.eventCount += changedOccurrences.size();
            }
        }
        if (fullRefreshRequired)
            co_return co_await refresh(std::move(settings), std::move(ownerAccountId),
                                       std::move(interval), std::move(displayTimeZone));
        co_return summary;
    }

    QCoro::Task<CalendarRefreshResult> CalendarService::refresh(LiveConnectionSettings settings,
                                                                std::string ownerAccountId,
                                                                VisibleInterval interval,
                                                                TimeZoneId displayTimeZone)
    {
        const auto generation = beginRefresh(ownerAccountId);
        const auto sessionResult = loadSession(m_connection, ownerAccountId);
        if (const auto* serviceError = std::get_if<OperationError>(&sessionResult))
            co_return *serviceError;
        const auto& session = std::get<api::Session>(sessionResult);
        RefreshedRange summary{.interval = interval,
                               .displayTimeZone = displayTimeZone,
                               .accountCount = 0,
                               .eventCount = 0};
        api::MethodCaller caller{m_methodTransport};
        for (const auto& [accountId, account] : session.accounts)
        {
            if (!account.accountCapabilities.calendars)
                continue;
            const auto calendarFenceResult = captureFence(m_connection, accountId, "Calendar");
            const auto eventFenceResult = captureFence(m_connection, accountId, "CalendarEvent");
            if (const auto* serviceError = std::get_if<OperationError>(&calendarFenceResult))
                co_return *serviceError;
            if (const auto* serviceError = std::get_if<OperationError>(&eventFenceResult))
                co_return *serviceError;
            const auto calendarFence =
                std::get<javelin::jmap::sync::RefreshFence>(calendarFenceResult);
            const auto eventFence = std::get<javelin::jmap::sync::RefreshFence>(eventFenceResult);
            const auto calendarRequest = api::calendarGet({.accountId = accountId,
                                                           .ids = std::nullopt,
                                                           .idsReference = std::nullopt,
                                                           .properties = std::nullopt});
            const auto queryRequest =
                api::calendarEventQuery({.accountId = accountId,
                                         .filter = {.inCalendar = std::nullopt,
                                                    .after = interval.start,
                                                    .before = interval.end,
                                                    .text = std::nullopt},
                                         .expandRecurrences = true,
                                         .timeZone = displayTimeZone,
                                         .position = 0,
                                         .limit = std::nullopt,
                                         .calculateTotal = true});
            // Expanded ids are opaque occurrence handles. Fetch the unexpanded result set as the
            // source of stable, editable event documents.
            const auto baseQueryRequest =
                api::calendarEventQuery({.accountId = accountId,
                                         .filter = {.inCalendar = std::nullopt,
                                                    .after = interval.start,
                                                    .before = interval.end,
                                                    .text = std::nullopt},
                                         .expandRecurrences = false,
                                         .timeZone = displayTimeZone,
                                         .position = 0,
                                         .limit = std::nullopt,
                                         .calculateTotal = true});
            if (!calendarRequest || !queryRequest || !baseQueryRequest)
                co_return error(OperationErrorCode::InvalidRequest,
                                QStringLiteral("Unable to serialize the calendar range request."));
            api::RequestBuilder builder;
            builder.useCore().useCapability(std::string{api::calendarsCapabilityUri});
            const auto calendarHandle = builder.call(*calendarRequest, "calendar-get");
            const auto queryHandle = builder.call(*queryRequest, "calendar-event-query");
            const auto baseQueryHandle =
                builder.call(*baseQueryRequest, "calendar-base-event-query");
            const auto result =
                co_await caller.call(context(settings, session, accountId), builder);
            if (!isCurrentRefresh(ownerAccountId, generation))
                co_return summary;
            const auto* envelope = std::get_if<api::ResponseEnvelope>(&result);
            if (!envelope)
                co_return callError(result);
            api::ResponseReader reader{*envelope};
            const auto calendarsRead = reader.require(calendarHandle);
            if (const auto* readError = std::get_if<api::ResponseReaderError>(&calendarsRead))
                co_return responseError(*readError);
            const auto queryRead = reader.require(queryHandle);
            if (const auto* readError = std::get_if<api::ResponseReaderError>(&queryRead))
                co_return responseError(*readError);
            const auto baseQueryRead = reader.require(baseQueryHandle);
            if (const auto* readError = std::get_if<api::ResponseReaderError>(&baseQueryRead))
                co_return responseError(*readError);
            const auto& calendars = std::get<api::CalendarGetResponse>(calendarsRead);
            const auto& query = std::get<api::CalendarEventQueryResponse>(queryRead);
            const auto& baseQuery = std::get<api::CalendarEventQueryResponse>(baseQueryRead);
            qCDebug(logCalendarService).noquote()
                << "calendar range query" << QString::fromStdString(accountId)
                << QString::fromStdString(interval.start.value)
                << QString::fromStdString(interval.end.value) << "expanded" << query.ids.size()
                << "base" << baseQuery.ids.size();
            sync::MutationJournalRepository genericJournal{m_connection};
            const auto activeCalendarMutations =
                genericJournal.listActive({.accountId = accountId, .dataType = "Calendar"});
            if (const auto* cacheError =
                    std::get_if<cache::DatabaseError>(&activeCalendarMutations))
                co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            const auto& activeDefaults =
                std::get<std::vector<sync::MutationRecord>>(activeCalendarMutations);
            if (!activeDefaults.empty())
            {
                const auto calendarCurrent = fenceGenerationIsCurrent(m_connection, calendarFence);
                if (const auto* serviceError = std::get_if<OperationError>(&calendarCurrent))
                    co_return *serviceError;
                if (!std::get<bool>(calendarCurrent))
                    co_return summary;
                if (std::ranges::any_of(
                        activeDefaults, [](const auto& mutation)
                        { return mutation.status != sync::MutationStatus::Unknown; }))
                    co_return summary;
                std::unordered_map<std::string, bool> defaults;
                for (const auto& calendar : calendars.list)
                    defaults.emplace(calendar.id, calendar.isDefault);
                auto resolveResult = sync::MutationProjectionTransaction::begin(
                    m_connection, QStringLiteral("Resolve default Calendar uncertainty"));
                if (const auto* cacheError = std::get_if<cache::DatabaseError>(&resolveResult))
                    co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
                auto resolve =
                    std::get<sync::MutationProjectionTransaction>(std::move(resolveResult));
                const std::array domains{sync::ConsistencyDomain{
                    .accountId = accountId,
                    .dataType = "Calendar",
                }};
                for (const auto& mutation : activeDefaults)
                {
                    const auto selected = defaults.find(mutation.objectId);
                    const auto status = selected != defaults.end() && selected->second
                                            ? sync::MutationStatus::Accepted
                                            : sync::MutationStatus::Rejected;
                    if (const auto cacheError =
                            resolve.transition(mutation.mutationId, status, calendars.state))
                        co_return error(OperationErrorCode::LocalStorageFailure,
                                        cacheError->message);
                    if (const auto cacheError = resolve.remove(mutation.mutationId))
                        co_return error(OperationErrorCode::LocalStorageFailure,
                                        cacheError->message);
                }
                if (const auto cacheError = resolve.advance(domains))
                    co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
                cache::CalendarRepository repository{m_connection};
                if (const auto cacheError = repository.applyCalendarDefaults(
                        resolve.cacheTransaction(), accountId, calendars.state, defaults))
                    co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
                if (const auto cacheError = resolve.commit())
                    co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
                co_return summary;
            }
            auto eventIds = query.ids;
            const auto total = query.total.value_or(eventIds.size());
            while (eventIds.size() < total)
            {
                const auto nextRequest =
                    api::calendarEventQuery({.accountId = accountId,
                                             .filter = {.inCalendar = std::nullopt,
                                                        .after = interval.start,
                                                        .before = interval.end,
                                                        .text = std::nullopt},
                                             .expandRecurrences = true,
                                             .timeZone = displayTimeZone,
                                             .position = eventIds.size(),
                                             .limit = query.limit,
                                             .calculateTotal = false});
                if (!nextRequest)
                    co_return error(OperationErrorCode::InvalidRequest,
                                    QStringLiteral("Unable to serialize calendar pagination."));
                api::RequestBuilder nextBuilder;
                nextBuilder.useCore().useCapability(std::string{api::calendarsCapabilityUri});
                const auto nextHandle = nextBuilder.call(*nextRequest, "calendar-event-query");
                const auto nextResult =
                    co_await caller.call(context(settings, session, accountId), nextBuilder);
                if (!isCurrentRefresh(ownerAccountId, generation))
                    co_return summary;
                const auto* nextEnvelope = std::get_if<api::ResponseEnvelope>(&nextResult);
                if (!nextEnvelope)
                    co_return callError(nextResult);
                const auto nextRead = api::ResponseReader{*nextEnvelope}.require(nextHandle);
                if (const auto* readError = std::get_if<api::ResponseReaderError>(&nextRead))
                    co_return responseError(*readError);
                auto next = std::get<api::CalendarEventQueryResponse>(nextRead);
                if (next.ids.empty())
                    co_return error(OperationErrorCode::ProtocolViolation,
                                    QStringLiteral("Calendar query pagination stopped early."));
                eventIds.insert(eventIds.end(), std::make_move_iterator(next.ids.begin()),
                                std::make_move_iterator(next.ids.end()));
            }

            auto baseEventIds = baseQuery.ids;
            const auto baseTotal = baseQuery.total.value_or(baseEventIds.size());
            while (baseEventIds.size() < baseTotal)
            {
                const auto nextRequest =
                    api::calendarEventQuery({.accountId = accountId,
                                             .filter = {.inCalendar = std::nullopt,
                                                        .after = interval.start,
                                                        .before = interval.end,
                                                        .text = std::nullopt},
                                             .expandRecurrences = false,
                                             .timeZone = displayTimeZone,
                                             .position = baseEventIds.size(),
                                             .limit = baseQuery.limit,
                                             .calculateTotal = false});
                if (!nextRequest)
                    co_return error(OperationErrorCode::InvalidRequest,
                                    QStringLiteral("Unable to serialize base event pagination."));
                api::RequestBuilder nextBuilder;
                nextBuilder.useCore().useCapability(std::string{api::calendarsCapabilityUri});
                const auto nextHandle = nextBuilder.call(*nextRequest, "calendar-base-event-query");
                const auto nextResult =
                    co_await caller.call(context(settings, session, accountId), nextBuilder);
                if (!isCurrentRefresh(ownerAccountId, generation))
                    co_return summary;
                const auto* nextEnvelope = std::get_if<api::ResponseEnvelope>(&nextResult);
                if (!nextEnvelope)
                    co_return callError(nextResult);
                const auto nextRead = api::ResponseReader{*nextEnvelope}.require(nextHandle);
                if (const auto* readError = std::get_if<api::ResponseReaderError>(&nextRead))
                    co_return responseError(*readError);
                auto next = std::get<api::CalendarEventQueryResponse>(nextRead);
                if (next.ids.empty())
                    co_return error(
                        OperationErrorCode::ProtocolViolation,
                        QStringLiteral("Base calendar query pagination stopped early."));
                baseEventIds.insert(baseEventIds.end(), std::make_move_iterator(next.ids.begin()),
                                    std::make_move_iterator(next.ids.end()));
            }

            const auto batchLimit =
                session.capabilities.coreDetails &&
                        session.capabilities.coreDetails->maxObjectsInGet
                    ? static_cast<std::size_t>(*session.capabilities.coreDetails->maxObjectsInGet)
                    : std::numeric_limits<std::size_t>::max();
            auto getResult = co_await getCalendarEventsBatched(
                caller, settings, session, accountId, eventIds, displayTimeZone, batchLimit,
                "calendar-event-get", [this, &ownerAccountId, generation]
                { return isCurrentRefresh(ownerAccountId, generation); });
            if (std::holds_alternative<SupersededRefresh>(getResult))
                co_return summary;
            if (const auto* serviceError = std::get_if<OperationError>(&getResult))
                co_return *serviceError;
            auto events = std::get<api::CalendarEventGetResponse>(std::move(getResult));
            auto baseGetResult = co_await getCalendarEventsBatched(
                caller, settings, session, accountId, baseEventIds, displayTimeZone, batchLimit,
                "calendar-base-event-get", [this, &ownerAccountId, generation]
                { return isCurrentRefresh(ownerAccountId, generation); });
            if (std::holds_alternative<SupersededRefresh>(baseGetResult))
                co_return summary;
            if (const auto* serviceError = std::get_if<OperationError>(&baseGetResult))
                co_return *serviceError;
            auto baseResponse = std::get<api::CalendarEventGetResponse>(std::move(baseGetResult));

            std::unordered_set<std::string> baseUids;
            for (const auto& event : baseResponse.list)
                if (!event.uid.empty())
                    baseUids.insert(event.uid);
            std::unordered_set<std::string> missingBaseUids;
            for (const auto& occurrence : events.list)
                if (occurrence.recurrenceId && !occurrence.uid.empty() &&
                    !baseUids.contains(occurrence.uid))
                    missingBaseUids.insert(occurrence.uid);
            std::vector<std::string> recoveredBaseIds;
            for (const auto& uid : missingBaseUids)
            {
                qCDebug(logCalendarService).noquote()
                    << "recover recurring calendar base by uid" << QString::fromStdString(accountId)
                    << QString::fromStdString(uid);
                std::uint64_t position = 0;
                std::optional<std::uint64_t> recoveryTotal;
                do
                {
                    const auto recoveryRequest =
                        api::calendarEventQuery({.accountId = accountId,
                                                 .filter = {.uid = uid},
                                                 .expandRecurrences = false,
                                                 .timeZone = displayTimeZone,
                                                 .position = position,
                                                 .limit = std::nullopt,
                                                 .calculateTotal = !recoveryTotal.has_value()});
                    if (!recoveryRequest)
                        co_return error(OperationErrorCode::InvalidRequest,
                                        QStringLiteral("Unable to serialize a recurring event "
                                                       "recovery query."));
                    api::RequestBuilder recoveryBuilder;
                    recoveryBuilder.useCore().useCapability(
                        std::string{api::calendarsCapabilityUri});
                    const auto recoveryHandle = recoveryBuilder.call(
                        *recoveryRequest, "calendar-base-event-recovery-query");
                    const auto recoveryResult = co_await caller.call(
                        context(settings, session, accountId), recoveryBuilder);
                    if (!isCurrentRefresh(ownerAccountId, generation))
                        co_return summary;
                    const auto* recoveryEnvelope =
                        std::get_if<api::ResponseEnvelope>(&recoveryResult);
                    if (!recoveryEnvelope)
                        co_return callError(recoveryResult);
                    const auto recoveryRead =
                        api::ResponseReader{*recoveryEnvelope}.require(recoveryHandle);
                    if (const auto* readError =
                            std::get_if<api::ResponseReaderError>(&recoveryRead))
                        co_return responseError(*readError);
                    auto recovery = std::get<api::CalendarEventQueryResponse>(recoveryRead);
                    if (!recoveryTotal.has_value())
                        recoveryTotal = recovery.total.value_or(recovery.ids.size());
                    if (recovery.ids.empty() && position < *recoveryTotal)
                        co_return error(OperationErrorCode::ProtocolViolation,
                                        QStringLiteral("Recurring event recovery pagination "
                                                       "stopped early."));
                    position += recovery.ids.size();
                    recoveredBaseIds.insert(recoveredBaseIds.end(),
                                            std::make_move_iterator(recovery.ids.begin()),
                                            std::make_move_iterator(recovery.ids.end()));
                } while (position < *recoveryTotal);
            }
            std::ranges::sort(recoveredBaseIds);
            recoveredBaseIds.erase(std::unique(recoveredBaseIds.begin(), recoveredBaseIds.end()),
                                   recoveredBaseIds.end());
            if (!recoveredBaseIds.empty())
            {
                auto recoveredGetResult = co_await getCalendarEventsBatched(
                    caller, settings, session, accountId, recoveredBaseIds, displayTimeZone,
                    batchLimit, "calendar-base-event-recovery-get",
                    [this, &ownerAccountId, generation]
                    { return isCurrentRefresh(ownerAccountId, generation); });
                if (std::holds_alternative<SupersededRefresh>(recoveredGetResult))
                    co_return summary;
                if (const auto* serviceError = std::get_if<OperationError>(&recoveredGetResult))
                    co_return *serviceError;
                auto recovered =
                    std::get<api::CalendarEventGetResponse>(std::move(recoveredGetResult));
                if (recovered.state != events.state)
                    co_return error(
                        OperationErrorCode::ProtocolViolation,
                        QStringLiteral("Recovered recurring events returned an inconsistent "
                                       "state."));
                if (!recovered.notFound.empty())
                    co_return error(OperationErrorCode::ProtocolViolation,
                                    QStringLiteral("The server did not return a recovered "
                                                   "recurring event."));
                baseResponse.list.insert(baseResponse.list.end(),
                                         std::make_move_iterator(recovered.list.begin()),
                                         std::make_move_iterator(recovered.list.end()));
            }
            if (events.state != baseResponse.state)
                co_return error(
                    OperationErrorCode::ProtocolViolation,
                    QStringLiteral("Calendar event batches returned inconsistent states."));
            if (!events.notFound.empty() || !baseResponse.notFound.empty())
                co_return error(OperationErrorCode::ProtocolViolation,
                                QStringLiteral("The server did not return a calendar event from "
                                               "the range query."));
            cache::CalendarRepository repository{m_connection};
            CalendarMutationJournal mutationJournal{m_connection, repository};
            const auto activeResult = mutationJournal.listActive(accountId);
            if (const auto* cacheError = std::get_if<cache::DatabaseError>(&activeResult))
                co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            const auto& activeMutations =
                std::get<std::vector<CalendarMutationRecord>>(activeResult);
            const auto rebasedResult =
                rebaseCalendarEvents(std::move(baseResponse.list), activeMutations);
            if (const auto* operationError = std::get_if<OperationError>(&rebasedResult))
                co_return *operationError;
            auto rebased = std::get<RebasedCalendarEvents>(rebasedResult);
            baseResponse.list = std::move(rebased.events);

            std::vector<Occurrence> occurrences;
            occurrences.reserve(events.list.size() + baseResponse.list.size());
            std::unordered_map<std::string, std::string> baseIdByUid;
            std::unordered_set<std::string> recurringBaseIds;
            for (const auto& event : baseResponse.list)
            {
                if (!event.uid.empty())
                    baseIdByUid.emplace(event.uid, event.id);
                if (event.recurrenceRule)
                {
                    recurringBaseIds.insert(event.id);
                    if (rebased.suppressedExpansionIds.contains(event.id))
                        occurrences.push_back(projectedOccurrence(event));
                    continue;
                }
                occurrences.push_back({.accountId = accountId,
                                       .id = event.id,
                                       .eventId = event.id,
                                       .recurrenceId = std::nullopt,
                                       .localStart = event.start,
                                       .localEnd = localEnd(event),
                                       .utcStart = event.utcStart,
                                       .utcEnd = event.utcEnd,
                                       .allDay = event.showWithoutTime});
            }
            for (const auto& event : events.list)
            {
                if ((event.baseEventId &&
                     rebased.suppressedExpansionIds.contains(*event.baseEventId)) ||
                    (!event.uid.empty() && rebased.suppressedExpansionUids.contains(event.uid)))
                    continue;
                std::optional<std::string> eventId;
                if (event.baseEventId && recurringBaseIds.contains(*event.baseEventId))
                    eventId = event.baseEventId;
                else if (event.recurrenceId && !event.uid.empty())
                {
                    const auto base = baseIdByUid.find(event.uid);
                    if (base != baseIdByUid.end() && recurringBaseIds.contains(base->second))
                        eventId = base->second;
                }
                if (!eventId)
                {
                    if (event.recurrenceId)
                        qCWarning(logCalendarService).noquote()
                            << "discard unmatched expanded calendar occurrence"
                            << QString::fromStdString(accountId) << QString::fromStdString(event.id)
                            << QString::fromStdString(event.uid)
                            << QString::fromStdString(event.recurrenceId->value)
                            << QString::fromStdString(event.title);
                    continue;
                }
                occurrences.push_back({.accountId = accountId,
                                       .id = event.id,
                                       .eventId = *eventId,
                                       .recurrenceId = event.recurrenceId,
                                       .localStart = event.start,
                                       .localEnd = localEnd(event),
                                       .utcStart = event.utcStart,
                                       .utcEnd = event.utcEnd,
                                       .allDay = event.showWithoutTime});
            }
            const auto calendarCurrent = fenceGenerationIsCurrent(m_connection, calendarFence);
            const auto eventCurrent = fenceGenerationIsCurrent(m_connection, eventFence);
            if (const auto* serviceError = std::get_if<OperationError>(&calendarCurrent))
                co_return *serviceError;
            if (const auto* serviceError = std::get_if<OperationError>(&eventCurrent))
                co_return *serviceError;
            if (!std::get<bool>(calendarCurrent) || !std::get<bool>(eventCurrent))
                co_return summary;
            if (const auto cacheError =
                    repository.replaceCalendars(accountId, calendars.state, calendars.list))
                co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            cache::CalendarWindow window{.accountId = accountId,
                                         .start = interval.start,
                                         .end = interval.end,
                                         .displayTimeZone = displayTimeZone,
                                         .queryState = query.queryState,
                                         .eventState = baseResponse.state,
                                         .events = std::move(baseResponse.list),
                                         .occurrences = std::move(occurrences)};
            if (const auto cacheError = repository.reconcileWindow(window))
                co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            if (!rebased.acceptedUnknown.empty())
            {
                auto transactionResult = javelin::jmap::sync::MutationProjectionTransaction::begin(
                    m_connection, QStringLiteral("Resolve CalendarEvent uncertainty"));
                if (const auto* cacheError = std::get_if<cache::DatabaseError>(&transactionResult))
                    co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
                auto transaction = std::get<javelin::jmap::sync::MutationProjectionTransaction>(
                    std::move(transactionResult));
                const std::array domains{javelin::jmap::sync::ConsistencyDomain{
                    .accountId = accountId,
                    .dataType = "CalendarEvent",
                }};
                if (const auto cacheError = transaction.advance(domains))
                    co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
                for (const auto* mutation : rebased.acceptedUnknown)
                {
                    if (const auto cacheError = transaction.transition(
                            mutation->mutationId, javelin::jmap::sync::MutationStatus::Accepted,
                            baseResponse.state))
                        co_return error(OperationErrorCode::LocalStorageFailure,
                                        cacheError->message);
                    if (const auto cacheError = transaction.remove(mutation->mutationId))
                        co_return error(OperationErrorCode::LocalStorageFailure,
                                        cacheError->message);
                }
                if (const auto cacheError = transaction.commit())
                    co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            }
            ++summary.accountCount;
            summary.eventCount += window.occurrences.size();
        }
        co_return summary;
    }

    QCoro::Task<CalendarMutationResult>
    CalendarService::create(LiveConnectionSettings settings, std::string ownerAccountId,
                            CreateEventCommand command, std::function<void()> projectionCommitted)
    {
        if (!command.ifInState)
        {
            const auto state = currentEventState(m_connection, command.accountId);
            if (const auto* serviceError = std::get_if<OperationError>(&state))
                co_return *serviceError;
            command.ifInState = std::get<std::string>(state);
        }
        if (command.event.uid.empty())
            command.event.uid = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        const auto id = command.event.id.empty() ? std::string{"event"} : command.event.id;
        std::vector<std::string> calendarIds;
        for (const auto& [calendarId, present] : command.event.calendarIds)
            if (present)
                calendarIds.push_back(calendarId);
        api::CalendarEventSetRequest request{.accountId = command.accountId,
                                             .ifInState = command.ifInState,
                                             .create = {{id, std::move(command.event)}},
                                             .update = {},
                                             .destroy = {},
                                             .sendSchedulingMessages = true};
        co_return co_await mutate(
            std::move(settings), std::move(ownerAccountId), std::move(request),
            std::move(calendarIds), std::move(command.operationGroupId),
            std::move(command.materialization), std::move(projectionCommitted));
    }

    QCoro::Task<CalendarMutationResult>
    CalendarService::update(LiveConnectionSettings settings, std::string ownerAccountId,
                            UpdateEventCommand command, std::function<void()> projectionCommitted)
    {
        if (!command.ifInState)
        {
            const auto state = currentEventState(m_connection, command.accountId);
            if (const auto* serviceError = std::get_if<OperationError>(&state))
                co_return *serviceError;
            command.ifInState = std::get<std::string>(state);
        }
        std::vector<std::string> calendarIds;
        for (const auto& [calendarId, present] : command.event.calendarIds)
            if (present)
                calendarIds.push_back(calendarId);
        const auto eventId = command.event.id;
        cache::CalendarRepository repository{m_connection};
        const auto cached = repository.findEvent(command.accountId, eventId);
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&cached))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        const auto& previous = std::get<std::optional<CalendarEvent>>(cached);
        if (!previous)
            co_return error(OperationErrorCode::InvalidRequest,
                            QStringLiteral("The calendar event is no longer in the cache."));
        if (*previous == command.event)
            co_return CommittedMutation{.accountId = std::move(command.accountId),
                                        .newState = *command.ifInState,
                                        .createdId = std::nullopt,
                                        .receipt = {}};
        api::CalendarEventSetRequest request{
            .accountId = command.accountId,
            .ifInState = command.ifInState,
            .create = {},
            .update = {{eventId, {.previous = *previous, .event = std::move(command.event)}}},
            .destroy = {},
            .sendSchedulingMessages = true};
        co_return co_await mutate(
            std::move(settings), std::move(ownerAccountId), std::move(request),
            std::move(calendarIds), std::move(command.operationGroupId),
            std::move(command.materialization), std::move(projectionCommitted));
    }

    QCoro::Task<CalendarMutationResult>
    CalendarService::remove(LiveConnectionSettings settings, std::string ownerAccountId,
                            DeleteEventCommand command, std::function<void()> projectionCommitted)
    {
        if (!command.ifInState)
        {
            const auto state = currentEventState(m_connection, command.accountId);
            if (const auto* serviceError = std::get_if<OperationError>(&state))
                co_return *serviceError;
            command.ifInState = std::get<std::string>(state);
        }
        api::CalendarEventSetRequest request{.accountId = command.accountId,
                                             .ifInState = command.ifInState,
                                             .create = {},
                                             .update = {},
                                             .destroy = {command.eventId},
                                             .sendSchedulingMessages = true};
        co_return co_await mutate(std::move(settings), std::move(ownerAccountId),
                                  std::move(request), std::move(command.calendarIds),
                                  std::move(command.operationGroupId), std::nullopt,
                                  std::move(projectionCommitted));
    }

    QCoro::Task<CalendarMutationResult>
    CalendarService::mutate(LiveConnectionSettings settings, std::string ownerAccountId,
                            api::CalendarEventSetRequest request,
                            std::vector<std::string> calendarIds,
                            std::optional<std::string> operationGroupId,
                            std::optional<CalendarRangeMaterialization> materialization,
                            std::function<void()> projectionCommitted)
    {
        const auto sessionResult = loadSession(m_connection, ownerAccountId);
        if (const auto* serviceError = std::get_if<OperationError>(&sessionResult))
            co_return *serviceError;
        const auto& session = std::get<api::Session>(sessionResult);
        const auto account = session.accounts.find(request.accountId);
        if (account == session.accounts.end() || !account->second.accountCapabilities.calendars)
            co_return error(
                OperationErrorCode::UnsupportedCapability,
                QStringLiteral("This account does not support JMAP Calendars draft-26."));
        cache::CalendarRepository repository{m_connection};
        const auto calendarsResult = repository.listCalendars(request.accountId);
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&calendarsResult))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        if (!writable(std::get<std::vector<Calendar>>(calendarsResult), calendarIds))
            co_return error(OperationErrorCode::PermissionDenied,
                            QStringLiteral("You do not have permission to modify this event."));
        const auto method = api::calendarEventSet(request);
        if (!method)
            co_return error(OperationErrorCode::InvalidRequest,
                            QStringLiteral("Unable to serialize the calendar change."));
        const auto preparedResult = prepareCalendarMutations(repository, request, operationGroupId);
        if (const auto* operationError = std::get_if<OperationError>(&preparedResult))
            co_return *operationError;
        auto prepared = std::get<PreparedCalendarMutations>(preparedResult);
        CalendarMutationJournal journal{m_connection, repository};
        if (!prepared.records.empty())
        {
            if (const auto cacheError = journal.queue(
                    prepared.records, request.ifInState.value_or(std::string{}),
                    prepared.projectedEvents, prepared.projectedOccurrences, prepared.destroyedIds))
                co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            if (projectionCommitted)
                projectionCommitted();
            if (const auto cacheError = journal.transition(
                    prepared.records, javelin::jmap::sync::MutationStatus::InFlight))
                co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        }
        api::RequestBuilder builder;
        builder.useCore().useCapability(std::string{api::calendarsCapabilityUri});
        const auto handle = builder.call(*method, "calendar-event-set");
        std::optional<api::CallHandle<api::CalendarEventQueryResponse>> queryHandle;
        std::optional<api::CallHandle<api::CalendarEventGetResponse>> getHandle;
        if (materialization.has_value())
        {
            const auto query = api::calendarEventQuery({
                .accountId = request.accountId,
                .filter =
                    {
                        .inCalendar = std::nullopt,
                        .after = materialization->interval.start,
                        .before = materialization->interval.end,
                        .text = std::nullopt,
                        .uid = std::nullopt,
                    },
                .expandRecurrences = true,
                .timeZone = materialization->displayTimeZone,
                .position = 0,
                .limit = std::nullopt,
                .calculateTotal = true,
            });
            if (!query)
                co_return error(OperationErrorCode::InvalidRequest,
                                QStringLiteral("Unable to serialize recurrence materialization."));
            queryHandle = builder.call(*query, "calendar-event-materialization-query");
            const auto get = api::calendarEventGet({
                .accountId = request.accountId,
                .ids = std::nullopt,
                .idsReference = api::resultReference(*queryHandle, "/ids"),
                .properties = std::nullopt,
                .recurrenceOverridesBefore = std::nullopt,
                .recurrenceOverridesAfter = std::nullopt,
                .reduceParticipants = false,
                .timeZone = materialization->displayTimeZone,
            });
            if (!get)
                co_return error(OperationErrorCode::InvalidRequest,
                                QStringLiteral("Unable to serialize expanded event retrieval."));
            getHandle = builder.call(*get, "calendar-event-materialization-get");
        }
        api::MethodCaller caller{m_methodTransport};
        const auto result =
            co_await caller.call(context(settings, session, request.accountId), builder);
        const auto* envelope = std::get_if<api::ResponseEnvelope>(&result);
        if (!envelope)
        {
            if (const auto cacheError = journal.transition(
                    prepared.records, javelin::jmap::sync::MutationStatus::Unknown))
                co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            co_return callError(result);
        }
        const auto read = api::ResponseReader{*envelope}.require(handle);
        if (const auto* readError = std::get_if<api::ResponseReaderError>(&read))
        {
            if (const auto cacheError = journal.transition(
                    prepared.records, javelin::jmap::sync::MutationStatus::Unknown))
                co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            co_return responseError(*readError);
        }
        const auto& response = std::get<api::CalendarEventSetResponse>(read);
        std::optional<api::CalendarEventQueryResponse> materializedQuery;
        std::optional<api::CalendarEventGetResponse> materializedEvents;
        if (queryHandle.has_value() && getHandle.has_value())
        {
            const api::ResponseReader reader{*envelope};
            const auto queryRead = reader.require(*queryHandle);
            const auto getRead = reader.require(*getHandle);
            if (const auto* queryResponse =
                    std::get_if<api::CalendarEventQueryResponse>(&queryRead);
                queryResponse != nullptr)
                materializedQuery = *queryResponse;
            if (const auto* getResponse = std::get_if<api::CalendarEventGetResponse>(&getRead);
                getResponse != nullptr)
                materializedEvents = *getResponse;
        }
        const auto* setError = !response.notCreated.empty()   ? &response.notCreated.begin()->second
                               : !response.notUpdated.empty() ? &response.notUpdated.begin()->second
                               : !response.notDestroyed.empty()
                                   ? &response.notDestroyed.begin()->second
                                   : nullptr;
        if (setError)
        {
            const auto errorJson = setError->description.value_or("CalendarEvent/set rejected");
            if (const auto restoreError =
                    restoreCalendarMutations(m_connection, repository, prepared.records,
                                             request.ifInState.value_or(std::string{}), errorJson))
                co_return *restoreError;
            if (projectionCommitted)
                projectionCommitted();
            if (setError->type == api::CalendarSetErrorType::StateMismatch)
                co_return error(
                    OperationErrorCode::Conflict,
                    QStringLiteral("The event changed on the server. Refresh and try again."));
            if (setError->type == api::CalendarSetErrorType::Forbidden)
                co_return error(OperationErrorCode::PermissionDenied,
                                QStringLiteral("The server denied this calendar operation."));
            if (setError->type == api::CalendarSetErrorType::NoSupportedScheduleMethods)
                co_return error(
                    OperationErrorCode::SchedulingUnsupported,
                    QStringLiteral("The server cannot schedule with one or more attendees."));
            co_return error(OperationErrorCode::ProtocolViolation,
                            QString::fromStdString(setError->description.value_or(
                                "The server rejected the calendar change.")));
        }
        auto accepted =
            acceptCalendarMutations(m_connection, repository, prepared.records, response);
        if (const auto* operationError = std::get_if<OperationError>(&accepted))
        {
            static_cast<void>(
                journal.transition(prepared.records, javelin::jmap::sync::MutationStatus::Unknown));
            co_return *operationError;
        }
        bool materializationComplete = !materialization.has_value();
        if (materialization.has_value() && materializedQuery.has_value() &&
            materializedEvents.has_value() &&
            materializedQuery->ids.size() ==
                materializedQuery->total.value_or(materializedQuery->ids.size()) &&
            materializedEvents->notFound.empty())
        {
            auto cached = repository.loadWindow(request.accountId, materialization->interval.start,
                                                materialization->interval.end,
                                                materialization->displayTimeZone);
            auto* window = std::get_if<std::optional<cache::CalendarWindow>>(&cached);
            if (window != nullptr && window->has_value())
            {
                std::unordered_map<std::string, std::string> eventIdByUid;
                for (const auto& event : (*window)->events)
                    eventIdByUid.insert_or_assign(event.uid, event.id);
                std::vector<Occurrence> occurrences;
                occurrences.reserve(materializedEvents->list.size());
                for (const auto& expanded : materializedEvents->list)
                {
                    auto eventId = expanded.baseEventId;
                    if (!eventId.has_value())
                    {
                        const auto base = eventIdByUid.find(expanded.uid);
                        if (base != eventIdByUid.end())
                            eventId = base->second;
                    }
                    if (!eventId.has_value())
                        continue;
                    occurrences.push_back({
                        .accountId = request.accountId,
                        .id = expanded.id,
                        .eventId = *eventId,
                        .recurrenceId = expanded.recurrenceId,
                        .localStart = expanded.start,
                        .localEnd = localEnd(expanded),
                        .utcStart = expanded.utcStart,
                        .utcEnd = expanded.utcEnd,
                        .allDay = expanded.showWithoutTime,
                    });
                }
                (*window)->queryState = materializedQuery->queryState;
                (*window)->eventState = response.newState;
                (*window)->occurrences = std::move(occurrences);
                if (const auto cacheError = repository.reconcileWindow(**window))
                    qCWarning(logCalendarService).noquote()
                        << "Accepted calendar edit needs later range materialization"
                        << cacheError->message;
                else
                    materializationComplete = true;
            }
        }
        std::get<CommittedMutation>(accepted).receipt.incompleteMaterialization =
            !materializationComplete;
        if (projectionCommitted)
            projectionCommitted();
        static_cast<void>(beginRefresh(ownerAccountId));
        co_return accepted;
    }
} // namespace javelin::jmap::calendar
