#include "jmap/calendar/CalendarService.h"

#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/MethodCaller.h"
#include "jmap/api/RequestBuilder.h"
#include "jmap/api/ResponseReader.h"
#include "jmap/auth/Auth.h"
#include "jmap/cache/SessionRepository.h"

#include <QDateTime>
#include <QRegularExpression>

#include <unordered_set>

namespace javelin::jmap::calendar
{
    namespace
    {
        using SessionResult = std::variant<api::Session, CalendarServiceError>;

        CalendarServiceError error(const CalendarServiceErrorCode code, QString message)
        {
            return {.code = code, .message = std::move(message)};
        }

        SessionResult loadSession(cache::DatabaseConnection& connection,
                                  const std::string_view ownerAccountId)
        {
            cache::SessionRepository repository{connection};
            const auto loaded = repository.load(ownerAccountId);
            if (const auto* databaseError = std::get_if<cache::DatabaseError>(&loaded))
                return error(CalendarServiceErrorCode::Cache, databaseError->message);
            const auto& session = std::get<std::optional<api::Session>>(loaded);
            if (!session)
                return error(CalendarServiceErrorCode::Capability,
                             QStringLiteral("No cached JMAP session is available."));
            if (!session->capabilities.calendars)
                return error(
                    CalendarServiceErrorCode::Capability,
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

        CalendarServiceError callError(const api::MethodCallerResult& result)
        {
            if (const auto* value = std::get_if<api::TransportError>(&result))
                return error(CalendarServiceErrorCode::Transport,
                             QString::fromStdString(value->message));
            if (const auto* value = std::get_if<api::AuthError>(&result))
                return error(CalendarServiceErrorCode::Authentication,
                             QString::fromStdString(value->message));
            if (const auto* value = std::get_if<api::ProtocolError>(&result))
                return error(CalendarServiceErrorCode::Protocol,
                             QString::fromStdString(value->message));
            return error(CalendarServiceErrorCode::Protocol,
                         QStringLiteral("The calendar request failed."));
        }

        CalendarServiceError responseError(const api::ResponseReaderError& value)
        {
            if (value.methodError)
            {
                if (value.methodError->type == "stateMismatch")
                    return error(
                        CalendarServiceErrorCode::StaleState,
                        QStringLiteral("The event changed on the server. Refresh and try again."));
                if (value.methodError->type == "forbidden")
                    return error(CalendarServiceErrorCode::Permission,
                                 QStringLiteral("The server denied this calendar operation."));
                if (value.methodError->type == "noSupportedScheduleMethods")
                    return error(
                        CalendarServiceErrorCode::Scheduling,
                        QStringLiteral("The server cannot schedule with one or more attendees."));
            }
            return error(CalendarServiceErrorCode::Protocol, QString::fromStdString(value.message));
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
            return error(CalendarServiceErrorCode::Cache, cacheError->message);
        return std::get<std::optional<cache::CalendarWindow>>(std::move(loaded));
    }

    CalendarAccountsResult CalendarService::accounts() const
    {
        cache::CalendarRepository repository{m_connection};
        auto loaded = repository.listAccounts();
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&loaded))
            return error(CalendarServiceErrorCode::Cache, cacheError->message);
        return std::get<std::vector<cache::CalendarAccount>>(std::move(loaded));
    }

