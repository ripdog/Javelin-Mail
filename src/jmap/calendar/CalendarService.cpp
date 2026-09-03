#include "jmap/calendar/CalendarCacheReader.h"
#include "jmap/calendar/CalendarMutationEngine.h"
#include "jmap/calendar/CalendarProtocolClient.h"
#include "jmap/calendar/CalendarSyncEngine.h"

#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/MethodCaller.h"
#include "jmap/api/PatchObject.h"
#include "jmap/api/RequestBuilder.h"
#include "jmap/api/ResponseReader.h"

#include "jmap/auth/Auth.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/calendar/CalendarColor.h"
#include "jmap/calendar/CalendarEventEditing.h"
#include "jmap/calendar/CalendarMutationJournal.h"
#include "jmap/sync/ConsistencyDomain.h"
#include "jmap/sync/MutationJournal.h"
#include <QSqlQuery>
#include <QTimeZone>
#include <QUuid>

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
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

        [[nodiscard]] std::variant<VisibleInterval, OperationError>
        boundExpandedInterval(const VisibleInterval& requested, const std::string_view maximum)
        {
            static const QRegularExpression durationExpression{
                QStringLiteral("^P(?:(\\d+)Y)?(?:(\\d+)M)?(?:(\\d+)W)?(?:(\\d+)D)?"
                               "(?:T(?:(\\d+)H)?(?:(\\d+)M)?(?:(\\d+(?:\\.\\d+)?)S)?)?$")};
            const auto match =
                durationExpression.match(QString::fromStdString(std::string{maximum}));
            if (!match.hasMatch())
                return error(OperationErrorCode::ProtocolViolation,
                             QStringLiteral("The server advertised an invalid maximum recurrence "
                                            "expansion duration."));

            auto start =
                QDateTime::fromString(QString::fromStdString(requested.start.value), Qt::ISODate);
            auto requestedEnd =
                QDateTime::fromString(QString::fromStdString(requested.end.value), Qt::ISODate);
            if (!start.isValid() || !requestedEnd.isValid())
                return error(
                    OperationErrorCode::InvalidRequest,
                    QStringLiteral("The reminder horizon has an invalid date-time range."));
            start = QDateTime{start.date(), start.time(), QTimeZone::UTC};
            requestedEnd = QDateTime{requestedEnd.date(), requestedEnd.time(), QTimeZone::UTC};
            if (!start.isValid() || !requestedEnd.isValid())
                return error(
                    OperationErrorCode::InvalidRequest,
                    QStringLiteral("The reminder horizon has an invalid date-time range."));

            bool yearsValid = true;
            bool monthsValid = true;
            const int years =
                match.captured(1).isEmpty() ? 0 : match.captured(1).toInt(&yearsValid);
            const int months =
                match.captured(2).isEmpty() ? 0 : match.captured(2).toInt(&monthsValid);
            if (!yearsValid || !monthsValid)
                return error(OperationErrorCode::ProtocolViolation,
                             QStringLiteral("The server advertised an oversized maximum recurrence "
                                            "expansion duration."));
            const auto component = [&match](const int index) -> qint64
            { return match.captured(index).isEmpty() ? 0 : match.captured(index).toLongLong(); };
            start = start.addYears(years);
            start = start.addMonths(months);
            start = start.addDays(component(3) * 7 + component(4));
            start = start.addSecs(component(5) * 3600 + component(6) * 60);
            if (!match.captured(7).isEmpty())
                start = start.addMSecs(qRound64(match.captured(7).toDouble() * 1000.0));
            if (!start.isValid() ||
                start <= QDateTime::fromString(QString::fromStdString(requested.start.value),
                                               Qt::ISODate))
                return error(OperationErrorCode::ProtocolViolation,
                             QStringLiteral("The server advertised an unusable maximum recurrence "
                                            "expansion duration."));
            if (requestedEnd <= start)
                return requested;

            return VisibleInterval{
                .start = requested.start,
                .end = {.value =
                            start.toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss")).toStdString()},
            };
        }

        [[nodiscard]] std::optional<std::optional<std::string>>
        calendarColorMutationPayload(const std::string_view payload)
        {
            if (payload == "null")
                return std::optional<std::optional<std::string>>{std::in_place, std::nullopt};
            if (payload.size() < 2 || payload.front() != '"' || payload.back() != '"')
                return std::nullopt;
            auto color = std::string{payload.substr(1, payload.size() - 2)};
            if (!isValidCalendarColor(color))
                return std::nullopt;
            return std::optional<std::optional<std::string>>{std::in_place, std::move(color)};
        }

        using DefaultAlertMap = std::unordered_map<std::string, Alert>;
        using DefaultAlertPair = std::pair<DefaultAlertMap, DefaultAlertMap>;

        QJsonObject defaultAlertMapJson(const DefaultAlertMap& alerts)
        {
            QJsonObject result;
            for (const auto& [id, alert] : alerts)
            {
                QJsonObject value;
                value.insert(QStringLiteral("action"), QString::fromStdString(alert.action));
                value.insert(QStringLiteral("triggerKind"), static_cast<int>(alert.triggerKind));
                value.insert(QStringLiteral("relativeTo"),
                             QString::fromStdString(alert.relativeTo));
                if (alert.offset)
                    value.insert(QStringLiteral("offset"),
                                 QString::fromStdString(alert.offset->value));
                if (alert.when)
                    value.insert(QStringLiteral("when"), QString::fromStdString(alert.when->value));
                if (alert.acknowledged)
                    value.insert(QStringLiteral("acknowledged"),
                                 QString::fromStdString(alert.acknowledged->value));
                result.insert(QString::fromStdString(id), value);
            }
            return result;
        }

        std::string defaultAlertsMutationPayload(const DefaultAlertMap& withTime,
                                                 const DefaultAlertMap& withoutTime)
        {
            QJsonObject root;
            root.insert(QStringLiteral("withTime"), defaultAlertMapJson(withTime));
            root.insert(QStringLiteral("withoutTime"), defaultAlertMapJson(withoutTime));
            return QJsonDocument{root}.toJson(QJsonDocument::Compact).toStdString();
        }

        std::optional<DefaultAlertMap> defaultAlertMapFromJson(const QJsonValue& value)
        {
            if (!value.isObject())
                return std::nullopt;
            DefaultAlertMap result;
            const auto object = value.toObject();
            result.reserve(static_cast<std::size_t>(object.size()));
            for (auto it = object.begin(); it != object.end(); ++it)
            {
                if (!it.value().isObject())
                    return std::nullopt;
                const auto encoded = it.value().toObject();
                const auto action = encoded.value(QStringLiteral("action"));
                const auto triggerKind = encoded.value(QStringLiteral("triggerKind"));
                const auto relativeTo = encoded.value(QStringLiteral("relativeTo"));
                if (!action.isString() || !triggerKind.isDouble() || !relativeTo.isString())
                    return std::nullopt;
                Alert alert{
                    .id = it.key().toStdString(),
                    .action = action.toString().toStdString(),
                    .triggerKind = static_cast<AlertTriggerKind>(triggerKind.toInt()),
                    .relativeTo = relativeTo.toString().toStdString(),
                    .offset = std::nullopt,
                    .when = std::nullopt,
                    .acknowledged = std::nullopt,
                };
                const auto offset = encoded.value(QStringLiteral("offset"));
                if (offset.isString())
                    alert.offset = Duration{.value = offset.toString().toStdString()};
                const auto when = encoded.value(QStringLiteral("when"));
                if (when.isString())
                    alert.when = UtcInstant{.value = when.toString().toStdString()};
                const auto acknowledged = encoded.value(QStringLiteral("acknowledged"));
                if (acknowledged.isString())
                    alert.acknowledged = UtcInstant{.value = acknowledged.toString().toStdString()};
                result.emplace(alert.id, std::move(alert));
            }
            return result;
        }

        std::optional<DefaultAlertPair>
        calendarDefaultAlertsMutationPayload(const std::string_view payload)
        {
            QJsonParseError parseError;
            const auto document = QJsonDocument::fromJson(
                QByteArray{payload.data(), static_cast<qsizetype>(payload.size())}, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject())
                return std::nullopt;
            const auto root = document.object();
            auto withTime = defaultAlertMapFromJson(root.value(QStringLiteral("withTime")));
            auto withoutTime = defaultAlertMapFromJson(root.value(QStringLiteral("withoutTime")));
            if (!withTime || !withoutTime)
                return std::nullopt;
            return DefaultAlertPair{std::move(*withTime), std::move(*withoutTime)};
        }

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
                    .apiUrl = session.apiUrl,
                    .requestLimits = api::coreRequestLimits(session)};
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
                "id",
                "baseEventId",
                "recurrenceId",
                "uid",
                "calendarIds",
                "title",
                "description",
                "locations",
                "start",
                "duration",
                "timeZone",
                "showWithoutTime",
                "isDraft",
                "isOrigin",
                "status",
                "organizerCalendarAddress",
                "useDefaultAlerts",
                "alerts",
                "utcStart",
                "utcEnd",
                "recurrenceRule",
                "recurrenceOverrides",
                "participants",
            };
            return properties;
        }

        template <typename IsCurrent>
        QCoro::Task<BatchedCalendarEvents> getCalendarEventsBatched(
            CalendarProtocolClient& protocolClient, const LiveConnectionSettings& settings,
            const api::Session& session, const std::string& accountId,
            const std::vector<std::string>& ids, const TimeZoneId& displayTimeZone,
            const std::size_t batchLimit, const std::string_view callId, IsCurrent isCurrent)
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
                const auto result = co_await protocolClient.call(
                    context(settings, session, accountId), std::move(builder));
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

        bool eventWritable(const std::vector<calendar::Calendar>& calendars,
                           const std::vector<std::string>& calendarIds, const CalendarEvent& event,
                           const std::string_view configuredAddress)
        {
            std::unordered_set<std::string> required{calendarIds.begin(), calendarIds.end()};
            for (const auto& item : calendars)
            {
                if (!required.erase(item.id))
                    continue;
                if (!eventEditableWithRights(event, item.myRights, configuredAddress))
                    return false;
            }
            return required.empty();
        }

        bool privateUpdatable(const std::vector<calendar::Calendar>& calendars,
                              const std::vector<std::string>& calendarIds)
        {
            std::unordered_set<std::string> required{calendarIds.begin(), calendarIds.end()};
            for (const auto& item : calendars)
            {
                if (!required.erase(item.id))
                    continue;
                if (!item.myRights.mayUpdatePrivate)
                    return false;
            }
            return required.empty();
        }

        bool rsvpable(const std::vector<calendar::Calendar>& calendars,
                      const std::vector<std::string>& calendarIds)
        {
            std::unordered_set<std::string> required{calendarIds.begin(), calendarIds.end()};
            for (const auto& item : calendars)
            {
                if (!required.erase(item.id))
                    continue;
                if (!item.myRights.mayRSVP)
                    return false;
            }
            return required.empty();
        }

        [[nodiscard]] std::optional<std::size_t>
        matchingParticipantIndex(const CalendarEvent& event,
                                 const std::vector<ParticipantIdentity>& identities)
        {
            const auto matchesIdentity =
                [&event](const ParticipantIdentity& identity) -> std::optional<std::size_t>
            { return participantIndexForAddress(event, identity.calendarAddress); };
            for (const auto& identity : identities)
                if (identity.isDefault)
                    if (const auto index = matchesIdentity(identity))
                        return index;
            for (const auto& identity : identities)
                if (const auto index = matchesIdentity(identity))
                    return index;
            return std::nullopt;
        }

        [[nodiscard]] bool onlyPerUserPropertiesChanged(const CalendarEvent& before,
                                                        const CalendarEvent& after)
        {
            auto comparableBefore = before;
            auto comparableAfter = after;
            comparableBefore.useDefaultAlerts = comparableAfter.useDefaultAlerts;
            comparableBefore.alerts = comparableAfter.alerts;
            return comparableBefore == comparableAfter;
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

        [[nodiscard]] bool isStateMismatch(const OperationError& operationError)
        {
            return operationError.code == OperationErrorCode::Conflict &&
                   operationError.protocolType == "stateMismatch";
        }

        [[nodiscard]] std::variant<CalendarEvent, OperationError>
        rebaseCalendarEventUpdate(const CalendarEvent& previous, const CalendarEvent& requested,
                                  const CalendarEvent& refreshed)
        {
            const auto previousDocument = api::serializeCalendarEventDocument(previous);
            const auto requestedDocument = api::serializeCalendarEventDocument(requested);
            const auto refreshedDocument = api::serializeCalendarEventDocument(refreshed);
            if (!previousDocument || !requestedDocument || !refreshedDocument)
                return error(OperationErrorCode::ProtocolViolation,
                             QStringLiteral("Unable to serialize a calendar event while resolving "
                                            "a concurrent server change."));
            const auto patch = api::makePatchObject(*previousDocument, *requestedDocument);
            const auto* patchDocument = std::get_if<std::string>(&patch);
            if (patchDocument == nullptr)
                return error(OperationErrorCode::ProtocolViolation,
                             QStringLiteral("Unable to preserve the requested calendar edit after "
                                            "a concurrent server change."));
            const auto rebased = api::applyPatchObject(*refreshedDocument, *patchDocument);
            const auto* rebasedDocument = std::get_if<std::string>(&rebased);
            if (rebasedDocument == nullptr)
                return error(OperationErrorCode::Conflict,
                             QStringLiteral("The event changed on the server in a way that "
                                            "conflicts with this edit."));
            const auto parsed =
                api::parseCalendarEventDocument(refreshed.accountId, *rebasedDocument);
            if (!parsed.ok() || !parsed.value)
                return error(OperationErrorCode::ProtocolViolation,
                             QStringLiteral("Unable to decode the rebased calendar edit."));
            auto event = *parsed.value;
            event.accountId = refreshed.accountId;
            event.id = refreshed.id;
            return event;
        }

        [[nodiscard]] std::optional<CalendarRangeMaterialization>
        recoveryMaterializationForEvent(const CalendarEvent& event)
        {
            const auto start =
                QDateTime::fromString(QString::fromStdString(event.start.value), Qt::ISODate);
            if (!start.isValid())
                return std::nullopt;
            const auto firstDate = start.date().addDays(-21);
            const auto lastDate = start.date().addDays(22);
            auto displayTimeZone = event.timeZone.value_or(TimeZoneId{
                .value = QString::fromUtf8(QTimeZone::systemTimeZoneId()).toStdString()});
            if (displayTimeZone.value.empty())
                displayTimeZone.value = "UTC";
            return CalendarRangeMaterialization{
                .interval =
                    {
                        .start = {.value =
                                      firstDate.toString(Qt::ISODate).toStdString() + "T00:00:00"},
                        .end = {.value =
                                    lastDate.toString(Qt::ISODate).toStdString() + "T00:00:00"},
                    },
                .displayTimeZone = std::move(displayTimeZone),
            };
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

        [[nodiscard]] bool timingInputsChanged(const CalendarEvent& previous,
                                               const CalendarEvent& current)
        {
            return previous.start != current.start || previous.duration != current.duration ||
                   previous.timeZone != current.timeZone ||
                   previous.showWithoutTime != current.showWithoutTime;
        }

        [[nodiscard]] bool occurrenceMaterializationInputsChanged(const CalendarEvent& previous,
                                                                  const CalendarEvent& current)
        {
            if (previous.uid != current.uid || timingInputsChanged(previous, current) ||
                previous.recurrenceRule != current.recurrenceRule)
                return true;

            const auto changesOccurrence = [](const RecurrenceOverride& override)
            {
                return override.excluded || override.start.has_value() ||
                       override.duration.has_value();
            };
            for (const auto& [recurrenceId, previousOverride] : previous.recurrenceOverrides)
            {
                const auto currentOverride = current.recurrenceOverrides.find(recurrenceId);
                if (currentOverride == current.recurrenceOverrides.end())
                {
                    if (changesOccurrence(previousOverride))
                        return true;
                    continue;
                }
                if (previousOverride.excluded != currentOverride->second.excluded ||
                    previousOverride.start != currentOverride->second.start ||
                    previousOverride.duration != currentOverride->second.duration)
                    return true;
            }
            for (const auto& [recurrenceId, currentOverride] : current.recurrenceOverrides)
            {
                if (!previous.recurrenceOverrides.contains(recurrenceId) &&
                    changesOccurrence(currentOverride))
                    return true;
            }
            return false;
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
            std::vector<Occurrence> rollbackOccurrences;
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
                if (base && timingInputsChanged(*base, projected))
                {
                    projected.utcStart.reset();
                    projected.utcEnd.reset();
                }
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
                // Expanded occurrence ids are server materialization. Keep the existing range
                // intact whenever an update crosses or remains inside recurring state.
                const bool recurrenceNeedsServerMaterialization =
                    projected.recurrenceRule || (base && base->recurrenceRule);
                if (!recurrenceNeedsServerMaterialization)
                    prepared.projectedOccurrences.push_back(projectedOccurrence(projected));
                prepared.projectedEvents.push_back(std::move(projected));
            }
            for (const auto& eventId : request.destroy)
            {
                const auto cached = repository.findEvent(request.accountId, eventId);
                if (const auto* cacheError = std::get_if<cache::DatabaseError>(&cached))
                    return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
                const auto& base = std::get<std::optional<CalendarEvent>>(cached);
                const auto occurrences =
                    repository.listEventOccurrences(request.accountId, eventId);
                if (const auto* cacheError = std::get_if<cache::DatabaseError>(&occurrences))
                    return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
                const auto& cachedOccurrences = std::get<std::vector<Occurrence>>(occurrences);
                prepared.rollbackOccurrences.insert(prepared.rollbackOccurrences.end(),
                                                    cachedOccurrences.begin(),
                                                    cachedOccurrences.end());
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
            std::unordered_set<std::string> suppressedExpansionIds;
            std::unordered_set<std::string> suppressedExpansionUids;
        };

        [[nodiscard]] std::variant<RebasedCalendarEvents, OperationError>
        rebaseCalendarEvents(std::vector<CalendarEvent> serverEvents,
                             const std::vector<CalendarMutationRecord>& mutations)
        {
            RebasedCalendarEvents result{
                .events = std::move(serverEvents),
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
                    // Absence from the visible-range snapshot is not proof that an ambiguous
                    // destroy succeeded: the event may simply have moved outside this range.
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
                else if (server != serverSnapshot.end() && mutation.baseDocument.has_value())
                {
                    auto previous = eventFromMutationDocument(mutation, *mutation.baseDocument,
                                                              mutation.objectId);
                    auto requested = eventFromMutationDocument(mutation, mutation.requestedDocument,
                                                               mutation.objectId);
                    if (const auto* operationError = std::get_if<OperationError>(&previous))
                        return *operationError;
                    if (const auto* operationError = std::get_if<OperationError>(&requested))
                        return *operationError;
                    auto rebased = rebaseCalendarEventUpdate(
                        std::get<CalendarEvent>(std::move(previous)),
                        std::get<CalendarEvent>(std::move(requested)), *server);
                    if (const auto* operationError = std::get_if<OperationError>(&rebased))
                        return *operationError;
                    visible = std::get<CalendarEvent>(std::move(rebased));
                    confirmed = sameDocument(visible, *server);
                }
                else if (server != serverSnapshot.end())
                    confirmed = sameDocument(visible, *server);
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

        struct ResolvedCalendarMutation
        {
            CalendarMutationRecord record;
            javelin::jmap::sync::MutationReconciliationDisposition disposition =
                javelin::jmap::sync::MutationReconciliationDisposition::Applied;
            std::optional<CalendarEvent> authoritativeEvent;
            std::vector<std::string> removedEventIds;
        };

        struct CalendarUnknownReconciliation
        {
            std::vector<CalendarMutationRecord> unresolved;
            std::vector<ResolvedCalendarMutation> resolved;
        };

        [[nodiscard]] QCoro::Task<std::variant<CalendarUnknownReconciliation, OperationError>>
        reconcileUnknownCalendarMutations(CalendarProtocolClient& protocolClient,
                                          const LiveConnectionSettings& settings,
                                          const std::string& ownerAccountId,
                                          const std::string& accountId,
                                          const std::string_view authoritativeState,
                                          const std::vector<CalendarMutationRecord>& mutations)
        {
            CalendarUnknownReconciliation result;
            result.unresolved.reserve(mutations.size());
            for (const auto& mutation : mutations)
            {
                if (mutation.status != javelin::jmap::sync::MutationStatus::Unknown)
                {
                    result.unresolved.push_back(mutation);
                    continue;
                }

                std::optional<std::string> eventId;
                std::string uid;
                if (mutation.kind == CalendarMutationKind::Create)
                {
                    const auto requested = eventFromMutationDocument(
                        mutation, mutation.requestedDocument, mutation.objectId);
                    if (const auto* operationError = std::get_if<OperationError>(&requested))
                        co_return *operationError;
                    uid = std::get<CalendarEvent>(requested).uid;
                    if (uid.empty())
                    {
                        result.unresolved.push_back(mutation);
                        continue;
                    }
                }
                else
                    eventId = mutation.objectId;

                auto authoritative = co_await protocolClient.getAuthoritativeEvent(
                    settings, ownerAccountId, accountId, eventId, uid);
                if (const auto* operationError = std::get_if<OperationError>(&authoritative))
                {
                    qCWarning(logCalendarService).noquote()
                        << "Unable to reconcile ambiguous CalendarEvent mutation"
                        << QString::fromStdString(accountId)
                        << QString::fromStdString(mutation.objectId) << operationError->message;
                    result.unresolved.push_back(mutation);
                    continue;
                }
                auto current = std::get<AuthoritativeCalendarEvent>(std::move(authoritative));
                if (current.state != authoritativeState)
                {
                    qCDebug(logCalendarService).noquote()
                        << "Defer ambiguous CalendarEvent reconciliation because collection state "
                           "advanced during refresh"
                        << QString::fromStdString(accountId)
                        << QString::fromStdString(mutation.objectId)
                        << QString::fromStdString(current.state)
                        << QString::fromStdString(std::string{authoritativeState});
                    result.unresolved.push_back(mutation);
                    continue;
                }

                if (!mutation.baseState.has_value() || *mutation.baseState == authoritativeState)
                {
                    // A lost response can race a request that is still completing on the server.
                    // Seeing the exact pre-dispatch state therefore is not proof of rejection.
                    // Keep the projection until the collection advances or the mutation can be
                    // correlated positively.
                    result.unresolved.push_back(mutation);
                    continue;
                }
                ResolvedCalendarMutation resolved{
                    .record = mutation,
                    .disposition = javelin::jmap::sync::MutationReconciliationDisposition::Applied,
                    .authoritativeEvent = std::nullopt,
                    .removedEventIds = {},
                };
                if (mutation.kind == CalendarMutationKind::Create)
                {
                    resolved.removedEventIds.push_back(mutation.objectId);
                    if (current.event.has_value())
                    {
                        resolved.disposition =
                            javelin::jmap::sync::MutationReconciliationDisposition::Applied;
                        resolved.authoritativeEvent = std::move(current.event);
                    }
                    else
                    {
                        resolved.disposition =
                            javelin::jmap::sync::MutationReconciliationDisposition::Superseded;
                    }
                }
                else if (mutation.kind == CalendarMutationKind::Destroy)
                {
                    if (!current.event.has_value())
                    {
                        resolved.disposition =
                            javelin::jmap::sync::MutationReconciliationDisposition::Applied;
                        resolved.removedEventIds.push_back(mutation.objectId);
                    }
                    else
                    {
                        resolved.disposition =
                            javelin::jmap::sync::MutationReconciliationDisposition::Superseded;
                        resolved.authoritativeEvent = std::move(current.event);
                    }
                }
                else
                {
                    if (!current.event.has_value())
                    {
                        resolved.disposition =
                            javelin::jmap::sync::MutationReconciliationDisposition::Superseded;
                        resolved.removedEventIds.push_back(mutation.objectId);
                    }
                    else
                    {
                        if (!mutation.baseDocument.has_value())
                        {
                            result.unresolved.push_back(mutation);
                            continue;
                        }
                        auto previous = eventFromMutationDocument(mutation, *mutation.baseDocument,
                                                                  mutation.objectId);
                        auto requested = eventFromMutationDocument(
                            mutation, mutation.requestedDocument, mutation.objectId);
                        if (const auto* operationError = std::get_if<OperationError>(&previous))
                            co_return *operationError;
                        if (const auto* operationError = std::get_if<OperationError>(&requested))
                            co_return *operationError;
                        auto rebased = rebaseCalendarEventUpdate(
                            std::get<CalendarEvent>(std::move(previous)),
                            std::get<CalendarEvent>(std::move(requested)), *current.event);
                        if (const auto* operationError = std::get_if<OperationError>(&rebased))
                            co_return *operationError;
                        const bool intentSatisfied = api::calendarEventWritablePropertiesEqual(
                            std::get<CalendarEvent>(rebased), *current.event);
                        resolved.disposition =
                            intentSatisfied
                                ? javelin::jmap::sync::MutationReconciliationDisposition::Applied
                                : javelin::jmap::sync::MutationReconciliationDisposition::
                                      Superseded;
                        resolved.authoritativeEvent = std::move(current.event);
                    }
                }
                result.resolved.push_back(std::move(resolved));
            }
            co_return result;
        }

        [[nodiscard]] std::variant<bool, OperationError> reconcileMutationMaterialization(
            cache::CalendarRepository& repository, cache::DatabaseTransaction& transaction,
            const std::string_view accountId, const std::string_view eventState,
            const CalendarRangeMaterialization& materialization,
            const api::CalendarEventQueryResponse& query,
            const api::CalendarEventGetResponse& expandedEvents,
            const std::span<const CalendarEvent> knownBaseEvents)
        {
            if (query.accountId != accountId || expandedEvents.accountId != accountId ||
                expandedEvents.state != eventState || query.position != 0 ||
                query.ids.size() != query.total.value_or(query.ids.size()) ||
                !expandedEvents.notFound.empty() || expandedEvents.list.size() != query.ids.size())
                return false;

            std::unordered_set<std::string> unaccountedIds;
            unaccountedIds.reserve(query.ids.size());
            for (const auto& id : query.ids)
            {
                if (!unaccountedIds.insert(id).second)
                    return false;
            }
            for (const auto& event : expandedEvents.list)
            {
                if (!unaccountedIds.erase(event.id))
                    return false;
            }
            if (!unaccountedIds.empty())
                return false;

            auto snapshot = repository.loadRangeSnapshot(accountId, materialization.interval.start,
                                                         materialization.interval.end,
                                                         materialization.displayTimeZone);
            if (const auto* cacheError = std::get_if<cache::DatabaseError>(&snapshot))
                return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            auto window = std::get<cache::CalendarWindow>(std::move(snapshot));
            window.queryState = query.queryState;
            window.eventState = std::string{eventState};

            for (const auto& baseEvent : knownBaseEvents)
            {
                const auto existing =
                    std::ranges::find(window.events, baseEvent.id, &CalendarEvent::id);
                if (existing == window.events.end())
                    window.events.push_back(baseEvent);
                else
                    *existing = baseEvent;
            }
            // The expanded range response is not a base-event snapshot. Only map it onto
            // base events already represented by the effective cache or supplied by the
            // accepted mutation. If the query contains an unrelated object that we cannot
            // safely associate with a known base, keep the old range and require a later
            // authoritative refresh rather than partially replacing the window.
            std::unordered_map<std::string, std::string> eventIdByUid;
            std::unordered_set<std::string> ambiguousUids;
            std::unordered_set<std::string> baseEventIds;
            for (const auto& event : window.events)
            {
                baseEventIds.insert(event.id);
                if (event.uid.empty() || ambiguousUids.contains(event.uid))
                    continue;
                const auto [existing, inserted] = eventIdByUid.emplace(event.uid, event.id);
                if (!inserted && existing->second != event.id)
                {
                    eventIdByUid.erase(event.uid);
                    ambiguousUids.insert(event.uid);
                }
            }

            std::vector<Occurrence> occurrences;
            occurrences.reserve(expandedEvents.list.size());
            for (const auto& expanded : expandedEvents.list)
            {
                std::optional<std::string> eventId;
                if (expanded.baseEventId.has_value() &&
                    baseEventIds.contains(*expanded.baseEventId))
                    eventId = expanded.baseEventId;
                else if (!expanded.recurrenceId.has_value() && baseEventIds.contains(expanded.id))
                    eventId = expanded.id;
                else if (!expanded.uid.empty())
                {
                    const auto base = eventIdByUid.find(expanded.uid);
                    if (base != eventIdByUid.end())
                        eventId = base->second;
                }
                if (!eventId.has_value())
                    return false;
                occurrences.push_back({
                    .accountId = std::string{accountId},
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
            window.occurrences = std::move(occurrences);
            if (const auto cacheError = repository.reconcileWindow(transaction, window))
                return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            return true;
        }

        [[nodiscard]] std::optional<OperationError> restoreCalendarMutations(
            cache::DatabaseConnection& connection, cache::CalendarRepository& repository,
            const std::vector<CalendarMutationRecord>& records, const std::string_view eventState,
            const std::optional<std::string_view> errorJson,
            const std::span<const Occurrence> rollbackOccurrences,
            const CalendarRangeMaterialization* materialization,
            const api::CalendarEventQueryResponse* materializedQuery,
            const api::CalendarEventGetResponse* materializedEvents)
        {
            auto transactionResult = javelin::jmap::sync::MutationProjectionTransaction::begin(
                connection, QStringLiteral("Reject CalendarEvent mutations"));
            if (const auto* cacheError = std::get_if<cache::DatabaseError>(&transactionResult))
                return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            auto transaction = std::get<javelin::jmap::sync::MutationProjectionTransaction>(
                std::move(transactionResult));
            std::vector<CalendarEvent> restoredEvents;
            std::vector<Occurrence> restoredOccurrences{rollbackOccurrences.begin(),
                                                        rollbackOccurrences.end()};
            std::unordered_set<std::string> occurrenceBackups;
            for (const auto& occurrence : rollbackOccurrences)
                occurrenceBackups.insert(occurrence.eventId);
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
                if (!event.recurrenceRule && !occurrenceBackups.contains(event.id))
                    restoredOccurrences.push_back(projectedOccurrence(event));
                restoredEvents.push_back(std::move(event));
            }
            if (const auto cacheError = repository.projectEvents(
                    transaction.cacheTransaction(), records.front().accountId, eventState,
                    restoredEvents, restoredOccurrences, removedIds))
                return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            if (materialization != nullptr && materializedQuery != nullptr &&
                materializedEvents != nullptr)
            {
                const auto reconciled = reconcileMutationMaterialization(
                    repository, transaction.cacheTransaction(), records.front().accountId,
                    eventState, *materialization, *materializedQuery, *materializedEvents,
                    restoredEvents);
                if (const auto* operationError = std::get_if<OperationError>(&reconciled))
                    return *operationError;
            }
            const std::array domains{javelin::jmap::sync::ConsistencyDomain{
                .accountId = records.front().accountId,
                .dataType = "CalendarEvent",
            }};
            if (const auto cacheError = transaction.advance(domains))
                return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            if (const auto cacheError = transaction.retireTerminal())
                return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            if (const auto cacheError = transaction.commit())
                return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            return std::nullopt;
        }

        [[nodiscard]] CalendarMutationResult
        acceptCalendarMutations(cache::DatabaseConnection& connection,
                                cache::CalendarRepository& repository,
                                const std::vector<CalendarMutationRecord>& records,
                                const api::CalendarEventSetResponse& response,
                                const std::span<const CalendarEvent> authoritativeUpdates,
                                const CalendarRangeMaterialization* materialization,
                                const api::CalendarEventQueryResponse* materializedQuery,
                                const api::CalendarEventGetResponse* materializedEvents)
        {
            bool materializationSafe = true;
            if (materialization != nullptr)
            {
                for (const auto& record : records)
                {
                    if (record.kind != CalendarMutationKind::Update ||
                        !record.projectedDocument.has_value())
                        continue;
                    auto projected = eventFromMutationDocument(record, *record.projectedDocument,
                                                               record.objectId);
                    if (const auto* operationError = std::get_if<OperationError>(&projected))
                        return *operationError;
                    const auto current = repository.findEvent(response.accountId, record.objectId);
                    if (const auto* cacheError = std::get_if<cache::DatabaseError>(&current))
                        return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
                    const auto& currentEvent = std::get<std::optional<CalendarEvent>>(current);
                    if (!currentEvent.has_value() ||
                        !api::calendarEventWritablePropertiesEqual(
                            *currentEvent, std::get<CalendarEvent>(projected)))
                        materializationSafe = false;
                }
            }

            bool existingWindowsRemainMaterialized = !records.empty();
            bool authoritativeRangeMaterializationRequired = false;
            for (const auto& record : records)
            {
                if (record.kind == CalendarMutationKind::Create &&
                    record.projectedDocument.has_value())
                {
                    auto projected = eventFromMutationDocument(record, *record.projectedDocument,
                                                               record.objectId);
                    if (const auto* operationError = std::get_if<OperationError>(&projected))
                        return *operationError;
                    const auto& projectedEvent = std::get<CalendarEvent>(projected);
                    authoritativeRangeMaterializationRequired =
                        authoritativeRangeMaterializationRequired ||
                        projectedEvent.recurrenceRule.has_value() ||
                        !projectedEvent.recurrenceOverrides.empty();
                }

                if (record.kind != CalendarMutationKind::Update || !record.baseDocument.has_value())
                {
                    existingWindowsRemainMaterialized = false;
                    continue;
                }
                auto previous =
                    eventFromMutationDocument(record, *record.baseDocument, record.objectId);
                if (const auto* operationError = std::get_if<OperationError>(&previous))
                    return *operationError;
                const auto current = repository.findEvent(response.accountId, record.objectId);
                if (const auto* cacheError = std::get_if<cache::DatabaseError>(&current))
                    return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
                const auto& currentEvent = std::get<std::optional<CalendarEvent>>(current);
                if (!currentEvent.has_value())
                {
                    existingWindowsRemainMaterialized = false;
                    continue;
                }
                if (occurrenceMaterializationInputsChanged(std::get<CalendarEvent>(previous),
                                                           *currentEvent))
                {
                    existingWindowsRemainMaterialized = false;
                    authoritativeRangeMaterializationRequired =
                        authoritativeRangeMaterializationRequired ||
                        currentEvent->recurrenceRule.has_value() ||
                        !currentEvent->recurrenceOverrides.empty();
                }
            }

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
                    bool recurrenceRemoved = false;
                    if (record.baseDocument.has_value() && record.projectedDocument.has_value())
                    {
                        auto previous = eventFromMutationDocument(record, *record.baseDocument,
                                                                  record.objectId);
                        if (const auto* operationError = std::get_if<OperationError>(&previous))
                            return *operationError;
                        auto projected = eventFromMutationDocument(
                            record, *record.projectedDocument, record.objectId);
                        if (const auto* operationError = std::get_if<OperationError>(&projected))
                            return *operationError;
                        recurrenceRemoved =
                            std::get<CalendarEvent>(previous).recurrenceRule.has_value() &&
                            !std::get<CalendarEvent>(projected).recurrenceRule.has_value();
                    }

                    const auto authoritative = std::ranges::find(
                        authoritativeUpdates, record.objectId, &CalendarEvent::id);
                    const bool hasAuthoritativeTiming = authoritative != authoritativeUpdates.end();
                    if (!hasAuthoritativeTiming && !recurrenceRemoved)
                        continue;
                    const auto current = repository.findEvent(response.accountId, record.objectId);
                    if (const auto* cacheError = std::get_if<cache::DatabaseError>(&current))
                        return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
                    const auto& currentEvent = std::get<std::optional<CalendarEvent>>(current);
                    if (!currentEvent)
                        continue;

                    auto acceptedEvent = *currentEvent;
                    const bool timingStillMatches =
                        hasAuthoritativeTiming &&
                        !timingInputsChanged(*currentEvent, *authoritative);
                    if (timingStillMatches)
                    {
                        acceptedEvent.utcStart = authoritative->utcStart;
                        acceptedEvent.utcEnd = authoritative->utcEnd;
                    }
                    if (!acceptedEvent.recurrenceRule && (timingStillMatches || recurrenceRemoved))
                        acceptedOccurrences.push_back(projectedOccurrence(acceptedEvent));
                    if (timingStillMatches)
                        acceptedEvents.push_back(std::move(acceptedEvent));
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
            if (existingWindowsRemainMaterialized)
            {
                if (const auto cacheError = repository.advanceEventWindows(
                        transaction.cacheTransaction(), response.accountId, response.oldState,
                        response.newState))
                    return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            }

            // The mutation engine owns the decision about whether recurrence expansion is needed.
            // Callers may provide a visible range, but a missing range cannot make a recurring
            // create or recurrence-affecting update complete: only an expanded authoritative query
            // can establish the resulting occurrence set. Recurrence removal remains locally
            // complete because the accepted projection collapses the series to its single base
            // occurrence.
            bool materializationComplete =
                materialization == nullptr ? !authoritativeRangeMaterializationRequired : false;
            if (materialization != nullptr && materializationSafe && materializedQuery != nullptr &&
                materializedEvents != nullptr)
            {
                const auto reconciled = reconcileMutationMaterialization(
                    repository, transaction.cacheTransaction(), response.accountId,
                    response.newState, *materialization, *materializedQuery, *materializedEvents,
                    acceptedEvents);
                if (const auto* operationError = std::get_if<OperationError>(&reconciled))
                    return *operationError;
                materializationComplete = std::get<bool>(reconciled);
            }
            if (const auto cacheError = transaction.retireTerminal())
                return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
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
                        .incompleteMaterialization = !materializationComplete,
                    },
            };
        }
    } // namespace

    CalendarCacheReader::CalendarCacheReader(cache::DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    CalendarProtocolClient::CalendarProtocolClient(cache::DatabaseConnection& connection,
                                                   api::JmapMethodTransport& methodTransport)
        : m_connection(connection), m_methodTransport(methodTransport)
    {
    }

    QCoro::Task<api::MethodCallerResult>
    CalendarProtocolClient::call(api::ApiRequestContext requestContext,
                                 api::RequestBuilder request) const
    {
        api::MethodCaller caller{m_methodTransport};
        co_return co_await caller.call(std::move(requestContext), std::move(request));
    }

    CalendarSyncEngine::CalendarSyncEngine(cache::DatabaseConnection& connection,
                                           CalendarProtocolClient& protocolClient)
        : m_connection(connection), m_protocolClient(protocolClient)
    {
    }

    CalendarMutationEngine::CalendarMutationEngine(cache::DatabaseConnection& connection,
                                                   CalendarProtocolClient& protocolClient,
                                                   CalendarSyncEngine& syncEngine,
                                                   CalendarReader& reader)
        : m_connection(connection), m_protocolClient(protocolClient), m_syncEngine(syncEngine),
          m_reader(reader)
    {
    }

    CalendarLoadResult CalendarCacheReader::loadCached(const std::string_view accountId,
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

    CalendarAccountsResult CalendarCacheReader::accounts() const
    {
        cache::CalendarRepository repository{m_connection};
        auto loaded = repository.listAccounts();
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&loaded))
            return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        return std::get<std::vector<cache::CalendarAccount>>(std::move(loaded));
    }

    CalendarListResult CalendarCacheReader::calendars(const std::string_view accountId) const
    {
        cache::CalendarRepository repository{m_connection};
        auto loaded = repository.listCalendars(accountId);
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&loaded))
            return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        return std::get<std::vector<Calendar>>(std::move(loaded));
    }

    ParticipantIdentityListResult
    CalendarCacheReader::participantIdentities(const std::string_view accountId) const
    {
        if (const auto validation = m_connection.validate())
            return error(OperationErrorCode::LocalStorageFailure, validation->message);
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT identity_id,name,calendar_address,is_default FROM "
            "calendar_participant_identities WHERE account_id=:account ORDER BY is_default DESC,"
            "identity_id"));
        query.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        if (!query.exec())
            return error(OperationErrorCode::LocalStorageFailure,
                         QStringLiteral("Read calendar participant identities: ") +
                             query.lastError().text());
        std::vector<ParticipantIdentity> result;
        while (query.next())
        {
            result.push_back({.id = query.value(0).toString().toStdString(),
                              .name = query.value(1).toString().toStdString(),
                              .calendarAddress = query.value(2).toString().toStdString(),
                              .isDefault = query.value(3).toBool()});
        }
        return result;
    }

    PendingCalendarInvitationsResult CalendarCacheReader::pendingInvitations() const
    {
        if (const auto validation = m_connection.validate())
            return error(OperationErrorCode::LocalStorageFailure, validation->message);
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT p.account_id,p.event_id,p.self_participant_id,p.event_document_json,"
            "COALESCE(a.owner_account_id,a.account_id),p.recurrence_id,p.display_recurrence_id,"
            "p.display_start,p.display_utc_start,"
            "(SELECT o.local_start FROM calendar_occurrences o WHERE o.account_id=p.account_id "
            "AND o.event_id=p.event_id AND substr(o.local_start,1,10)>=:today ORDER BY "
            "o.local_start LIMIT 1),"
            "(SELECT o.start_utc FROM calendar_occurrences o WHERE o.account_id=p.account_id AND "
            "o.event_id=p.event_id AND substr(o.local_start,1,10)>=:today ORDER BY o.local_start "
            "LIMIT 1),"
            "(SELECT o.recurrence_id FROM calendar_occurrences o WHERE o.account_id=p.account_id "
            "AND o.event_id=p.event_id AND substr(o.local_start,1,10)>=:today ORDER BY "
            "o.local_start LIMIT 1) FROM calendar_pending_invitations p JOIN accounts a ON "
            "a.account_id=p.account_id ORDER BY p.discovered_at,p.account_id,p.event_id,"
            "p.recurrence_id"));
        query.bindValue(QStringLiteral(":today"), QDate::currentDate().toString(Qt::ISODate));
        if (!query.exec())
            return error(OperationErrorCode::LocalStorageFailure,
                         QStringLiteral("Read pending calendar invitations: ") +
                             query.lastError().text());

        cache::CalendarRepository repository{m_connection};
        std::unordered_map<std::string, std::vector<Calendar>> calendarsByAccount;
        std::vector<PendingCalendarInvitation> result;
        while (query.next())
        {
            const auto accountId = query.value(0).toString().toStdString();
            const auto parsed =
                api::parseCalendarEventDocument(accountId, query.value(3).toString().toStdString());
            if (!parsed.ok())
                return error(OperationErrorCode::LocalStorageFailure,
                             QStringLiteral("Parse pending calendar invitation"));
            const auto& event = *parsed.value;
            const auto recurrenceText = query.value(5).toString();
            const auto recurrenceId =
                recurrenceText.isEmpty()
                    ? std::optional<LocalDateTime>{}
                    : std::optional<LocalDateTime>{{.value = recurrenceText.toStdString()}};
            std::optional<CalendarEvent> effectiveOccurrence;
            const CalendarEvent* effectiveEvent = &event;
            if (recurrenceId)
            {
                effectiveOccurrence = effectiveOccurrenceEvent(event, *recurrenceId);
                if (!effectiveOccurrence)
                    continue;
                effectiveEvent = &*effectiveOccurrence;
            }
            const auto selfParticipantId = query.value(2).toString().toStdString();
            const auto attendee =
                std::ranges::find(effectiveEvent->attendees, selfParticipantId, &Attendee::id);
            const auto organizer = [effectiveEvent]() -> std::string
            {
                if (effectiveEvent->organizerCalendarAddress)
                {
                    const auto participant = std::ranges::find_if(
                        effectiveEvent->attendees,
                        [effectiveEvent](const Attendee& value)
                        {
                            return value.calendarAddress ==
                                       *effectiveEvent->organizerCalendarAddress ||
                                   value.isOwner;
                        });
                    if (participant != effectiveEvent->attendees.end() &&
                        !participant->name.empty())
                        return participant->name;
                    return *effectiveEvent->organizerCalendarAddress;
                }
                const auto owner =
                    std::ranges::find(effectiveEvent->attendees, true, &Attendee::isOwner);
                return owner == effectiveEvent->attendees.end() ? std::string{} : owner->name;
            }();

            auto calendarsIt = calendarsByAccount.find(accountId);
            if (calendarsIt == calendarsByAccount.end())
            {
                auto listed = repository.listCalendars(accountId);
                if (const auto* cacheError = std::get_if<cache::DatabaseError>(&listed))
                    return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
                calendarsIt =
                    calendarsByAccount
                        .emplace(accountId, std::get<std::vector<Calendar>>(std::move(listed)))
                        .first;
            }
            bool hasMembership = false;
            bool rsvpAllowed = true;
            for (const auto& [calendarId, present] : event.calendarIds)
            {
                if (!present)
                    continue;
                hasMembership = true;
                const auto calendar =
                    std::ranges::find(calendarsIt->second, calendarId, &Calendar::id);
                if (calendar == calendarsIt->second.end() || !calendar->myRights.mayRSVP)
                    rsvpAllowed = false;
            }
            rsvpAllowed = rsvpAllowed && hasMembership;

            const auto displayTime =
                !query.value(7).isNull()
                    ? LocalDateTime{.value = query.value(7).toString().toStdString()}
                : !query.value(9).isNull()
                    ? LocalDateTime{.value = query.value(9).toString().toStdString()}
                    : effectiveEvent->start;
            const auto displayUtc =
                !query.value(8).isNull()
                    ? std::optional<UtcInstant>{{.value = query.value(8).toString().toStdString()}}
                : !query.value(10).isNull()
                    ? std::optional<UtcInstant>{{.value = query.value(10).toString().toStdString()}}
                    : effectiveEvent->utcStart;
            const auto displayRecurrenceId =
                !query.value(6).isNull()
                    ? std::optional<LocalDateTime>{{.value =
                                                        query.value(6).toString().toStdString()}}
                : !query.value(11).isNull()
                    ? std::optional<LocalDateTime>{{.value =
                                                        query.value(11).toString().toStdString()}}
                    : recurrenceId;
            result.push_back(PendingCalendarInvitation{
                .ownerAccountId = query.value(4).toString().toStdString(),
                .accountId = accountId,
                .eventId = query.value(1).toString().toStdString(),
                .title = effectiveEvent->title,
                .organizer = organizer,
                .displayTime = displayTime,
                .displayUtc = displayUtc,
                .recurrenceId = recurrenceId,
                .displayRecurrenceId = displayRecurrenceId,
                .allDay = effectiveEvent->showWithoutTime,
                .recurring = event.recurrenceRule.has_value(),
                .selfParticipantId = selfParticipantId,
                .participationStatus = attendee == effectiveEvent->attendees.end()
                                           ? std::string{}
                                           : attendee->participationStatus,
                .rsvpAllowed = rsvpAllowed,
            });
        }
        return result;
    }

    CalendarEventReadResult CalendarCacheReader::event(const std::string_view accountId,
                                                       const std::string_view eventId) const
    {
        cache::CalendarRepository repository{m_connection};
        auto loaded = repository.findEvent(accountId, eventId);
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&loaded))
            return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        return std::get<std::optional<CalendarEvent>>(std::move(loaded));
    }

    QCoro::Task<CalendarMutationResult> CalendarMutationEngine::createCalendar(
        LiveConnectionSettings settings, std::string ownerAccountId, CreateCalendarCommand command)
    {
        if (command.name.empty() || command.name.size() > 255)
            co_return error(OperationErrorCode::InvalidUserInput,
                            QStringLiteral("Calendar names must contain 1 to 255 UTF-8 bytes."));
        if (command.color.has_value() && !isValidCalendarColor(*command.color))
            co_return error(OperationErrorCode::InvalidUserInput,
                            QStringLiteral("Choose a valid calendar color."));
        const auto sessionResult = loadSession(m_connection, ownerAccountId);
        if (const auto* serviceError = std::get_if<OperationError>(&sessionResult))
            co_return *serviceError;
        const auto& session = std::get<api::Session>(sessionResult);
        const auto account = session.accounts.find(command.accountId);
        if (account == session.accounts.end() || !account->second.accountCapabilities.calendars)
            co_return error(OperationErrorCode::UnsupportedCapability,
                            QStringLiteral("This account does not support JMAP Calendars."));
        if (!account->second.accountCapabilities.calendars->mayCreateCalendar)
            co_return error(OperationErrorCode::PermissionDenied,
                            QStringLiteral("The server does not allow creating calendars here."));

        const auto suffix = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        Calendar projected{
            .accountId = command.accountId,
            .id = "pending-" + suffix,
            .name = std::move(command.name),
            .description = std::nullopt,
            .color = std::move(command.color),
            .sortOrder = 0,
            .isSubscribed = true,
            .isVisible = true,
            .isDefault = false,
            .timeZone = std::nullopt,
            .defaultAlertsWithTime = {},
            .defaultAlertsWithoutTime = {},
            .myRights =
                {
                    .mayReadFreeBusy = true,
                    .mayReadItems = true,
                    .mayWriteAll = true,
                    .mayWriteOwn = true,
                    .mayUpdatePrivate = true,
                    .mayRSVP = true,
                    .mayShare = true,
                    .mayDelete = true,
                },
        };
        api::CalendarSetRequest request{
            .accountId = command.accountId,
            .ifInState = std::nullopt,
            .create = {{"new-calendar", projected}},
            .update = {},
            .destroy = {},
            .onDestroyRemoveEvents = false,
            .onSuccessSetIsDefault = std::nullopt,
        };
        co_return co_await mutateCalendar(std::move(settings), session, std::move(request),
                                          std::move(projected), std::nullopt);
    }

    QCoro::Task<CalendarMutationResult> CalendarMutationEngine::deleteCalendar(
        LiveConnectionSettings settings, std::string ownerAccountId, DeleteCalendarCommand command)
    {
        const auto listed = m_reader.calendars(command.accountId);
        if (const auto* serviceError = std::get_if<OperationError>(&listed))
            co_return *serviceError;
        const auto& available = std::get<std::vector<Calendar>>(listed);
        const auto selected = std::ranges::find(available, command.calendarId, &Calendar::id);
        if (selected == available.end())
            co_return error(OperationErrorCode::InvalidRequest,
                            QStringLiteral("The selected calendar is no longer available."));
        if (!selected->myRights.mayDelete)
            co_return error(OperationErrorCode::PermissionDenied,
                            QStringLiteral("You do not have permission to delete this calendar."));
        const auto sessionResult = loadSession(m_connection, ownerAccountId);
        if (const auto* serviceError = std::get_if<OperationError>(&sessionResult))
            co_return *serviceError;
        const auto& session = std::get<api::Session>(sessionResult);
        const auto account = session.accounts.find(command.accountId);
        if (account == session.accounts.end() || !account->second.accountCapabilities.calendars)
            co_return error(OperationErrorCode::UnsupportedCapability,
                            QStringLiteral("This account does not support JMAP Calendars."));
        api::CalendarSetRequest request{
            .accountId = command.accountId,
            .ifInState = std::nullopt,
            .create = {},
            .update = {},
            .destroy = {command.calendarId},
            .onDestroyRemoveEvents = command.removeEvents,
            .onSuccessSetIsDefault = std::nullopt,
        };
        co_return co_await mutateCalendar(std::move(settings), session, std::move(request),
                                          std::nullopt, std::move(command.calendarId));
    }

    QCoro::Task<CalendarMutationResult> CalendarMutationEngine::mutateCalendar(
        LiveConnectionSettings settings, api::Session session, api::CalendarSetRequest request,
        std::optional<Calendar> projectedCalendar, std::optional<std::string> deletedCalendarId)
    {
        cache::CalendarRepository repository{m_connection};
        const auto stateResult = repository.stateToken(request.accountId, "Calendar");
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&stateResult))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        const auto& state = std::get<std::optional<std::string>>(stateResult);
        if (!state.has_value())
            co_return error(OperationErrorCode::InvalidRequest,
                            QStringLiteral("Refresh calendars before changing them."));
        request.ifInState = *state;
        const auto method = api::calendarSet(request);
        if (!method)
            co_return error(OperationErrorCode::InvalidRequest,
                            QStringLiteral("Unable to serialize the calendar change."));

        sync::MutationJournalRepository journal{m_connection};
        const sync::ConsistencyDomain domain{.accountId = request.accountId,
                                             .dataType = "Calendar"};
        const auto active = journal.listActive(domain);
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&active))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        if (!std::get<std::vector<sync::MutationRecord>>(active).empty())
            co_return error(OperationErrorCode::Conflict,
                            QStringLiteral("Another calendar change is still unresolved."));

        const auto mutationId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        const auto creationId = request.create.empty()
                                    ? std::optional<std::string>{}
                                    : std::optional{request.create.begin()->first};
        const sync::MutationRecord mutation{
            .mutationId = mutationId,
            .operationGroupId = std::nullopt,
            .domain = domain,
            .objectId = projectedCalendar ? projectedCalendar->id : *deletedCalendarId,
            .mutationKind = projectedCalendar ? "calendar_create" : "calendar_destroy",
            .status = sync::MutationStatus::Pending,
            .payloadJson = "{}",
            .baseState = *state,
            .acceptedState = std::nullopt,
            .errorJson = std::nullopt,
        };
        auto queueResult = sync::MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Queue Calendar mutation"));
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&queueResult))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        auto queue = std::get<sync::MutationProjectionTransaction>(std::move(queueResult));
        if (const auto cacheError = queue.append(mutation))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        const std::array domains{domain};
        if (const auto cacheError = queue.advance(domains))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        const auto projectionError =
            projectedCalendar
                ? repository.projectCalendarCreation(queue.cacheTransaction(), request.accountId,
                                                     *state, *projectedCalendar)
                : repository.projectCalendarDeletion(queue.cacheTransaction(), request.accountId,
                                                     *deletedCalendarId, mutationId);
        if (projectionError)
            co_return error(OperationErrorCode::LocalStorageFailure, projectionError->message);
        if (const auto cacheError = queue.commit())
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);

        const auto reject = [&](const QString& message) -> std::optional<OperationError>
        {
            auto result = sync::MutationProjectionTransaction::begin(
                m_connection, QStringLiteral("Reject Calendar mutation"));
            if (const auto* cacheError = std::get_if<cache::DatabaseError>(&result))
                return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            auto transaction = std::get<sync::MutationProjectionTransaction>(std::move(result));
            const auto description = message.toStdString();
            if (const auto cacheError = transaction.transition(
                    mutationId, sync::MutationStatus::Rejected, std::nullopt, description))
                return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            const auto restoreError =
                projectedCalendar
                    ? repository.removeProjectedCalendar(transaction.cacheTransaction(),
                                                         request.accountId, projectedCalendar->id)
                    : repository.clearCalendarDeletion(transaction.cacheTransaction(), mutationId);
            if (restoreError)
                return error(OperationErrorCode::LocalStorageFailure, restoreError->message);
            if (const auto cacheError = transaction.remove(mutationId))
                return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            if (const auto cacheError = transaction.commit())
                return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            return std::nullopt;
        };
        if (const auto cacheError = journal.transition(mutationId, sync::MutationStatus::InFlight))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);

        api::RequestBuilder builder;
        builder.useCore().useCapability(std::string{api::calendarsCapabilityUri});
        const auto handle = builder.call(*method, "calendar-set-manager");
        const auto result = co_await m_protocolClient.call(
            context(settings, session, request.accountId), std::move(builder));
        const auto* envelope = std::get_if<api::ResponseEnvelope>(&result);
        if (!envelope)
        {
            if (const auto cacheError =
                    journal.transition(mutationId, sync::MutationStatus::Unknown))
                co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            co_return callError(result);
        }
        const auto read = api::ResponseReader{*envelope}.require(handle);
        if (const auto* readError = std::get_if<api::ResponseReaderError>(&read))
        {
            if (!readError->methodError.has_value())
            {
                if (const auto cacheError =
                        journal.transition(mutationId, sync::MutationStatus::Unknown))
                    co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
                co_return responseError(*readError);
            }
            if (const auto restoreError = reject(QStringLiteral("Calendar/set rejected")))
                co_return *restoreError;
            co_return responseError(*readError);
        }
        const auto& response = std::get<api::CalendarSetResponse>(read);
        const auto* setError = !response.notCreated.empty() ? &response.notCreated.begin()->second
                               : !response.notDestroyed.empty()
                                   ? &response.notDestroyed.begin()->second
                                   : nullptr;
        if (setError)
        {
            const auto detail =
                QString::fromStdString(setError->description.value_or("Calendar/set rejected"));
            if (const auto restoreError = reject(detail))
                co_return *restoreError;
            if (setError->type == api::CalendarSetErrorType::StateMismatch)
                co_return error(
                    OperationErrorCode::Conflict,
                    QStringLiteral("Calendars changed on the server. Refresh and try again."));
            if (setError->type == api::CalendarSetErrorType::Forbidden)
                co_return error(OperationErrorCode::PermissionDenied,
                                QStringLiteral("The server denied this calendar change."));
            co_return error(OperationErrorCode::ProtocolViolation, detail);
        }

        std::optional<std::string> acceptedId;
        bool acceptedDefault = false;
        if (projectedCalendar)
        {
            const auto created = response.created.find(*creationId);
            if (created == response.created.end() || !created->second.id.has_value())
            {
                if (const auto cacheError =
                        journal.transition(mutationId, sync::MutationStatus::Unknown))
                    co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
                co_return error(OperationErrorCode::ProtocolViolation,
                                QStringLiteral("Calendar/set omitted the created calendar id."));
            }
            acceptedId = created->second.id;
            acceptedDefault = created->second.isDefault.value_or(false);
        }
        else if (std::ranges::find(response.destroyed, *deletedCalendarId) ==
                 response.destroyed.end())
        {
            if (const auto cacheError =
                    journal.transition(mutationId, sync::MutationStatus::Unknown))
                co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            co_return error(OperationErrorCode::ProtocolViolation,
                            QStringLiteral("Calendar/set omitted the deleted calendar."));
        }

        auto acceptResult = sync::MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Accept Calendar mutation"));
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&acceptResult))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        auto accept = std::get<sync::MutationProjectionTransaction>(std::move(acceptResult));
        if (const auto cacheError =
                accept.transition(mutationId, sync::MutationStatus::Accepted, response.newState))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        if (const auto cacheError = accept.advance(domains))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        if (projectedCalendar)
        {
            if (const auto cacheError = repository.acceptProjectedCalendar(
                    accept.cacheTransaction(), request.accountId, projectedCalendar->id,
                    *acceptedId, response.newState, acceptedDefault))
                co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        }
        else
        {
            if (const auto cacheError =
                    repository.clearCalendarDeletion(accept.cacheTransaction(), mutationId))
                co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            if (const auto cacheError = repository.removeProjectedCalendar(
                    accept.cacheTransaction(), request.accountId, *deletedCalendarId))
                co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            if (const auto cacheError = repository.applyCalendarDefaults(
                    accept.cacheTransaction(), request.accountId, response.newState, {}))
                co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        }
        if (const auto cacheError = accept.remove(mutationId))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        if (const auto cacheError = accept.commit())
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        co_return CommittedMutation{
            .accountId = std::move(request.accountId),
            .newState = response.newState,
            .createdId = std::move(acceptedId),
            .receipt =
                {
                    .domains = {{
                        .accountId = response.accountId,
                        .dataType = "Calendar",
                        .oldState = response.oldState,
                        .newState = response.newState,
                    }},
                    .acceptedObjectIds = {acceptedId.value_or(
                        deletedCalendarId.value_or(std::string{}))},
                    .rejectedObjectIds = {},
                    .affectedCacheViews = {},
                    .incompleteMaterialization = false,
                },
        };
    }

    QCoro::Task<AuthoritativeCalendarEventResult> CalendarProtocolClient::getAuthoritativeEvent(
        LiveConnectionSettings settings, std::string ownerAccountId, std::string accountId,
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
                co_await call(context(settings, session, accountId), std::move(builder));
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
            *this, settings, session, accountId, eventIds, TimeZoneId{.value = "Etc/UTC"},
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
    CalendarMutationEngine::setCalendarSubscribed(LiveConnectionSettings settings,
                                                  std::string ownerAccountId, std::string accountId,
                                                  std::string calendarId, const bool subscribed)
    {
        const auto listed = m_reader.calendars(accountId);
        if (const auto* serviceError = std::get_if<OperationError>(&listed))
            co_return *serviceError;
        const auto& available = std::get<std::vector<Calendar>>(listed);
        const auto selected = std::ranges::find(available, calendarId, &Calendar::id);
        if (selected == available.end())
            co_return error(OperationErrorCode::InvalidRequest,
                            QStringLiteral("The selected calendar is no longer available."));

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
                            QStringLiteral("Refresh calendars before changing subscriptions."));
        if (selected->isSubscribed == subscribed)
            co_return CommittedMutation{
                .accountId = std::move(accountId),
                .newState = *state,
                .createdId = std::nullopt,
                .receipt = {},
            };

        sync::MutationJournalRepository journal{m_connection};
        const sync::ConsistencyDomain domain{.accountId = accountId, .dataType = "Calendar"};
        const auto active = journal.listActive(domain);
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&active))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        if (!std::get<std::vector<sync::MutationRecord>>(active).empty())
            co_return error(OperationErrorCode::Conflict,
                            QStringLiteral("Another calendar change is still unresolved."));

        const auto method = api::calendarSet({
            .accountId = accountId,
            .ifInState = state,
            .create = {},
            .update = {{calendarId,
                        {.isSubscribed = subscribed,
                         .color = std::nullopt,
                         .defaultAlertsWithTime = std::nullopt,
                         .defaultAlertsWithoutTime = std::nullopt}}},
            .destroy = {},
            .onDestroyRemoveEvents = false,
            .onSuccessSetIsDefault = std::nullopt,
        });
        if (!method)
            co_return error(
                OperationErrorCode::InvalidRequest,
                QStringLiteral("Unable to serialize the calendar subscription change."));

        const sync::MutationRecord mutation{
            .mutationId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),
            .operationGroupId = std::nullopt,
            .domain = domain,
            .objectId = calendarId,
            .mutationKind = "calendar_set_subscribed",
            .status = sync::MutationStatus::Pending,
            .payloadJson = subscribed ? "true" : "false",
            .baseState = *state,
            .acceptedState = std::nullopt,
            .errorJson = std::nullopt,
        };
        auto queueResult = sync::MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Queue Calendar subscription mutation"));
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&queueResult))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        auto queue = std::get<sync::MutationProjectionTransaction>(std::move(queueResult));
        if (const auto cacheError = queue.append(mutation))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        const std::array domains{domain};
        if (const auto cacheError = queue.advance(domains))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        if (const auto cacheError = repository.applyCalendarSubscription(
                queue.cacheTransaction(), accountId, calendarId, *state, subscribed))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        if (const auto cacheError = queue.commit())
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);

        const auto transition = [&](const sync::MutationStatus status,
                                    const std::optional<std::string_view> acceptedState =
                                        std::nullopt) -> std::optional<OperationError>
        {
            auto result = sync::MutationProjectionTransaction::begin(
                m_connection, QStringLiteral("Transition Calendar subscription mutation"));
            if (const auto* cacheError = std::get_if<cache::DatabaseError>(&result))
                return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            auto transaction = std::get<sync::MutationProjectionTransaction>(std::move(result));
            if (const auto cacheError =
                    transaction.transition(mutation.mutationId, status, acceptedState))
                return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            if (status == sync::MutationStatus::Rejected)
            {
                if (const auto cacheError = repository.applyCalendarSubscription(
                        transaction.cacheTransaction(), accountId, calendarId, *state,
                        selected->isSubscribed))
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
        const auto handle = builder.call(*method, "calendar-set-subscription");
        const auto result = co_await m_protocolClient.call(context(settings, session, accountId),
                                                           std::move(builder));
        const auto* envelope = std::get_if<api::ResponseEnvelope>(&result);
        if (!envelope)
        {
            if (const auto transitionError = transition(sync::MutationStatus::Unknown))
                co_return *transitionError;
            co_return callError(result);
        }
        const auto read = api::ResponseReader{*envelope}.require(handle);
        if (const auto* readError = std::get_if<api::ResponseReaderError>(&read))
        {
            const auto status = readError->methodError.has_value() ? sync::MutationStatus::Rejected
                                                                   : sync::MutationStatus::Unknown;
            if (const auto transitionError = transition(status))
                co_return *transitionError;
            co_return responseError(*readError);
        }
        const auto& response = std::get<api::CalendarSetResponse>(read);
        if (const auto failed = response.notUpdated.find(calendarId);
            failed != response.notUpdated.end())
        {
            if (const auto transitionError = transition(sync::MutationStatus::Rejected))
                co_return *transitionError;
            if (failed->second.type == api::CalendarSetErrorType::StateMismatch)
                co_return error(
                    OperationErrorCode::Conflict,
                    QStringLiteral("Calendars changed on the server. Refresh and try again."));
            if (failed->second.type == api::CalendarSetErrorType::Forbidden)
                co_return error(OperationErrorCode::PermissionDenied,
                                QStringLiteral("The server denied this calendar subscription."));
            co_return error(OperationErrorCode::ProtocolViolation,
                            QString::fromStdString(failed->second.description.value_or(
                                "The calendar subscription update was rejected.")));
        }
        if (!response.updated.contains(calendarId))
        {
            if (const auto transitionError = transition(sync::MutationStatus::Unknown))
                co_return *transitionError;
            co_return error(OperationErrorCode::ProtocolViolation,
                            QStringLiteral("Calendar/set omitted the updated calendar."));
        }

        auto acceptResult = sync::MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Accept Calendar subscription mutation"));
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&acceptResult))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        auto accept = std::get<sync::MutationProjectionTransaction>(std::move(acceptResult));
        if (const auto cacheError = accept.transition(
                mutation.mutationId, sync::MutationStatus::Accepted, response.newState))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        if (const auto cacheError = accept.advance(domains))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        if (const auto cacheError = repository.applyCalendarSubscription(
                accept.cacheTransaction(), accountId, calendarId, response.newState, subscribed))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        if (const auto cacheError = accept.remove(mutation.mutationId))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        if (const auto cacheError = accept.commit())
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        co_return CommittedMutation{
            .accountId = std::move(accountId),
            .newState = response.newState,
            .createdId = std::nullopt,
            .receipt =
                {
                    .domains = {{
                        .accountId = response.accountId,
                        .dataType = "Calendar",
                        .oldState = response.oldState,
                        .newState = response.newState,
                    }},
                    .acceptedObjectIds = {std::move(calendarId)},
                    .rejectedObjectIds = {},
                    .affectedCacheViews = {},
                    .incompleteMaterialization = false,
                },
        };
    }

    QCoro::Task<CalendarMutationResult> CalendarMutationEngine::setCalendarColor(
        LiveConnectionSettings settings, std::string ownerAccountId, std::string accountId,
        std::string calendarId, std::optional<std::string> color)
    {
        if (color.has_value() && !isValidCalendarColor(*color))
            co_return error(OperationErrorCode::InvalidUserInput,
                            QStringLiteral("Choose a valid calendar color."));

        const auto listed = m_reader.calendars(accountId);
        if (const auto* serviceError = std::get_if<OperationError>(&listed))
            co_return *serviceError;
        const auto& available = std::get<std::vector<Calendar>>(listed);
        const auto selected = std::ranges::find(available, calendarId, &Calendar::id);
        if (selected == available.end())
            co_return error(OperationErrorCode::InvalidRequest,
                            QStringLiteral("The selected calendar is no longer available."));
        if (!selected->isSubscribed)
            co_return error(OperationErrorCode::PermissionDenied,
                            QStringLiteral("Subscribe to the calendar before changing its color."));

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
                            QStringLiteral("Refresh calendars before changing their color."));
        if (selected->color == color)
            co_return CommittedMutation{
                .accountId = std::move(accountId),
                .newState = *state,
                .createdId = std::nullopt,
                .receipt = {},
            };

        sync::MutationJournalRepository journal{m_connection};
        const sync::ConsistencyDomain domain{.accountId = accountId, .dataType = "Calendar"};
        const auto active = journal.listActive(domain);
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&active))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        if (!std::get<std::vector<sync::MutationRecord>>(active).empty())
            co_return error(OperationErrorCode::Conflict,
                            QStringLiteral("Another calendar change is still unresolved."));

        const std::optional<std::optional<std::string>> colorPatch{std::in_place, color};
        const auto method = api::calendarSet({
            .accountId = accountId,
            .ifInState = state,
            .create = {},
            .update = {{calendarId,
                        {.isSubscribed = std::nullopt,
                         .color = colorPatch,
                         .defaultAlertsWithTime = std::nullopt,
                         .defaultAlertsWithoutTime = std::nullopt}}},
            .destroy = {},
            .onDestroyRemoveEvents = false,
            .onSuccessSetIsDefault = std::nullopt,
        });
        if (!method)
            co_return error(OperationErrorCode::InvalidRequest,
                            QStringLiteral("Unable to serialize the calendar color change."));

        const sync::MutationRecord mutation{
            .mutationId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),
            .operationGroupId = std::nullopt,
            .domain = domain,
            .objectId = calendarId,
            .mutationKind = "calendar_set_color",
            .status = sync::MutationStatus::Pending,
            .payloadJson = color ? "\"" + *color + "\"" : "null",
            .baseState = *state,
            .acceptedState = std::nullopt,
            .errorJson = std::nullopt,
        };
        auto queueResult = sync::MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Queue Calendar color mutation"));
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&queueResult))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        auto queue = std::get<sync::MutationProjectionTransaction>(std::move(queueResult));
        if (const auto cacheError = queue.append(mutation))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        const std::array domains{domain};
        if (const auto cacheError = queue.advance(domains))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        if (const auto cacheError = repository.applyCalendarColor(
                queue.cacheTransaction(), accountId, calendarId, *state, color))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        if (const auto cacheError = queue.commit())
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);

        const auto transition = [&](const sync::MutationStatus status,
                                    const std::optional<std::string_view> acceptedState =
                                        std::nullopt) -> std::optional<OperationError>
        {
            auto result = sync::MutationProjectionTransaction::begin(
                m_connection, QStringLiteral("Transition Calendar color mutation"));
            if (const auto* cacheError = std::get_if<cache::DatabaseError>(&result))
                return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            auto transaction = std::get<sync::MutationProjectionTransaction>(std::move(result));
            if (const auto cacheError =
                    transaction.transition(mutation.mutationId, status, acceptedState))
                return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            if (status == sync::MutationStatus::Rejected)
            {
                if (const auto cacheError =
                        repository.applyCalendarColor(transaction.cacheTransaction(), accountId,
                                                      calendarId, *state, selected->color))
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
        const auto handle = builder.call(*method, "calendar-set-color");
        const auto result = co_await m_protocolClient.call(context(settings, session, accountId),
                                                           std::move(builder));
        const auto* envelope = std::get_if<api::ResponseEnvelope>(&result);
        if (!envelope)
        {
            if (const auto transitionError = transition(sync::MutationStatus::Unknown))
                co_return *transitionError;
            co_return callError(result);
        }
        const auto read = api::ResponseReader{*envelope}.require(handle);
        if (const auto* readError = std::get_if<api::ResponseReaderError>(&read))
        {
            const auto status = readError->methodError.has_value() ? sync::MutationStatus::Rejected
                                                                   : sync::MutationStatus::Unknown;
            if (const auto transitionError = transition(status))
                co_return *transitionError;
            co_return responseError(*readError);
        }
        const auto& response = std::get<api::CalendarSetResponse>(read);
        if (const auto failed = response.notUpdated.find(calendarId);
            failed != response.notUpdated.end())
        {
            if (const auto transitionError = transition(sync::MutationStatus::Rejected))
                co_return *transitionError;
            if (failed->second.type == api::CalendarSetErrorType::StateMismatch)
                co_return error(
                    OperationErrorCode::Conflict,
                    QStringLiteral("Calendars changed on the server. Refresh and try again."));
            if (failed->second.type == api::CalendarSetErrorType::Forbidden)
                co_return error(OperationErrorCode::PermissionDenied,
                                QStringLiteral("The server denied this calendar color change."));
            co_return error(OperationErrorCode::ProtocolViolation,
                            QString::fromStdString(failed->second.description.value_or(
                                "The calendar color update was rejected.")));
        }
        if (!response.updated.contains(calendarId))
        {
            if (const auto transitionError = transition(sync::MutationStatus::Unknown))
                co_return *transitionError;
            co_return error(OperationErrorCode::ProtocolViolation,
                            QStringLiteral("Calendar/set omitted the updated calendar."));
        }

        auto acceptResult = sync::MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Accept Calendar color mutation"));
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&acceptResult))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        auto accept = std::get<sync::MutationProjectionTransaction>(std::move(acceptResult));
        if (const auto cacheError = accept.transition(
                mutation.mutationId, sync::MutationStatus::Accepted, response.newState))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        if (const auto cacheError = accept.advance(domains))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        if (const auto cacheError = repository.applyCalendarColor(
                accept.cacheTransaction(), accountId, calendarId, response.newState, color))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        if (const auto cacheError = accept.remove(mutation.mutationId))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        if (const auto cacheError = accept.commit())
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        co_return CommittedMutation{
            .accountId = std::move(accountId),
            .newState = response.newState,
            .createdId = std::nullopt,
            .receipt =
                {
                    .domains = {{
                        .accountId = response.accountId,
                        .dataType = "Calendar",
                        .oldState = response.oldState,
                        .newState = response.newState,
                    }},
                    .acceptedObjectIds = {std::move(calendarId)},
                    .rejectedObjectIds = {},
                    .affectedCacheViews = {},
                    .incompleteMaterialization = false,
                },
        };
    }

    QCoro::Task<CalendarMutationResult> CalendarMutationEngine::setCalendarDefaultAlerts(
        LiveConnectionSettings settings, std::string ownerAccountId, std::string accountId,
        std::string calendarId, std::unordered_map<std::string, Alert> withTime,
        std::unordered_map<std::string, Alert> withoutTime)
    {
        const auto validDefaults = [](const auto& alerts)
        {
            return std::ranges::all_of(alerts,
                                       [](const auto& item)
                                       {
                                           const auto& [id, alert] = item;
                                           return !id.empty() && alert.id == id &&
                                                  alert.triggerKind == AlertTriggerKind::Offset &&
                                                  alert.offset.has_value() &&
                                                  !alert.when.has_value();
                                       });
        };
        if (!validDefaults(withTime) || !validDefaults(withoutTime))
            co_return error(OperationErrorCode::InvalidUserInput,
                            QStringLiteral("Calendar default notifications must be relative to the "
                                           "event start or end."));

        const auto listed = m_reader.calendars(accountId);
        if (const auto* serviceError = std::get_if<OperationError>(&listed))
            co_return *serviceError;
        const auto& available = std::get<std::vector<Calendar>>(listed);
        const auto selected = std::ranges::find(available, calendarId, &Calendar::id);
        if (selected == available.end())
            co_return error(OperationErrorCode::InvalidRequest,
                            QStringLiteral("The selected calendar is no longer available."));
        if (!selected->isSubscribed)
            co_return error(
                OperationErrorCode::PermissionDenied,
                QStringLiteral(
                    "Subscribe to the calendar before changing its default notifications."));

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
            co_return error(
                OperationErrorCode::InvalidRequest,
                QStringLiteral("Refresh calendars before changing default notifications."));
        if (selected->defaultAlertsWithTime == withTime &&
            selected->defaultAlertsWithoutTime == withoutTime)
            co_return CommittedMutation{
                .accountId = std::move(accountId),
                .newState = *state,
                .createdId = std::nullopt,
                .receipt = {},
            };

        sync::MutationJournalRepository journal{m_connection};
        const sync::ConsistencyDomain domain{.accountId = accountId, .dataType = "Calendar"};
        const auto active = journal.listActive(domain);
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&active))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        if (!std::get<std::vector<sync::MutationRecord>>(active).empty())
            co_return error(OperationErrorCode::Conflict,
                            QStringLiteral("Another calendar change is still unresolved."));

        const auto method = api::calendarSet({
            .accountId = accountId,
            .ifInState = state,
            .create = {},
            .update = {{calendarId,
                        {.isSubscribed = std::nullopt,
                         .color = std::nullopt,
                         .defaultAlertsWithTime = withTime,
                         .defaultAlertsWithoutTime = withoutTime}}},
            .destroy = {},
            .onDestroyRemoveEvents = false,
            .onSuccessSetIsDefault = std::nullopt,
        });
        if (!method)
            co_return error(
                OperationErrorCode::InvalidRequest,
                QStringLiteral("Unable to serialize the calendar default notification change."));

        const sync::MutationRecord mutation{
            .mutationId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),
            .operationGroupId = std::nullopt,
            .domain = domain,
            .objectId = calendarId,
            .mutationKind = "calendar_set_default_alerts",
            .status = sync::MutationStatus::Pending,
            .payloadJson = defaultAlertsMutationPayload(withTime, withoutTime),
            .baseState = *state,
            .acceptedState = std::nullopt,
            .errorJson = std::nullopt,
        };
        auto queueResult = sync::MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Queue Calendar default notification mutation"));
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&queueResult))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        auto queue = std::get<sync::MutationProjectionTransaction>(std::move(queueResult));
        if (const auto cacheError = queue.append(mutation))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        const std::array domains{domain};
        if (const auto cacheError = queue.advance(domains))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        if (const auto cacheError = repository.applyCalendarDefaultAlerts(
                queue.cacheTransaction(), accountId, calendarId, *state, withTime, withoutTime))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        if (const auto cacheError = queue.commit())
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);

        const auto markOutcomeUnknown = [](OperationError value)
        {
            value.outcomeUnknown = true;
            return value;
        };
        const auto transition = [&](const sync::MutationStatus status,
                                    const std::optional<std::string_view> acceptedState =
                                        std::nullopt) -> std::optional<OperationError>
        {
            auto result = sync::MutationProjectionTransaction::begin(
                m_connection, QStringLiteral("Transition Calendar default notification mutation"));
            if (const auto* cacheError = std::get_if<cache::DatabaseError>(&result))
                return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            auto transaction = std::get<sync::MutationProjectionTransaction>(std::move(result));
            if (const auto cacheError =
                    transaction.transition(mutation.mutationId, status, acceptedState))
                return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            if (status == sync::MutationStatus::Rejected)
            {
                if (const auto cacheError = repository.applyCalendarDefaultAlerts(
                        transaction.cacheTransaction(), accountId, calendarId, *state,
                        selected->defaultAlertsWithTime, selected->defaultAlertsWithoutTime))
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
        const auto handle = builder.call(*method, "calendar-set-default-alerts");
        const auto result = co_await m_protocolClient.call(context(settings, session, accountId),
                                                           std::move(builder));
        const auto* envelope = std::get_if<api::ResponseEnvelope>(&result);
        if (!envelope)
        {
            if (const auto transitionError = transition(sync::MutationStatus::Unknown))
                co_return markOutcomeUnknown(*transitionError);
            co_return markOutcomeUnknown(callError(result));
        }
        const auto read = api::ResponseReader{*envelope}.require(handle);
        if (const auto* readError = std::get_if<api::ResponseReaderError>(&read))
        {
            const auto status = readError->methodError.has_value() ? sync::MutationStatus::Rejected
                                                                   : sync::MutationStatus::Unknown;
            if (const auto transitionError = transition(status))
                co_return status == sync::MutationStatus::Unknown
                    ? markOutcomeUnknown(*transitionError)
                    : *transitionError;
            auto failure = responseError(*readError);
            if (status == sync::MutationStatus::Unknown)
                failure = markOutcomeUnknown(std::move(failure));
            co_return failure;
        }
        const auto& response = std::get<api::CalendarSetResponse>(read);
        if (const auto failed = response.notUpdated.find(calendarId);
            failed != response.notUpdated.end())
        {
            if (const auto transitionError = transition(sync::MutationStatus::Rejected))
                co_return *transitionError;
            if (failed->second.type == api::CalendarSetErrorType::StateMismatch)
                co_return error(
                    OperationErrorCode::Conflict,
                    QStringLiteral("Calendars changed on the server. Refresh and try again."));
            if (failed->second.type == api::CalendarSetErrorType::Forbidden)
                co_return error(
                    OperationErrorCode::PermissionDenied,
                    QStringLiteral("The server denied this default notification change."));
            co_return error(OperationErrorCode::ProtocolViolation,
                            QString::fromStdString(failed->second.description.value_or(
                                "The calendar default notification update was rejected.")));
        }
        if (!response.updated.contains(calendarId))
        {
            if (const auto transitionError = transition(sync::MutationStatus::Unknown))
                co_return markOutcomeUnknown(*transitionError);
            co_return markOutcomeUnknown(
                error(OperationErrorCode::ProtocolViolation,
                      QStringLiteral("Calendar/set omitted the updated calendar.")));
        }

        auto acceptResult = sync::MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Accept Calendar default notification mutation"));
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&acceptResult))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        auto accept = std::get<sync::MutationProjectionTransaction>(std::move(acceptResult));
        if (const auto cacheError = accept.transition(
                mutation.mutationId, sync::MutationStatus::Accepted, response.newState))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        if (const auto cacheError = accept.advance(domains))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        if (const auto cacheError = repository.applyCalendarDefaultAlerts(
                accept.cacheTransaction(), accountId, calendarId, response.newState, withTime,
                withoutTime))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        if (const auto cacheError = accept.remove(mutation.mutationId))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        if (const auto cacheError = accept.commit())
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        co_return CommittedMutation{
            .accountId = std::move(accountId),
            .newState = response.newState,
            .createdId = std::nullopt,
            .receipt =
                {
                    .domains = {{
                        .accountId = response.accountId,
                        .dataType = "Calendar",
                        .oldState = response.oldState,
                        .newState = response.newState,
                    }},
                    .acceptedObjectIds = {std::move(calendarId)},
                    .rejectedObjectIds = {},
                    .affectedCacheViews = {},
                    .incompleteMaterialization = false,
                },
        };
    }

    QCoro::Task<CalendarMutationResult>
    CalendarMutationEngine::setDefaultCalendar(LiveConnectionSettings settings,
                                               std::string ownerAccountId, std::string accountId,
                                               std::string calendarId)
    {
        const auto listed = m_reader.calendars(accountId);
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

        const auto method = api::calendarSet({.accountId = accountId,
                                              .ifInState = state,
                                              .create = {},
                                              .update = {},
                                              .destroy = {},
                                              .onDestroyRemoveEvents = false,
                                              .onSuccessSetIsDefault = calendarId});
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
        const auto result = co_await m_protocolClient.call(context(settings, session, accountId),
                                                           std::move(builder));
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
            accepted = selectedUpdate != response.updated.end() && selectedUpdate->second &&
                       selectedUpdate->second->isDefault.value_or(false);
            if (accepted)
            {
                acceptedState = response.newState;
                for (const auto& [id, update] : response.updated)
                    if (update && update->isDefault.has_value())
                        defaults.insert_or_assign(id, *update->isDefault);
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
            const auto verifyResult = co_await m_protocolClient.call(
                context(settings, session, accountId), std::move(verifyBuilder));
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
                defaults.insert_or_assign(calendar.id, calendar.isDefault);
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

    void CalendarSyncEngine::invalidateRefresh(const std::string_view ownerAccountId)
    {
        static_cast<void>(beginRefresh(ownerAccountId));
    }

    std::uint64_t CalendarSyncEngine::beginRefresh(const std::string_view ownerAccountId)
    {
        return ++m_refreshGenerations[std::string{ownerAccountId}];
    }

    bool CalendarSyncEngine::isCurrentRefresh(const std::string_view ownerAccountId,
                                              const std::uint64_t generation) const
    {
        const auto current = m_refreshGenerations.find(std::string{ownerAccountId});
        return current != m_refreshGenerations.end() && current->second == generation;
    }

    QCoro::Task<std::variant<bool, OperationError>>
    CalendarSyncEngine::refreshMetadata(LiveConnectionSettings settings, std::string ownerAccountId)
    {
        const auto sessionResult = loadSession(m_connection, ownerAccountId);
        if (const auto* serviceError = std::get_if<OperationError>(&sessionResult))
            co_return *serviceError;
        const auto& session = std::get<api::Session>(sessionResult);
        cache::CalendarRepository repository{m_connection};
        bool authoritative = true;
        for (const auto& [accountId, account] : session.accounts)
        {
            if (!account.accountCapabilities.calendars)
                continue;

            const auto calendarFenceResult = captureFence(m_connection, accountId, "Calendar");
            if (const auto* serviceError = std::get_if<OperationError>(&calendarFenceResult))
                co_return *serviceError;
            const auto calendarFence =
                std::get<javelin::jmap::sync::RefreshFence>(calendarFenceResult);
            const auto request = api::calendarGet({.accountId = accountId,
                                                   .ids = std::nullopt,
                                                   .idsReference = std::nullopt,
                                                   .properties = std::nullopt});
            if (!request)
                co_return error(
                    OperationErrorCode::InvalidRequest,
                    QStringLiteral("Unable to serialize the calendar metadata request."));
            api::RequestBuilder builder;
            builder.useCore().useCapability(std::string{api::calendarsCapabilityUri});
            const auto handle = builder.call(*request, "calendar-metadata-get");
            const auto result = co_await m_protocolClient.call(
                context(settings, session, accountId), std::move(builder));
            const auto* envelope = std::get_if<api::ResponseEnvelope>(&result);
            if (!envelope)
                co_return callError(result);
            const auto read = api::ResponseReader{*envelope}.require(handle);
            if (const auto* readError = std::get_if<api::ResponseReaderError>(&read))
                co_return responseError(*readError);
            const auto& calendars = std::get<api::CalendarGetResponse>(read);

            sync::MutationJournalRepository journal{m_connection};
            const auto active =
                journal.listActive({.accountId = accountId, .dataType = "Calendar"});
            if (const auto* cacheError = std::get_if<cache::DatabaseError>(&active))
                co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            if (!std::get<std::vector<sync::MutationRecord>>(active).empty())
            {
                authoritative = false;
                qCDebug(logCalendarService).noquote()
                    << "calendar metadata refresh kept active projection"
                    << QString::fromStdString(accountId);
                continue;
            }

            const auto current = fenceGenerationIsCurrent(m_connection, calendarFence);
            if (const auto* serviceError = std::get_if<OperationError>(&current))
                co_return *serviceError;
            if (!std::get<bool>(current))
            {
                authoritative = false;
                continue;
            }
            if (const auto cacheError =
                    repository.replaceCalendars(accountId, calendars.state, calendars.list))
                co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            qCDebug(logCalendarService).noquote()
                << "calendar metadata refresh" << QString::fromStdString(accountId) << "calendars"
                << calendars.list.size();
        }
        co_return authoritative;
    }

    QCoro::Task<CalendarRefreshResult>
    CalendarSyncEngine::refreshChanged(LiveConnectionSettings settings, std::string ownerAccountId,
                                       VisibleInterval interval, TimeZoneId displayTimeZone)
    {
        const auto generation = beginRefresh(ownerAccountId);
        const auto sessionResult = loadSession(m_connection, ownerAccountId);
        if (const auto* serviceError = std::get_if<OperationError>(&sessionResult))
            co_return *serviceError;
        const auto& session = std::get<api::Session>(sessionResult);
        cache::CalendarRepository repository{m_connection};
        RefreshedRange summary{.interval = interval,
                               .displayTimeZone = displayTimeZone,
                               .accountCount = 0,
                               .eventCount = 0,
                               .calendarMetadataAuthoritative = true,
                               .reconciledMutations = {}};
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

            const auto maxChanges = session.capabilities.coreDetails
                                        ? session.capabilities.coreDetails->maxObjectsInGet
                                        : std::nullopt;
            const auto calendarRequest = api::calendarChanges(
                {.accountId = accountId, .sinceState = *calendarState, .maxChanges = maxChanges});
            const auto eventRequest = api::calendarEventChanges(
                {.accountId = accountId, .sinceState = *eventState, .maxChanges = maxChanges});
            if (!calendarRequest || !eventRequest)
                co_return error(OperationErrorCode::InvalidRequest,
                                QStringLiteral("Unable to serialize calendar changes."));
            api::RequestBuilder builder;
            builder.useCore().useCapability(std::string{api::calendarsCapabilityUri});
            const auto calendarHandle = builder.call(*calendarRequest, "calendar-changes");
            const auto eventHandle = builder.call(*eventRequest, "calendar-event-changes");
            const auto result = co_await m_protocolClient.call(
                context(settings, session, accountId), std::move(builder));
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
            const bool calendarMetadataChanged = hasChanges(calendarChanges);
            // CalendarEvent/query filters are evaluated in the requested display timezone, while
            // CalendarEvent.start remains in the event's own timezone. A changed event therefore
            // cannot be placed into cached range windows by comparing local start/end strings.
            // Rematerialize the visible range from the server for every real event delta; retain
            // the incremental path only for collection-state advancement and Calendar metadata.
            if (hasChanges(eventChanges))
            {
                fullRefreshRequired = true;
                break;
            }
            std::optional<api::CalendarGetResponse> changedCalendars;
            if (calendarMetadataChanged)
            {
                const auto calendarGetRequest = api::calendarGet({.accountId = accountId,
                                                                  .ids = std::nullopt,
                                                                  .idsReference = std::nullopt,
                                                                  .properties = std::nullopt});
                if (!calendarGetRequest)
                    co_return error(OperationErrorCode::InvalidRequest,
                                    QStringLiteral("Unable to serialize changed calendar data."));

                api::RequestBuilder getBuilder;
                getBuilder.useCore().useCapability(std::string{api::calendarsCapabilityUri});
                const auto calendarGetHandle =
                    getBuilder.call(*calendarGetRequest, "changed-calendars");
                const auto getResult = co_await m_protocolClient.call(
                    context(settings, session, accountId), std::move(getBuilder));
                if (!isCurrentRefresh(ownerAccountId, generation))
                    co_return summary;
                const auto* getEnvelope = std::get_if<api::ResponseEnvelope>(&getResult);
                if (!getEnvelope)
                    co_return callError(getResult);
                const auto getRead = api::ResponseReader{*getEnvelope}.require(calendarGetHandle);
                if (const auto* readError = std::get_if<api::ResponseReaderError>(&getRead))
                    co_return responseError(*readError);
                auto getResponse = std::get<api::CalendarGetResponse>(getRead);
                if (getResponse.accountId != accountId || !getResponse.notFound.empty())
                    co_return error(OperationErrorCode::ProtocolViolation,
                                    QStringLiteral("Invalid changed Calendar/get response."));
                changedCalendars = std::move(getResponse);
            }

            const auto calendarCurrent = fenceIsCurrent(m_connection, calendarFence);
            const auto eventCurrent = fenceIsCurrent(m_connection, eventFence);
            if (const auto* serviceError = std::get_if<OperationError>(&calendarCurrent))
                co_return *serviceError;
            if (const auto* serviceError = std::get_if<OperationError>(&eventCurrent))
                co_return *serviceError;
            if (!std::get<bool>(calendarCurrent) || !std::get<bool>(eventCurrent))
                co_return summary;
            const std::string& acceptedCalendarState =
                changedCalendars ? changedCalendars->state : calendarChanges.newState;
            if (changedCalendars)
            {
                if (const auto cacheError = repository.replaceCalendars(
                        accountId, acceptedCalendarState, changedCalendars->list))
                    co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            }
            if (const auto cacheError =
                    repository.applyEventDelta(accountId, acceptedCalendarState,
                                               eventChanges.newState, displayTimeZone, {}, {}, {}))
                co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            if (calendarMetadataChanged)
                ++summary.accountCount;
        }
        if (fullRefreshRequired)
            co_return co_await refresh(std::move(settings), std::move(ownerAccountId),
                                       std::move(interval), std::move(displayTimeZone));
        co_return summary;
    }

    QCoro::Task<CalendarRefreshResult>
    CalendarSyncEngine::refresh(LiveConnectionSettings settings, std::string ownerAccountId,
                                VisibleInterval interval, TimeZoneId displayTimeZone,
                                const bool refreshCalendarMetadata)
    {
        co_return co_await refreshMaterialization(std::move(settings), std::move(ownerAccountId),
                                                  std::move(interval), std::move(displayTimeZone),
                                                  refreshCalendarMetadata,
                                                  MaterializationTarget::PresentationWindow);
    }

    QCoro::Task<CalendarRefreshResult>
    CalendarSyncEngine::refreshReminderHorizon(LiveConnectionSettings settings,
                                               std::string ownerAccountId, VisibleInterval interval,
                                               TimeZoneId displayTimeZone)
    {
        co_return co_await refreshMaterialization(std::move(settings), std::move(ownerAccountId),
                                                  std::move(interval), std::move(displayTimeZone),
                                                  false, MaterializationTarget::ReminderHorizon);
    }

    QCoro::Task<CalendarRefreshResult> CalendarSyncEngine::refreshMaterialization(
        LiveConnectionSettings settings, std::string ownerAccountId, VisibleInterval interval,
        TimeZoneId displayTimeZone, const bool refreshCalendarMetadata,
        const MaterializationTarget target)
    {
        const auto generation = beginRefresh(ownerAccountId);
        const auto sessionResult = loadSession(m_connection, ownerAccountId);
        if (const auto* serviceError = std::get_if<OperationError>(&sessionResult))
            co_return *serviceError;
        const auto& session = std::get<api::Session>(sessionResult);
        RefreshedRange summary{.interval = interval,
                               .displayTimeZone = displayTimeZone,
                               .accountCount = 0,
                               .eventCount = 0,
                               .calendarMetadataAuthoritative = !refreshCalendarMetadata,
                               .reconciledMutations = {}};
        for (const auto& [accountId, account] : session.accounts)
        {
            if (!account.accountCapabilities.calendars)
                continue;
            auto accountInterval = interval;
            if (target == MaterializationTarget::ReminderHorizon)
            {
                const auto bounded = boundExpandedInterval(
                    interval, account.accountCapabilities.calendars->maxExpandedQueryDuration);
                if (const auto* serviceError = std::get_if<OperationError>(&bounded))
                    co_return *serviceError;
                accountInterval = std::get<VisibleInterval>(bounded);
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
            std::optional<api::MethodRequest<api::CalendarGetResponse>> calendarRequest;
            if (refreshCalendarMetadata)
            {
                calendarRequest = api::calendarGet({.accountId = accountId,
                                                    .ids = std::nullopt,
                                                    .idsReference = std::nullopt,
                                                    .properties = std::nullopt});
            }
            const auto queryRequest =
                api::calendarEventQuery({.accountId = accountId,
                                         .filter = {.inCalendar = std::nullopt,
                                                    .after = accountInterval.start,
                                                    .before = accountInterval.end,
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
                                                    .after = accountInterval.start,
                                                    .before = accountInterval.end,
                                                    .text = std::nullopt},
                                         .expandRecurrences = false,
                                         .timeZone = displayTimeZone,
                                         .position = 0,
                                         .limit = std::nullopt,
                                         .calculateTotal = true});
            if ((refreshCalendarMetadata && !calendarRequest) || !queryRequest || !baseQueryRequest)
                co_return error(OperationErrorCode::InvalidRequest,
                                QStringLiteral("Unable to serialize the calendar range request."));
            api::RequestBuilder builder;
            builder.useCore().useCapability(std::string{api::calendarsCapabilityUri});
            std::optional<api::CallHandle<api::CalendarGetResponse>> calendarHandle;
            if (calendarRequest)
                calendarHandle = builder.call(*calendarRequest, "calendar-get");
            const auto queryHandle = builder.call(*queryRequest, "calendar-event-query");
            const auto baseQueryHandle =
                builder.call(*baseQueryRequest, "calendar-base-event-query");
            const auto result = co_await m_protocolClient.call(
                context(settings, session, accountId), std::move(builder));
            if (!isCurrentRefresh(ownerAccountId, generation))
                co_return summary;
            const auto* envelope = std::get_if<api::ResponseEnvelope>(&result);
            if (!envelope)
                co_return callError(result);
            api::ResponseReader reader{*envelope};
            std::optional<api::CalendarGetResponse> calendarResponse;
            if (calendarHandle)
            {
                const auto calendarsRead = reader.require(*calendarHandle);
                if (const auto* readError = std::get_if<api::ResponseReaderError>(&calendarsRead))
                    co_return responseError(*readError);
                calendarResponse = std::get<api::CalendarGetResponse>(calendarsRead);
            }
            const auto queryRead = reader.require(queryHandle);
            if (const auto* readError = std::get_if<api::ResponseReaderError>(&queryRead))
                co_return responseError(*readError);
            const auto baseQueryRead = reader.require(baseQueryHandle);
            if (const auto* readError = std::get_if<api::ResponseReaderError>(&baseQueryRead))
                co_return responseError(*readError);
            const auto& query = std::get<api::CalendarEventQueryResponse>(queryRead);
            const auto& baseQuery = std::get<api::CalendarEventQueryResponse>(baseQueryRead);
            const auto materializationReason = target == MaterializationTarget::ReminderHorizon
                                                   ? QStringLiteral("reminder-horizon")
                                                   : QStringLiteral("visible-range");
            qCDebug(logCalendarService).noquote()
                << "calendar range query reason" << materializationReason
                << QString::fromStdString(accountId)
                << QString::fromStdString(accountInterval.start.value)
                << QString::fromStdString(accountInterval.end.value) << "expanded"
                << query.ids.size() << "base" << baseQuery.ids.size() << "metadata"
                << refreshCalendarMetadata;
            if (calendarResponse)
            {
                const auto& calendars = *calendarResponse;
                sync::MutationJournalRepository genericJournal{m_connection};
                const auto activeCalendarMutations =
                    genericJournal.listActive({.accountId = accountId, .dataType = "Calendar"});
                if (const auto* cacheError =
                        std::get_if<cache::DatabaseError>(&activeCalendarMutations))
                    co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
                const auto& activeCalendarChanges =
                    std::get<std::vector<sync::MutationRecord>>(activeCalendarMutations);
                if (!activeCalendarChanges.empty())
                {
                    const auto calendarCurrent =
                        fenceGenerationIsCurrent(m_connection, calendarFence);
                    if (const auto* serviceError = std::get_if<OperationError>(&calendarCurrent))
                        co_return *serviceError;
                    if (!std::get<bool>(calendarCurrent))
                        co_return summary;
                    if (std::ranges::any_of(
                            activeCalendarChanges, [](const auto& mutation)
                            { return mutation.status != sync::MutationStatus::Unknown; }))
                        co_return summary;
                    const auto& mutation = activeCalendarChanges.front();
                    if (mutation.mutationKind == "calendar_create")
                        co_return summary;
                    if (mutation.mutationKind == "calendar_destroy")
                    {
                        if (std::ranges::find(calendars.list, mutation.objectId, &Calendar::id) !=
                            calendars.list.end())
                            co_return summary;
                        auto resolveResult = sync::MutationProjectionTransaction::begin(
                            m_connection, QStringLiteral("Resolve deleted Calendar uncertainty"));
                        if (const auto* cacheError =
                                std::get_if<cache::DatabaseError>(&resolveResult))
                            co_return error(OperationErrorCode::LocalStorageFailure,
                                            cacheError->message);
                        auto resolve =
                            std::get<sync::MutationProjectionTransaction>(std::move(resolveResult));
                        if (const auto cacheError =
                                resolve.transition(mutation.mutationId,
                                                   sync::MutationStatus::Accepted, calendars.state))
                            co_return error(OperationErrorCode::LocalStorageFailure,
                                            cacheError->message);
                        const std::array domains{sync::ConsistencyDomain{
                            .accountId = accountId,
                            .dataType = "Calendar",
                        }};
                        if (const auto cacheError = resolve.advance(domains))
                            co_return error(OperationErrorCode::LocalStorageFailure,
                                            cacheError->message);
                        cache::CalendarRepository repository{m_connection};
                        if (const auto cacheError = repository.clearCalendarDeletion(
                                resolve.cacheTransaction(), mutation.mutationId))
                            co_return error(OperationErrorCode::LocalStorageFailure,
                                            cacheError->message);
                        if (const auto cacheError = repository.removeProjectedCalendar(
                                resolve.cacheTransaction(), accountId, mutation.objectId))
                            co_return error(OperationErrorCode::LocalStorageFailure,
                                            cacheError->message);
                        if (const auto cacheError = repository.applyCalendarDefaults(
                                resolve.cacheTransaction(), accountId, calendars.state, {}))
                            co_return error(OperationErrorCode::LocalStorageFailure,
                                            cacheError->message);
                        if (const auto cacheError = resolve.remove(mutation.mutationId))
                            co_return error(OperationErrorCode::LocalStorageFailure,
                                            cacheError->message);
                        if (const auto cacheError = resolve.commit())
                            co_return error(OperationErrorCode::LocalStorageFailure,
                                            cacheError->message);
                        summary.calendarMetadataAuthoritative = true;
                        co_return summary;
                    }
                    if (mutation.mutationKind == "calendar_set_subscribed")
                    {
                        const auto calendar =
                            std::ranges::find(calendars.list, mutation.objectId, &Calendar::id);
                        if (calendar == calendars.list.end() ||
                            (mutation.payloadJson != "true" && mutation.payloadJson != "false"))
                            co_return summary;
                        const bool desired = mutation.payloadJson == "true";
                        const auto status = calendar->isSubscribed == desired
                                                ? sync::MutationStatus::Accepted
                                                : sync::MutationStatus::Rejected;
                        auto resolveResult = sync::MutationProjectionTransaction::begin(
                            m_connection,
                            QStringLiteral("Resolve Calendar subscription uncertainty"));
                        if (const auto* cacheError =
                                std::get_if<cache::DatabaseError>(&resolveResult))
                            co_return error(OperationErrorCode::LocalStorageFailure,
                                            cacheError->message);
                        auto resolve =
                            std::get<sync::MutationProjectionTransaction>(std::move(resolveResult));
                        if (const auto cacheError =
                                resolve.transition(mutation.mutationId, status, calendars.state))
                            co_return error(OperationErrorCode::LocalStorageFailure,
                                            cacheError->message);
                        const std::array domains{sync::ConsistencyDomain{
                            .accountId = accountId,
                            .dataType = "Calendar",
                        }};
                        if (const auto cacheError = resolve.advance(domains))
                            co_return error(OperationErrorCode::LocalStorageFailure,
                                            cacheError->message);
                        cache::CalendarRepository repository{m_connection};
                        if (const auto cacheError = repository.applyCalendarSubscription(
                                resolve.cacheTransaction(), accountId, mutation.objectId,
                                calendars.state, calendar->isSubscribed))
                            co_return error(OperationErrorCode::LocalStorageFailure,
                                            cacheError->message);
                        if (const auto cacheError = resolve.remove(mutation.mutationId))
                            co_return error(OperationErrorCode::LocalStorageFailure,
                                            cacheError->message);
                        if (const auto cacheError = resolve.commit())
                            co_return error(OperationErrorCode::LocalStorageFailure,
                                            cacheError->message);
                        summary.calendarMetadataAuthoritative = true;
                        co_return summary;
                    }
                    if (mutation.mutationKind == "calendar_set_color")
                    {
                        const auto calendar =
                            std::ranges::find(calendars.list, mutation.objectId, &Calendar::id);
                        const auto desired = calendarColorMutationPayload(mutation.payloadJson);
                        if (calendar == calendars.list.end() || !desired.has_value())
                            co_return summary;
                        const auto status = calendar->color == *desired
                                                ? sync::MutationStatus::Accepted
                                                : sync::MutationStatus::Rejected;
                        auto resolveResult = sync::MutationProjectionTransaction::begin(
                            m_connection, QStringLiteral("Resolve Calendar color uncertainty"));
                        if (const auto* cacheError =
                                std::get_if<cache::DatabaseError>(&resolveResult))
                            co_return error(OperationErrorCode::LocalStorageFailure,
                                            cacheError->message);
                        auto resolve =
                            std::get<sync::MutationProjectionTransaction>(std::move(resolveResult));
                        if (const auto cacheError =
                                resolve.transition(mutation.mutationId, status, calendars.state))
                            co_return error(OperationErrorCode::LocalStorageFailure,
                                            cacheError->message);
                        const std::array domains{sync::ConsistencyDomain{
                            .accountId = accountId,
                            .dataType = "Calendar",
                        }};
                        if (const auto cacheError = resolve.advance(domains))
                            co_return error(OperationErrorCode::LocalStorageFailure,
                                            cacheError->message);
                        cache::CalendarRepository repository{m_connection};
                        if (const auto cacheError = repository.applyCalendarColor(
                                resolve.cacheTransaction(), accountId, mutation.objectId,
                                calendars.state, calendar->color))
                            co_return error(OperationErrorCode::LocalStorageFailure,
                                            cacheError->message);
                        if (const auto cacheError = resolve.remove(mutation.mutationId))
                            co_return error(OperationErrorCode::LocalStorageFailure,
                                            cacheError->message);
                        if (const auto cacheError = resolve.commit())
                            co_return error(OperationErrorCode::LocalStorageFailure,
                                            cacheError->message);
                        summary.calendarMetadataAuthoritative = true;
                        co_return summary;
                    }
                    if (mutation.mutationKind == "calendar_set_default_alerts")
                    {
                        const auto calendar =
                            std::ranges::find(calendars.list, mutation.objectId, &Calendar::id);
                        const auto desired =
                            calendarDefaultAlertsMutationPayload(mutation.payloadJson);
                        if (calendar == calendars.list.end() || !desired.has_value())
                            co_return summary;
                        const auto status =
                            calendar->defaultAlertsWithTime == desired->first &&
                                    calendar->defaultAlertsWithoutTime == desired->second
                                ? sync::MutationStatus::Accepted
                                : sync::MutationStatus::Rejected;
                        auto resolveResult = sync::MutationProjectionTransaction::begin(
                            m_connection,
                            QStringLiteral("Resolve Calendar default notification uncertainty"));
                        if (const auto* cacheError =
                                std::get_if<cache::DatabaseError>(&resolveResult))
                            co_return error(OperationErrorCode::LocalStorageFailure,
                                            cacheError->message);
                        auto resolve =
                            std::get<sync::MutationProjectionTransaction>(std::move(resolveResult));
                        if (const auto cacheError =
                                resolve.transition(mutation.mutationId, status, calendars.state))
                            co_return error(OperationErrorCode::LocalStorageFailure,
                                            cacheError->message);
                        const std::array domains{sync::ConsistencyDomain{
                            .accountId = accountId,
                            .dataType = "Calendar",
                        }};
                        if (const auto cacheError = resolve.advance(domains))
                            co_return error(OperationErrorCode::LocalStorageFailure,
                                            cacheError->message);
                        cache::CalendarRepository repository{m_connection};
                        if (const auto cacheError = repository.applyCalendarDefaultAlerts(
                                resolve.cacheTransaction(), accountId, mutation.objectId,
                                calendars.state, calendar->defaultAlertsWithTime,
                                calendar->defaultAlertsWithoutTime))
                            co_return error(OperationErrorCode::LocalStorageFailure,
                                            cacheError->message);
                        if (const auto cacheError = resolve.remove(mutation.mutationId))
                            co_return error(OperationErrorCode::LocalStorageFailure,
                                            cacheError->message);
                        if (const auto cacheError = resolve.commit())
                            co_return error(OperationErrorCode::LocalStorageFailure,
                                            cacheError->message);
                        summary.calendarMetadataAuthoritative = true;
                        co_return summary;
                    }
                    std::unordered_map<std::string, bool> defaults;
                    for (const auto& calendar : calendars.list)
                        defaults.emplace(calendar.id, calendar.isDefault);
                    auto resolveResult = sync::MutationProjectionTransaction::begin(
                        m_connection, QStringLiteral("Resolve default Calendar uncertainty"));
                    if (const auto* cacheError = std::get_if<cache::DatabaseError>(&resolveResult))
                        co_return error(OperationErrorCode::LocalStorageFailure,
                                        cacheError->message);
                    auto resolve =
                        std::get<sync::MutationProjectionTransaction>(std::move(resolveResult));
                    const std::array domains{sync::ConsistencyDomain{
                        .accountId = accountId,
                        .dataType = "Calendar",
                    }};
                    for (const auto& defaultMutation : activeCalendarChanges)
                    {
                        const auto selected = defaults.find(defaultMutation.objectId);
                        const auto status = selected != defaults.end() && selected->second
                                                ? sync::MutationStatus::Accepted
                                                : sync::MutationStatus::Rejected;
                        if (const auto cacheError = resolve.transition(defaultMutation.mutationId,
                                                                       status, calendars.state))
                            co_return error(OperationErrorCode::LocalStorageFailure,
                                            cacheError->message);
                        if (const auto cacheError = resolve.remove(defaultMutation.mutationId))
                            co_return error(OperationErrorCode::LocalStorageFailure,
                                            cacheError->message);
                    }
                    if (const auto cacheError = resolve.advance(domains))
                        co_return error(OperationErrorCode::LocalStorageFailure,
                                        cacheError->message);
                    cache::CalendarRepository repository{m_connection};
                    if (const auto cacheError = repository.applyCalendarDefaults(
                            resolve.cacheTransaction(), accountId, calendars.state, defaults))
                        co_return error(OperationErrorCode::LocalStorageFailure,
                                        cacheError->message);
                    if (const auto cacheError = resolve.commit())
                        co_return error(OperationErrorCode::LocalStorageFailure,
                                        cacheError->message);
                    summary.calendarMetadataAuthoritative = true;
                    co_return summary;
                }
            }
            auto eventIds = query.ids;
            const auto total = query.total.value_or(eventIds.size());
            while (eventIds.size() < total)
            {
                const auto nextRequest =
                    api::calendarEventQuery({.accountId = accountId,
                                             .filter = {.inCalendar = std::nullopt,
                                                        .after = accountInterval.start,
                                                        .before = accountInterval.end,
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
                const auto nextResult = co_await m_protocolClient.call(
                    context(settings, session, accountId), std::move(nextBuilder));
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
                                                        .after = accountInterval.start,
                                                        .before = accountInterval.end,
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
                const auto nextResult = co_await m_protocolClient.call(
                    context(settings, session, accountId), std::move(nextBuilder));
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
                m_protocolClient, settings, session, accountId, eventIds, displayTimeZone,
                batchLimit, "calendar-event-get", [this, &ownerAccountId, generation]
                { return isCurrentRefresh(ownerAccountId, generation); });
            if (std::holds_alternative<SupersededRefresh>(getResult))
                co_return summary;
            if (const auto* serviceError = std::get_if<OperationError>(&getResult))
                co_return *serviceError;
            auto events = std::get<api::CalendarEventGetResponse>(std::move(getResult));
            auto baseGetResult = co_await getCalendarEventsBatched(
                m_protocolClient, settings, session, accountId, baseEventIds, displayTimeZone,
                batchLimit, "calendar-base-event-get", [this, &ownerAccountId, generation]
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
                    const auto recoveryResult = co_await m_protocolClient.call(
                        context(settings, session, accountId), std::move(recoveryBuilder));
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
                    m_protocolClient, settings, session, accountId, recoveredBaseIds,
                    displayTimeZone, batchLimit, "calendar-base-event-recovery-get",
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
            auto reconciliationResult = co_await reconcileUnknownCalendarMutations(
                m_protocolClient, settings, ownerAccountId, accountId, baseResponse.state,
                activeMutations);
            if (const auto* operationError = std::get_if<OperationError>(&reconciliationResult))
                co_return *operationError;
            auto reconciliation =
                std::get<CalendarUnknownReconciliation>(std::move(reconciliationResult));
            const auto rebasedResult =
                rebaseCalendarEvents(std::move(baseResponse.list), reconciliation.unresolved);
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
            if (calendarResponse)
            {
                const auto calendarCurrent = fenceGenerationIsCurrent(m_connection, calendarFence);
                if (const auto* serviceError = std::get_if<OperationError>(&calendarCurrent))
                    co_return *serviceError;
                if (!std::get<bool>(calendarCurrent))
                    co_return summary;
            }
            const auto eventCurrent = fenceGenerationIsCurrent(m_connection, eventFence);
            if (const auto* serviceError = std::get_if<OperationError>(&eventCurrent))
                co_return *serviceError;
            if (!std::get<bool>(eventCurrent))
                co_return summary;
            if (calendarResponse)
            {
                if (const auto cacheError = repository.replaceCalendars(
                        accountId, calendarResponse->state, calendarResponse->list))
                    co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
                summary.calendarMetadataAuthoritative = true;
            }
            const auto materializedEventCount = occurrences.size();
            std::optional<cache::CalendarWindow> window;
            std::optional<cache::CalendarReminderHorizon> reminderHorizon;
            if (target == MaterializationTarget::ReminderHorizon)
            {
                reminderHorizon.emplace(cache::CalendarReminderHorizon{
                    .accountId = accountId,
                    .start = accountInterval.start,
                    .end = accountInterval.end,
                    .displayTimeZone = displayTimeZone,
                    .eventState = baseResponse.state,
                    .events = std::move(baseResponse.list),
                    .occurrences = std::move(occurrences),
                });
            }
            else
            {
                window.emplace(cache::CalendarWindow{
                    .accountId = accountId,
                    .start = accountInterval.start,
                    .end = accountInterval.end,
                    .displayTimeZone = displayTimeZone,
                    .queryState = query.queryState,
                    .eventState = baseResponse.state,
                    .events = std::move(baseResponse.list),
                    .occurrences = std::move(occurrences),
                });
            }
            if (reconciliation.resolved.empty())
            {
                const auto cacheError = reminderHorizon
                                            ? repository.reconcileReminderHorizon(*reminderHorizon)
                                            : repository.reconcileWindow(*window);
                if (cacheError)
                    co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            }
            else
            {
                auto transactionResult = javelin::jmap::sync::MutationProjectionTransaction::begin(
                    m_connection, QStringLiteral("Resolve CalendarEvent uncertainty"));
                if (const auto* cacheError = std::get_if<cache::DatabaseError>(&transactionResult))
                    co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
                auto transaction = std::get<javelin::jmap::sync::MutationProjectionTransaction>(
                    std::move(transactionResult));
                const auto materializationError =
                    reminderHorizon
                        ? repository.reconcileReminderHorizon(transaction.cacheTransaction(),
                                                              *reminderHorizon)
                        : repository.reconcileWindow(transaction.cacheTransaction(), *window);
                if (materializationError)
                    co_return error(OperationErrorCode::LocalStorageFailure,
                                    materializationError->message);
                std::vector<CalendarEvent> authoritativeEvents;
                std::vector<std::string> removedEventIds;
                for (const auto& resolved : reconciliation.resolved)
                {
                    if (resolved.authoritativeEvent.has_value())
                        authoritativeEvents.push_back(*resolved.authoritativeEvent);
                    removedEventIds.insert(removedEventIds.end(), resolved.removedEventIds.begin(),
                                           resolved.removedEventIds.end());
                }
                if (const auto cacheError = repository.projectEvents(
                        transaction.cacheTransaction(), accountId, baseResponse.state,
                        authoritativeEvents, {}, removedEventIds,
                        reminderHorizon ? cache::CalendarEventStatePersistence::PreserveCursor
                                        : cache::CalendarEventStatePersistence::AdvanceCursor))
                    co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
                const std::array domains{javelin::jmap::sync::ConsistencyDomain{
                    .accountId = accountId,
                    .dataType = "CalendarEvent",
                }};
                if (const auto cacheError = transaction.advance(domains))
                    co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
                for (const auto& resolved : reconciliation.resolved)
                {
                    const auto status =
                        resolved.disposition ==
                                javelin::jmap::sync::MutationReconciliationDisposition::Applied
                            ? javelin::jmap::sync::MutationStatus::Accepted
                            : javelin::jmap::sync::MutationStatus::Rejected;
                    const auto rejection =
                        status == javelin::jmap::sync::MutationStatus::Rejected
                            ? std::optional<
                                  std::string_view>{R"({"type":"supersededByAuthoritativeState"})"}
                            : std::nullopt;
                    if (const auto cacheError = transaction.transition(
                            resolved.record.mutationId, status,
                            status == javelin::jmap::sync::MutationStatus::Accepted
                                ? std::optional<std::string_view>{baseResponse.state}
                                : std::nullopt,
                            rejection))
                        co_return error(OperationErrorCode::LocalStorageFailure,
                                        cacheError->message);
                }
                if (const auto cacheError = transaction.retireTerminal())
                    co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
                if (const auto cacheError = transaction.commit())
                    co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);

                for (const auto& resolved : reconciliation.resolved)
                {
                    const auto disposition =
                        resolved.disposition ==
                                javelin::jmap::sync::MutationReconciliationDisposition::Applied
                            ? QStringLiteral("applied")
                            : QStringLiteral("superseded");
                    qCInfo(logCalendarService)
                        << "Resolved ambiguous CalendarEvent mutation" << disposition;
                    summary.reconciledMutations.push_back({
                        .accountId = accountId,
                        .dataType = "CalendarEvent",
                        .objectId = resolved.record.objectId,
                        .operationGroupId = resolved.record.operationGroupId,
                        .disposition = resolved.disposition,
                    });
                }
            }
            ++summary.accountCount;
            summary.eventCount += materializedEventCount;
        }
        co_return summary;
    }

    QCoro::Task<CalendarMutationResult>
    CalendarMutationEngine::create(LiveConnectionSettings settings, std::string ownerAccountId,
                                   CreateEventCommand command,
                                   std::function<void()> projectionCommitted)
    {
        const bool scheduled =
            std::ranges::any_of(command.event.attendees, [](const Attendee& participant)
                                { return participant.isAttendee && !participant.isOwner; });
        const bool hasOwner =
            std::ranges::any_of(command.event.attendees,
                                [](const Attendee& participant) { return participant.isOwner; });
        if (scheduled && !hasOwner)
        {
            CalendarCacheReader reader{m_connection};
            const auto identitiesResult = reader.participantIdentities(command.accountId);
            if (const auto* readError = std::get_if<OperationError>(&identitiesResult))
                co_return *readError;
            const auto& identities = std::get<std::vector<ParticipantIdentity>>(identitiesResult);
            if (identities.empty())
            {
                co_return error(
                    OperationErrorCode::SchedulingUnsupported,
                    QStringLiteral(
                        "This calendar account has no participant identity for scheduling."));
            }
            const auto preferred =
                std::ranges::find(identities, true, &ParticipantIdentity::isDefault);
            const auto& identity = preferred != identities.end() ? *preferred : identities.front();
            const auto ownerIndex =
                participantIndexForAddress(command.event, identity.calendarAddress);
            if (!ownerIndex.has_value())
            {
                std::unordered_set<std::string> participantIds;
                for (const auto& participant : command.event.attendees)
                    participantIds.insert(participant.id);
                std::string participantId = "owner";
                for (std::size_t suffix = 2; participantIds.contains(participantId); ++suffix)
                    participantId = "owner-" + std::to_string(suffix);
                std::optional<std::string> email;
                const auto calendarAddress =
                    QString::fromStdString(identity.calendarAddress).trimmed();
                if (calendarAddress.startsWith(QStringLiteral("mailto:"), Qt::CaseInsensitive))
                    email = calendarAddress.mid(7).toStdString();
                command.event.attendees.push_back({
                    .id = std::move(participantId),
                    .name = identity.name,
                    .email = std::move(email),
                    .calendarAddress = identity.calendarAddress,
                    .participationStatus = "accepted",
                    .isOwner = true,
                    .isAttendee = true,
                    .roles = {},
                    .expectReply = false,
                    .scheduleSequence = 0,
                    .scheduleUpdated = std::nullopt,
                });
            }
            else
            {
                auto& owner = command.event.attendees[*ownerIndex];
                owner.isOwner = true;
                owner.isAttendee = true;
                owner.participationStatus = "accepted";
                owner.expectReply = false;
            }
        }
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
        co_return co_await mutate(std::move(settings), std::move(ownerAccountId),
                                  std::move(request), std::move(calendarIds),
                                  std::move(command.operationGroupId),
                                  std::move(command.materialization), MutationPermission::Write,
                                  std::move(projectionCommitted));
    }

    QCoro::Task<CalendarMutationResult>
    CalendarMutationEngine::update(LiveConnectionSettings settings, std::string ownerAccountId,
                                   UpdateEventCommand command,
                                   std::function<void()> projectionCommitted)
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
        const bool privateOnly = onlyPerUserPropertiesChanged(*previous, command.event);
        const auto calendarsResult = repository.listCalendars(command.accountId);
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&calendarsResult))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        if (!privateOnly && !eventWritable(std::get<std::vector<Calendar>>(calendarsResult),
                                           calendarIds, *previous, settings.loginEmail))
            co_return error(OperationErrorCode::PermissionDenied,
                            QStringLiteral("You do not have permission to modify this event."));
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
            std::move(command.materialization),
            privateOnly ? MutationPermission::Private : MutationPermission::Write,
            std::move(projectionCommitted));
    }

    QCoro::Task<CalendarMutationResult>
    CalendarMutationEngine::remove(LiveConnectionSettings settings, std::string ownerAccountId,
                                   DeleteEventCommand command,
                                   std::function<void()> projectionCommitted)
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
                                  std::move(command.operationGroupId),
                                  std::move(command.materialization), MutationPermission::Write,
                                  std::move(projectionCommitted));
    }

    QCoro::Task<CalendarMutationResult>
    CalendarMutationEngine::respond(LiveConnectionSettings settings, std::string ownerAccountId,
                                    RespondToEventCommand command,
                                    std::function<void()> projectionCommitted)
    {
        if (command.participationStatus != "accepted" &&
            command.participationStatus != "tentative" && command.participationStatus != "declined")
            co_return error(OperationErrorCode::InvalidRequest,
                            QStringLiteral("The requested RSVP status is invalid."));

        cache::CalendarRepository repository{m_connection};
        const auto cached = repository.findEvent(command.accountId, command.eventId);
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&cached))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        auto previous = std::get<std::optional<CalendarEvent>>(cached);
        if (!previous)
            co_return error(OperationErrorCode::NotFound,
                            QStringLiteral("The calendar invitation is no longer in the cache."));
        if (previous->isOrigin)
            co_return error(
                OperationErrorCode::InvalidRequest,
                QStringLiteral(
                    "This event is organized by this account and does not need an RSVP."));
        std::vector<std::string> initialCalendarIds;
        for (const auto& [calendarId, present] : previous->calendarIds)
            if (present)
                initialCalendarIds.push_back(calendarId);
        const auto calendarsResult = repository.listCalendars(command.accountId);
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&calendarsResult))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        if (!rsvpable(std::get<std::vector<Calendar>>(calendarsResult), initialCalendarIds))
            co_return error(OperationErrorCode::PermissionDenied,
                            QStringLiteral("You do not have permission to RSVP to this event."));

        const auto sessionResult = loadSession(m_connection, ownerAccountId);
        if (const auto* serviceError = std::get_if<OperationError>(&sessionResult))
            co_return *serviceError;
        const auto& session = std::get<api::Session>(sessionResult);
        const auto account = session.accounts.find(command.accountId);
        if (account == session.accounts.end() || !account->second.accountCapabilities.calendars)
            co_return error(
                OperationErrorCode::UnsupportedCapability,
                QStringLiteral("This account does not support JMAP Calendars draft-26."));

        const auto identitiesMethod = api::participantIdentityGet({.accountId = command.accountId,
                                                                   .ids = std::nullopt,
                                                                   .idsReference = std::nullopt,
                                                                   .properties = std::nullopt});
        if (!identitiesMethod)
            co_return error(
                OperationErrorCode::InvalidRequest,
                QStringLiteral("Unable to serialize the participant identity request."));
        api::RequestBuilder identityBuilder;
        identityBuilder.useCore().useCapability(std::string{api::calendarsCapabilityUri});
        const auto identityHandle =
            identityBuilder.call(*identitiesMethod, "participant-identities");
        const auto identityResult = co_await m_protocolClient.call(
            context(settings, session, command.accountId), std::move(identityBuilder));
        const auto* identityEnvelope = std::get_if<api::ResponseEnvelope>(&identityResult);
        if (!identityEnvelope)
            co_return callError(identityResult);
        const auto identityRead = api::ResponseReader{*identityEnvelope}.require(identityHandle);
        if (const auto* readError = std::get_if<api::ResponseReaderError>(&identityRead))
            co_return responseError(*readError);
        const auto& identities = std::get<api::ParticipantIdentityGetResponse>(identityRead).list;

        const auto refreshed = repository.findEvent(command.accountId, command.eventId);
        if (const auto* cacheError = std::get_if<cache::DatabaseError>(&refreshed))
            co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
        previous = std::get<std::optional<CalendarEvent>>(refreshed);
        if (!previous)
            co_return error(OperationErrorCode::NotFound,
                            QStringLiteral("The calendar invitation is no longer in the cache."));
        if (previous->isOrigin)
            co_return error(
                OperationErrorCode::InvalidRequest,
                QStringLiteral(
                    "This event is now organized by this account and does not need an RSVP."));
        if (!command.ifInState)
        {
            const auto state = currentEventState(m_connection, command.accountId);
            if (const auto* serviceError = std::get_if<OperationError>(&state))
                co_return *serviceError;
            command.ifInState = std::get<std::string>(state);
        }
        std::vector<std::string> calendarIds;
        for (const auto& [calendarId, present] : previous->calendarIds)
            if (present)
                calendarIds.push_back(calendarId);
        std::optional<CalendarEvent> occurrence;
        const CalendarEvent* responseEvent = &*previous;
        if (command.recurrenceId)
        {
            if (!previous->recurrenceRule &&
                !previous->recurrenceOverrides.contains(command.recurrenceId->value))
                co_return error(OperationErrorCode::InvalidRequest,
                                QStringLiteral("This event has no matching recurring occurrence."));
            occurrence = effectiveOccurrenceEvent(*previous, *command.recurrenceId);
            if (!occurrence)
                co_return error(OperationErrorCode::NotFound,
                                QStringLiteral("This recurring occurrence no longer exists."));
            responseEvent = &*occurrence;
        }
        if (responseEvent->attendees.empty())
            co_return error(OperationErrorCode::InvalidRequest,
                            QStringLiteral("This invitation has no participants to RSVP as."));
        const auto participantIndex = matchingParticipantIndex(*responseEvent, identities);
        if (!participantIndex)
            co_return error(
                OperationErrorCode::InvalidRequest,
                QStringLiteral(
                    "This invitation does not contain one of your calendar identities."));

        if (responseEvent->attendees[*participantIndex].participationStatus ==
            command.participationStatus)
            co_return CommittedMutation{.accountId = std::move(command.accountId),
                                        .newState = *command.ifInState,
                                        .createdId = std::nullopt,
                                        .receipt = {}};
        auto updated =
            command.recurrenceId
                ? setOccurrenceParticipationStatus(*previous, *command.recurrenceId,
                                                   responseEvent->attendees[*participantIndex].id,
                                                   command.participationStatus)
                : *previous;
        if (!command.recurrenceId)
            updated.attendees[*participantIndex].participationStatus = command.participationStatus;
        api::CalendarEventSetRequest request{
            .accountId = command.accountId,
            .ifInState = command.ifInState,
            .create = {},
            .update = {{command.eventId, {.previous = *previous, .event = std::move(updated)}}},
            .destroy = {},
            .sendSchedulingMessages = true,
        };
        co_return co_await mutate(std::move(settings), std::move(ownerAccountId),
                                  std::move(request), std::move(calendarIds), std::nullopt,
                                  std::move(command.materialization), MutationPermission::Rsvp,
                                  std::move(projectionCommitted));
    }

    QCoro::Task<CalendarMutationResult> CalendarMutationEngine::mutate(
        LiveConnectionSettings settings, std::string ownerAccountId,
        api::CalendarEventSetRequest request, std::vector<std::string> calendarIds,
        std::optional<std::string> operationGroupId,
        std::optional<CalendarRangeMaterialization> materialization,
        const MutationPermission permission, std::function<void()> projectionCommitted,
        const bool recoverStateMismatch)
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
        const auto& calendars = std::get<std::vector<Calendar>>(calendarsResult);
        const bool permitted =
            permission == MutationPermission::Rsvp      ? rsvpable(calendars, calendarIds)
            : permission == MutationPermission::Private ? privateUpdatable(calendars, calendarIds)
                                                        : writable(calendars, calendarIds);
        if (!permitted)
        {
            const auto message =
                permission == MutationPermission::Rsvp
                    ? QStringLiteral("You do not have permission to RSVP to this event.")
                : permission == MutationPermission::Private
                    ? QStringLiteral("You do not have permission to change private event settings.")
                    : QStringLiteral("You do not have permission to modify this event.");
            co_return error(OperationErrorCode::PermissionDenied, message);
        }
        const auto method = api::calendarEventSet(request);
        if (!method)
            co_return error(OperationErrorCode::InvalidRequest,
                            QStringLiteral("Unable to serialize the calendar change."));
        const auto preparedResult = prepareCalendarMutations(repository, request, operationGroupId);
        if (const auto* operationError = std::get_if<OperationError>(&preparedResult))
            co_return *operationError;
        auto prepared = std::get<PreparedCalendarMutations>(preparedResult);
        auto recoveryMaterialization = materialization;
        if (!recoveryMaterialization)
        {
            const CalendarEvent* anchor = nullptr;
            if (!request.update.empty())
                anchor = &request.update.begin()->second.previous;
            else if (!request.create.empty())
                anchor = &request.create.begin()->second;
            if (anchor != nullptr)
                recoveryMaterialization = recoveryMaterializationForEvent(*anchor);
            else
            {
                const auto baseRecord =
                    std::ranges::find_if(prepared.records, [](const auto& record)
                                         { return record.baseDocument.has_value(); });
                if (baseRecord != prepared.records.end())
                {
                    const auto base = eventFromMutationDocument(
                        *baseRecord, *baseRecord->baseDocument, baseRecord->objectId);
                    if (const auto* event = std::get_if<CalendarEvent>(&base))
                        recoveryMaterialization = recoveryMaterializationForEvent(*event);
                }
            }
        }
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
        const auto recoverFromStateMismatch =
            [this, &settings, &ownerAccountId, &request, &calendarIds, &operationGroupId,
             &materialization, permission, &projectionCommitted, &prepared, &journal, &repository,
             &recoveryMaterialization,
             recoverStateMismatch](OperationError failure) -> QCoro::Task<CalendarMutationResult>
        {
            if (!prepared.records.empty())
            {
                if (const auto restoreError = restoreCalendarMutations(
                        m_connection, repository, prepared.records,
                        request.ifInState.value_or(std::string{}), failure.message.toStdString(),
                        prepared.rollbackOccurrences, nullptr, nullptr, nullptr))
                    co_return *restoreError;
            }
            if (!recoverStateMismatch || !recoveryMaterialization)
            {
                if (projectionCommitted)
                    projectionCommitted();
                co_return failure;
            }

            qCInfo(logCalendarService) << "CalendarEvent state mismatch; refresh and retry";
            auto refreshed = co_await m_syncEngine.refreshChanged(
                settings, ownerAccountId, recoveryMaterialization->interval,
                recoveryMaterialization->displayTimeZone);
            if (const auto* refreshError = std::get_if<OperationError>(&refreshed))
            {
                if (projectionCommitted)
                    projectionCommitted();
                co_return *refreshError;
            }

            const auto currentStateResult = currentEventState(m_connection, request.accountId);
            if (const auto* stateError = std::get_if<OperationError>(&currentStateResult))
            {
                if (projectionCommitted)
                    projectionCommitted();
                co_return *stateError;
            }
            const auto& currentState = std::get<std::string>(currentStateResult);

            for (auto& [eventId, update] : request.update)
            {
                const auto current = repository.findEvent(request.accountId, eventId);
                if (const auto* cacheError = std::get_if<cache::DatabaseError>(&current))
                {
                    if (projectionCommitted)
                        projectionCommitted();
                    co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
                }
                const auto& refreshedEvent = std::get<std::optional<CalendarEvent>>(current);
                if (!refreshedEvent)
                {
                    if (projectionCommitted)
                        projectionCommitted();
                    co_return error(OperationErrorCode::NotFound,
                                    QStringLiteral("The event was removed while this edit was "
                                                   "being synchronized."));
                }
                auto rebased =
                    rebaseCalendarEventUpdate(update.previous, update.event, *refreshedEvent);
                if (const auto* rebaseError = std::get_if<OperationError>(&rebased))
                {
                    if (projectionCommitted)
                        projectionCommitted();
                    co_return *rebaseError;
                }
                update.previous = *refreshedEvent;
                update.event = std::get<CalendarEvent>(std::move(rebased));
            }

            if (!request.update.empty())
            {
                calendarIds.clear();
                for (const auto& [calendarId, present] :
                     request.update.begin()->second.event.calendarIds)
                    if (present)
                        calendarIds.push_back(calendarId);
            }
            else if (!request.destroy.empty())
            {
                const auto current =
                    repository.findEvent(request.accountId, request.destroy.front());
                if (const auto* cacheError = std::get_if<cache::DatabaseError>(&current))
                {
                    if (projectionCommitted)
                        projectionCommitted();
                    co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
                }
                const auto& refreshedEvent = std::get<std::optional<CalendarEvent>>(current);
                if (!refreshedEvent)
                {
                    if (projectionCommitted)
                        projectionCommitted();
                    co_return CommittedMutation{
                        .accountId = request.accountId,
                        .newState = currentState,
                        .createdId = std::nullopt,
                        .receipt =
                            {
                                .domains = {},
                                .acceptedObjectIds = request.destroy,
                                .rejectedObjectIds = {},
                                .affectedCacheViews = {"calendar"},
                                .incompleteMaterialization = false,
                            },
                    };
                }
                calendarIds.clear();
                for (const auto& [calendarId, present] : refreshedEvent->calendarIds)
                    if (present)
                        calendarIds.push_back(calendarId);
            }

            request.ifInState = currentState;
            qCInfo(logCalendarService) << "Retry CalendarEvent mutation";
            bool retryProjectionCommitted = false;
            auto retryCallback = [&projectionCommitted, &retryProjectionCommitted]
            {
                retryProjectionCommitted = true;
                if (projectionCommitted)
                    projectionCommitted();
            };
            auto retried =
                co_await mutate(settings, ownerAccountId, request, calendarIds, operationGroupId,
                                materialization, permission, std::move(retryCallback), false);
            if (!retryProjectionCommitted && projectionCommitted)
                projectionCommitted();
            co_return retried;
        };
        std::vector<std::string> timingUpdateIds;
        bool timingUpdateTouchesRecurringMaterialization = false;
        for (const auto& [eventId, update] : request.update)
        {
            if (!timingInputsChanged(update.previous, update.event))
                continue;
            timingUpdateIds.push_back(eventId);
            timingUpdateTouchesRecurringMaterialization =
                timingUpdateTouchesRecurringMaterialization || update.previous.recurrenceRule ||
                update.event.recurrenceRule || !update.previous.recurrenceOverrides.empty() ||
                !update.event.recurrenceOverrides.empty();
        }

        auto timingReadTimeZone =
            materialization
                ? materialization->displayTimeZone
                : TimeZoneId{.value =
                                 QString::fromUtf8(QTimeZone::systemTimeZoneId()).toStdString()};
        if (timingReadTimeZone.value.empty())
            timingReadTimeZone.value = "UTC";

        api::RequestBuilder builder;
        builder.useCore().useCapability(std::string{api::calendarsCapabilityUri});
        const auto handle = builder.call(*method, "calendar-event-set");
        std::optional<api::CallHandle<api::CalendarEventGetResponse>> timingGetHandle;
        if (!timingUpdateIds.empty())
        {
            const auto get = api::calendarEventGet({
                .accountId = request.accountId,
                .ids = timingUpdateIds,
                .idsReference = std::nullopt,
                .properties = calendarEventReadProperties(),
                .recurrenceOverridesBefore = std::nullopt,
                .recurrenceOverridesAfter = std::nullopt,
                .reduceParticipants = false,
                .timeZone = timingReadTimeZone,
            });
            if (!get)
                co_return error(OperationErrorCode::InvalidRequest,
                                QStringLiteral("Unable to serialize updated event retrieval."));
            timingGetHandle = builder.call(*get, "calendar-event-derived-timing-get");
        }
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
                .properties = calendarEventReadProperties(),
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
        const auto result = co_await m_protocolClient.call(
            context(settings, session, request.accountId), std::move(builder));
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
            auto failure = responseError(*readError);
            if (isStateMismatch(failure))
                co_return co_await recoverFromStateMismatch(std::move(failure));
            if (const auto cacheError = journal.transition(
                    prepared.records, javelin::jmap::sync::MutationStatus::Unknown))
                co_return error(OperationErrorCode::LocalStorageFailure, cacheError->message);
            co_return failure;
        }
        const auto& response = std::get<api::CalendarEventSetResponse>(read);
        std::vector<CalendarEvent> authoritativeTimingUpdates;
        bool timingReadComplete = timingUpdateIds.empty();
        if (timingGetHandle.has_value())
        {
            const auto timingRead = api::ResponseReader{*envelope}.require(*timingGetHandle);
            if (const auto* timingResponse =
                    std::get_if<api::CalendarEventGetResponse>(&timingRead);
                timingResponse != nullptr && timingResponse->accountId == request.accountId &&
                timingResponse->state == response.newState && timingResponse->notFound.empty() &&
                timingResponse->list.size() == timingUpdateIds.size() &&
                std::ranges::all_of(
                    timingUpdateIds,
                    [&](const std::string& eventId)
                    {
                        const auto event =
                            std::ranges::find(timingResponse->list, eventId, &CalendarEvent::id);
                        const auto requested = request.update.find(eventId);
                        return event != timingResponse->list.end() &&
                               requested != request.update.end() &&
                               !timingInputsChanged(requested->second.event, *event) &&
                               event->utcStart.has_value() && event->utcEnd.has_value();
                    }))
            {
                authoritativeTimingUpdates = timingResponse->list;
                timingReadComplete = true;
            }
        }
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
            if (setError->type == api::CalendarSetErrorType::StateMismatch)
            {
                auto failure =
                    error(OperationErrorCode::Conflict,
                          QString::fromStdString(setError->description.value_or(
                              "The event changed on the server. Refresh and try again.")));
                failure.protocolType = "stateMismatch";
                co_return co_await recoverFromStateMismatch(std::move(failure));
            }
            if (const auto restoreError = restoreCalendarMutations(
                    m_connection, repository, prepared.records,
                    request.ifInState.value_or(std::string{}), errorJson,
                    prepared.rollbackOccurrences, materialization ? &*materialization : nullptr,
                    materializedQuery ? &*materializedQuery : nullptr,
                    materializedEvents ? &*materializedEvents : nullptr))
                co_return *restoreError;
            if (projectionCommitted)
                projectionCommitted();
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
        auto accepted = acceptCalendarMutations(
            m_connection, repository, prepared.records, response, authoritativeTimingUpdates,
            materialization ? &*materialization : nullptr,
            materializedQuery ? &*materializedQuery : nullptr,
            materializedEvents ? &*materializedEvents : nullptr);
        if (const auto* operationError = std::get_if<OperationError>(&accepted))
        {
            static_cast<void>(
                journal.transition(prepared.records, javelin::jmap::sync::MutationStatus::Unknown));
            co_return *operationError;
        }
        auto& receipt = std::get<CommittedMutation>(accepted).receipt;
        const bool materializationComplete =
            !materialization.has_value() || !receipt.incompleteMaterialization;
        const bool recurringTimingMaterializationComplete =
            !timingUpdateTouchesRecurringMaterialization ||
            (materialization.has_value() && materializationComplete);
        receipt.incompleteMaterialization = receipt.incompleteMaterialization ||
                                            !timingReadComplete ||
                                            !recurringTimingMaterializationComplete;
        if (projectionCommitted)
            projectionCommitted();
        m_syncEngine.invalidateRefresh(ownerAccountId);
        co_return accepted;
    }
} // namespace javelin::jmap::calendar