    CalendarListResult CalendarService::calendars(const std::string_view accountId) const
    {
        cache::CalendarRepository repository{m_connection};
        auto loaded = repository.listCalendars(accountId);
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&loaded))
            return error(CalendarServiceErrorCode::Cache, cacheError->message);
        return std::get<std::vector<Calendar>>(std::move(loaded));
    }

    QCoro::Task<CalendarRefreshResult> CalendarService::refresh(LiveConnectionSettings settings,
                                                                std::string ownerAccountId,
                                                                VisibleInterval interval,
                                                                TimeZoneId displayTimeZone)
    {
        const auto sessionResult = loadSession(m_connection, ownerAccountId);
        if (const auto* serviceError = std::get_if<CalendarServiceError>(&sessionResult))
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
            if (!calendarRequest || !queryRequest)
                co_return error(CalendarServiceErrorCode::Validation,
                                QStringLiteral("Unable to serialize the calendar range request."));
            api::RequestBuilder builder;
            builder.useCore().useCapability(std::string{api::calendarsCapabilityUri});
            const auto calendarHandle = builder.call(*calendarRequest, "calendar-get");
            const auto queryHandle = builder.call(*queryRequest, "calendar-event-query");
            const auto result =
                co_await caller.call(context(settings, session, accountId), builder);
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
            const auto& calendars = std::get<api::CalendarGetResponse>(calendarsRead);
            const auto& query = std::get<api::CalendarEventQueryResponse>(queryRead);
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
                    co_return error(CalendarServiceErrorCode::Validation,
                                    QStringLiteral("Unable to serialize calendar pagination."));
                api::RequestBuilder nextBuilder;
                nextBuilder.useCore().useCapability(std::string{api::calendarsCapabilityUri});
                const auto nextHandle = nextBuilder.call(*nextRequest, "calendar-event-query");
                const auto nextResult =
                    co_await caller.call(context(settings, session, accountId), nextBuilder);
                const auto* nextEnvelope = std::get_if<api::ResponseEnvelope>(&nextResult);
                if (!nextEnvelope)
                    co_return callError(nextResult);
                const auto nextRead = api::ResponseReader{*nextEnvelope}.require(nextHandle);
                if (const auto* readError = std::get_if<api::ResponseReaderError>(&nextRead))
                    co_return responseError(*readError);
                auto next = std::get<api::CalendarEventQueryResponse>(nextRead);
                if (next.ids.empty())
                    co_return error(CalendarServiceErrorCode::Protocol,
                                    QStringLiteral("Calendar query pagination stopped early."));
                eventIds.insert(eventIds.end(), std::make_move_iterator(next.ids.begin()),
                                std::make_move_iterator(next.ids.end()));
            }

            const auto getRequest =
                api::calendarEventGet({.accountId = accountId,
                                       .ids = std::move(eventIds),
                                       .properties = std::nullopt,
                                       .recurrenceOverridesBefore = std::nullopt,
                                       .recurrenceOverridesAfter = std::nullopt,
                                       .reduceParticipants = false,
                                       .timeZone = displayTimeZone});
            if (!getRequest)
                co_return error(CalendarServiceErrorCode::Validation,
                                QStringLiteral("Unable to serialize the calendar event request."));
            api::RequestBuilder getBuilder;
            getBuilder.useCore().useCapability(std::string{api::calendarsCapabilityUri});
            const auto getHandle = getBuilder.call(*getRequest, "calendar-event-get");
            const auto getResult =
                co_await caller.call(context(settings, session, accountId), getBuilder);
            const auto* getEnvelope = std::get_if<api::ResponseEnvelope>(&getResult);
            if (!getEnvelope)
                co_return callError(getResult);
            const auto getRead = api::ResponseReader{*getEnvelope}.require(getHandle);
            if (const auto* readError = std::get_if<api::ResponseReaderError>(&getRead))
                co_return responseError(*readError);
            const auto& events = std::get<api::CalendarEventGetResponse>(getRead);

            std::vector<Occurrence> occurrences;
            occurrences.reserve(events.list.size());
            std::vector<CalendarEvent> baseEvents;
            baseEvents.reserve(events.list.size());
            std::unordered_set<std::string> baseEventIds;
            for (const auto& event : events.list)
            {
                const auto eventId = event.baseEventId.value_or(event.id);
                occurrences.push_back({.accountId = accountId,
                                       .id = event.id,
                                       .eventId = eventId,
                                       .recurrenceId = event.recurrenceId,
                                       .localStart = event.start,
                                       .localEnd = localEnd(event),
                                       .utcStart = event.utcStart,
                                       .utcEnd = event.utcEnd,
                                       .allDay = event.showWithoutTime});
                if (event.baseEventId)
                    baseEventIds.insert(*event.baseEventId);
                else
                    baseEvents.push_back(event);
            }
            if (!baseEventIds.empty())
            {
                std::vector<std::string> ids{baseEventIds.begin(), baseEventIds.end()};
                const auto baseRequest =
                    api::calendarEventGet({.accountId = accountId,
                                           .ids = std::move(ids),
                                           .properties = std::nullopt,
                                           .recurrenceOverridesBefore = std::nullopt,
                                           .recurrenceOverridesAfter = std::nullopt,
                                           .reduceParticipants = false,
                                           .timeZone = displayTimeZone});
                if (!baseRequest)
                    co_return error(CalendarServiceErrorCode::Validation,
                                    QStringLiteral("Unable to serialize the base event request."));
                api::RequestBuilder baseBuilder;
                baseBuilder.useCore().useCapability(std::string{api::calendarsCapabilityUri});
                const auto baseHandle = baseBuilder.call(*baseRequest, "calendar-base-event-get");
                const auto baseResult =
                    co_await caller.call(context(settings, session, accountId), baseBuilder);
                const auto* baseEnvelope = std::get_if<api::ResponseEnvelope>(&baseResult);
                if (!baseEnvelope)
                    co_return callError(baseResult);
                const auto baseRead = api::ResponseReader{*baseEnvelope}.require(baseHandle);
                if (const auto* readError = std::get_if<api::ResponseReaderError>(&baseRead))
                    co_return responseError(*readError);
                auto fetched = std::get<api::CalendarEventGetResponse>(baseRead);
                if (!fetched.notFound.empty() || fetched.list.size() != baseEventIds.size())
                    co_return error(CalendarServiceErrorCode::Protocol,
                                    QStringLiteral("The server did not return a referenced base "
                                                   "calendar event."));
                baseEvents.insert(baseEvents.end(), std::make_move_iterator(fetched.list.begin()),
                                  std::make_move_iterator(fetched.list.end()));
            }

            cache::CalendarRepository repository{m_connection};
            if (const auto cacheError =
                    repository.replaceCalendars(accountId, calendars.state, calendars.list))
                co_return error(CalendarServiceErrorCode::Cache, cacheError->message);
            cache::CalendarWindow window{.accountId = accountId,
                                         .start = interval.start,
                                         .end = interval.end,
                                         .displayTimeZone = displayTimeZone,
                                         .queryState = query.queryState,
                                         .events = std::move(baseEvents),
                                         .occurrences = std::move(occurrences)};
            if (const auto cacheError = repository.reconcileWindow(window))
                co_return error(CalendarServiceErrorCode::Cache, cacheError->message);
            ++summary.accountCount;
            summary.eventCount += window.occurrences.size();
        }
        co_return summary;
    }

    QCoro::Task<CalendarMutationResult> CalendarService::create(LiveConnectionSettings settings,
                                                                std::string ownerAccountId,
                                                                CreateEventCommand command)
    {
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
        co_return co_await mutate(std::move(settings), std::move(ownerAccountId),
                                  std::move(request), std::move(calendarIds));
    }

    QCoro::Task<CalendarMutationResult> CalendarService::update(LiveConnectionSettings settings,
                                                                std::string ownerAccountId,
                                                                UpdateEventCommand command)
    {
        std::vector<std::string> calendarIds;
        for (const auto& [calendarId, present] : command.event.calendarIds)
            if (present)
                calendarIds.push_back(calendarId);
        const auto eventId = command.event.id;
        api::CalendarEventSetRequest request{.accountId = command.accountId,
                                             .ifInState = command.ifInState,
                                             .create = {},
                                             .update = {{eventId, std::move(command.event)}},
                                             .destroy = {},
                                             .sendSchedulingMessages = true};
        co_return co_await mutate(std::move(settings), std::move(ownerAccountId),
                                  std::move(request), std::move(calendarIds));
    }

    QCoro::Task<CalendarMutationResult> CalendarService::remove(LiveConnectionSettings settings,
                                                                std::string ownerAccountId,
                                                                DeleteEventCommand command)
    {
        api::CalendarEventSetRequest request{.accountId = command.accountId,
                                             .ifInState = command.ifInState,
                                             .create = {},
                                             .update = {},
                                             .destroy = {command.eventId},
                                             .sendSchedulingMessages = true};
        co_return co_await mutate(std::move(settings), std::move(ownerAccountId),
                                  std::move(request), std::move(command.calendarIds));
    }

    QCoro::Task<CalendarMutationResult>
    CalendarService::mutate(LiveConnectionSettings settings, std::string ownerAccountId,
                            api::CalendarEventSetRequest request,
                            std::vector<std::string> calendarIds)
    {
        const auto sessionResult = loadSession(m_connection, ownerAccountId);
        if (const auto* serviceError = std::get_if<CalendarServiceError>(&sessionResult))
            co_return *serviceError;
        const auto& session = std::get<api::Session>(sessionResult);
        const auto account = session.accounts.find(request.accountId);
        if (account == session.accounts.end() || !account->second.accountCapabilities.calendars)
            co_return error(
                CalendarServiceErrorCode::Capability,
                QStringLiteral("This account does not support JMAP Calendars draft-26."));
        cache::CalendarRepository repository{m_connection};
        const auto calendarsResult = repository.listCalendars(request.accountId);
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&calendarsResult))
            co_return error(CalendarServiceErrorCode::Cache, cacheError->message);
        if (!writable(std::get<std::vector<Calendar>>(calendarsResult), calendarIds))
            co_return error(CalendarServiceErrorCode::Permission,
                            QStringLiteral("You do not have permission to modify this event."));
        const auto method = api::calendarEventSet(request);
        if (!method)
            co_return error(CalendarServiceErrorCode::Validation,
                            QStringLiteral("Unable to serialize the calendar change."));
        api::RequestBuilder builder;
        builder.useCore().useCapability(std::string{api::calendarsCapabilityUri});
        const auto handle = builder.call(*method, "calendar-event-set");
        api::MethodCaller caller{m_methodTransport};
        const auto result =
            co_await caller.call(context(settings, session, request.accountId), builder);
        const auto* envelope = std::get_if<api::ResponseEnvelope>(&result);
        if (!envelope)
            co_return callError(result);
        const auto read = api::ResponseReader{*envelope}.require(handle);
        if (const auto* readError = std::get_if<api::ResponseReaderError>(&read))
            co_return responseError(*readError);
        const auto& response = std::get<api::CalendarEventSetResponse>(read);
        const auto* setError = !response.notCreated.empty()   ? &response.notCreated.begin()->second
                               : !response.notUpdated.empty() ? &response.notUpdated.begin()->second
                               : !response.notDestroyed.empty()
                                   ? &response.notDestroyed.begin()->second
                                   : nullptr;
        if (setError)
        {
            if (setError->type == api::CalendarSetErrorType::StateMismatch)
                co_return error(
                    CalendarServiceErrorCode::StaleState,
                    QStringLiteral("The event changed on the server. Refresh and try again."));
            if (setError->type == api::CalendarSetErrorType::Forbidden)
                co_return error(CalendarServiceErrorCode::Permission,
                                QStringLiteral("The server denied this calendar operation."));
            if (setError->type == api::CalendarSetErrorType::NoSupportedScheduleMethods)
                co_return error(
                    CalendarServiceErrorCode::Scheduling,
                    QStringLiteral("The server cannot schedule with one or more attendees."));
            co_return error(CalendarServiceErrorCode::Protocol,
                            QString::fromStdString(setError->description.value_or(
                                "The server rejected the calendar change.")));
        }
        co_return CommittedMutation{.accountId = request.accountId,
                                    .newState = response.newState,
                                    .createdId = response.created.empty()
                                                     ? std::nullopt
                                                     : response.created.begin()->second.id};
    }
} // namespace javelin::jmap::calendar
