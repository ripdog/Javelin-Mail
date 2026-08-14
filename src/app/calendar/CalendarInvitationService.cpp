#include "app/CalendarInvitationService.h"

#include "app/AccountRuntimeManager.h"
#include "app/CalendarApplicationService.h"
#include "jmap/api/CalendarMethods.h"
#include "jmap/api/MethodCaller.h"
#include "jmap/api/RequestBuilder.h"
#include "jmap/api/ResponseReader.h"
#include "jmap/cache/CalendarRepository.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/calendar/CalendarEventEditing.h"
#include "jmap/calendar/CalendarProtocolClient.h"
#include "jmap/calendar/CalendarReader.h"

#include <QCoroTask>

#include <KLocalizedString>

#include <QDate>
#include <QDateTime>
#include <QDebug>
#include <QLocale>
#include <QTimeZone>

#include <algorithm>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace javelin::app
{
    namespace
    {
        struct CallFailure
        {
            QString message;
            bool cannotCalculateChanges = false;
        };

        template <typename Response> using CallResult = std::variant<Response, CallFailure>;

        [[nodiscard]] javelin::jmap::LiveConnectionSettings
        liveSettings(const AccountConnectionSettings& settings)
        {
            return {.sessionUrl = settings.sessionUrl,
                    .loginEmail = settings.loginEmail,
                    .apiKey = settings.apiKey};
        }

        [[nodiscard]] javelin::jmap::api::ApiRequestContext
        context(const javelin::jmap::LiveConnectionSettings& settings,
                const javelin::jmap::api::Session& session, std::string accountId)
        {
            return {.credentials = {.accountId = std::move(accountId),
                                    .emailAddress = settings.loginEmail,
                                    .sessionUrl = settings.sessionUrl,
                                    .token = {.accessToken = settings.apiKey,
                                              .refreshToken = std::nullopt,
                                              .expiry = std::nullopt}},
                    .apiUrl = session.apiUrl,
                    .requestLimits = javelin::jmap::api::coreRequestLimits(session)};
        }

        template <typename Response>
        QCoro::Task<CallResult<Response>>
        call(javelin::jmap::calendar::CalendarProtocolClient& protocolClient,
             const javelin::jmap::LiveConnectionSettings& settings,
             const javelin::jmap::api::Session& session, const std::string& accountId,
             const javelin::jmap::api::MethodRequest<Response>& request, std::string callId)
        {
            javelin::jmap::api::RequestBuilder builder;
            builder.useCore().useCapability(
                std::string{javelin::jmap::api::calendarsCapabilityUri});
            const auto handle = builder.call(request, std::move(callId));
            const auto result = co_await protocolClient.call(context(settings, session, accountId),
                                                             std::move(builder));
            const auto* envelope = std::get_if<javelin::jmap::api::ResponseEnvelope>(&result);
            if (envelope == nullptr)
            {
                co_return CallFailure{.message = QStringLiteral("JMAP calendar request failed")};
            }
            const auto read = javelin::jmap::api::ResponseReader{*envelope}.require(handle);
            if (const auto* error = std::get_if<javelin::jmap::api::ResponseReaderError>(&read))
            {
                co_return CallFailure{
                    .message = QString::fromStdString(error->message),
                    .cannotCalculateChanges = error->methodError.has_value() &&
                                              error->methodError->type == "cannotCalculateChanges",
                };
            }
            co_return std::get<Response>(read);
        }

        [[nodiscard]] const std::vector<std::string>& eventProperties()
        {
            static const std::vector<std::string> properties{"id",
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
                                                             "participants"};
            return properties;
        }

        [[nodiscard]] const std::vector<std::string>& notificationProperties()
        {
            static const std::vector<std::string> properties{
                "id",      "created", "changedBy", "comment", "type", "calendarEventId",
                "isDraft", "event",   "eventPatch"};
            return properties;
        }

        [[nodiscard]] const std::vector<std::string>& calendarProperties()
        {
            static const std::vector<std::string> properties{
                "id",           "name",      "description", "color",    "sortOrder",
                "isSubscribed", "isVisible", "isDefault",   "timeZone", "myRights"};
            return properties;
        }

        [[nodiscard]] const std::vector<std::string>& participantIdentityProperties()
        {
            static const std::vector<std::string> properties{"id", "name", "calendarAddress",
                                                             "isDefault"};
            return properties;
        }

        [[nodiscard]] std::size_t getBatchLimit(const javelin::jmap::api::Session& session)
        {
            if (session.capabilities.coreDetails &&
                session.capabilities.coreDetails->maxObjectsInGet)
                return static_cast<std::size_t>(*session.capabilities.coreDetails->maxObjectsInGet);
            return 256;
        }

        QCoro::Task<CallResult<javelin::jmap::api::CalendarEventGetResponse>>
        getEvents(javelin::jmap::calendar::CalendarProtocolClient& protocolClient,
                  const javelin::jmap::LiveConnectionSettings& settings,
                  const javelin::jmap::api::Session& session, const std::string& accountId,
                  const std::vector<std::string>& ids,
                  const javelin::jmap::calendar::TimeZoneId& zone)
        {
            const auto limit = getBatchLimit(session);
            if (limit == 0)
                co_return CallFailure{
                    .message = QStringLiteral("Server advertises maxObjectsInGet as zero")};
            javelin::jmap::api::CalendarEventGetResponse combined{
                .accountId = accountId, .state = {}, .list = {}, .notFound = {}};
            std::size_t offset = 0;
            bool first = true;
            do
            {
                const auto count = std::min(limit, ids.size() - offset);
                std::vector<std::string> batch{ids.begin() + static_cast<std::ptrdiff_t>(offset),
                                               ids.begin() +
                                                   static_cast<std::ptrdiff_t>(offset + count)};
                const auto method =
                    javelin::jmap::api::calendarEventGet({.accountId = accountId,
                                                          .ids = std::move(batch),
                                                          .idsReference = std::nullopt,
                                                          .properties = eventProperties(),
                                                          .recurrenceOverridesBefore = std::nullopt,
                                                          .recurrenceOverridesAfter = std::nullopt,
                                                          .reduceParticipants = false,
                                                          .timeZone = zone});
                if (!method)
                    co_return CallFailure{
                        .message = QStringLiteral("Unable to serialize CalendarEvent/get")};
                auto response = co_await call(protocolClient, settings, session, accountId, *method,
                                              "calendar-invitation-event-get");
                if (const auto* failure = std::get_if<CallFailure>(&response))
                    co_return *failure;
                auto page =
                    std::get<javelin::jmap::api::CalendarEventGetResponse>(std::move(response));
                if (first)
                {
                    combined.state = page.state;
                    first = false;
                }
                else if (combined.state != page.state)
                {
                    co_return CallFailure{
                        .message = QStringLiteral("CalendarEvent/get batches changed state")};
                }
                combined.list.insert(combined.list.end(),
                                     std::make_move_iterator(page.list.begin()),
                                     std::make_move_iterator(page.list.end()));
                combined.notFound.insert(combined.notFound.end(),
                                         std::make_move_iterator(page.notFound.begin()),
                                         std::make_move_iterator(page.notFound.end()));
                offset += count;
            } while (offset < ids.size());
            co_return combined;
        }

        QCoro::Task<CallResult<javelin::jmap::api::CalendarEventNotificationGetResponse>>
        getNotifications(javelin::jmap::calendar::CalendarProtocolClient& protocolClient,
                         const javelin::jmap::LiveConnectionSettings& settings,
                         const javelin::jmap::api::Session& session, const std::string& accountId,
                         const std::vector<std::string>& ids)
        {
            const auto limit = getBatchLimit(session);
            if (limit == 0)
                co_return CallFailure{
                    .message = QStringLiteral("Server advertises maxObjectsInGet as zero")};
            javelin::jmap::api::CalendarEventNotificationGetResponse combined{
                .accountId = accountId, .state = {}, .list = {}, .notFound = {}};
            std::size_t offset = 0;
            bool first = true;
            do
            {
                const auto count = std::min(limit, ids.size() - offset);
                std::vector<std::string> batch{ids.begin() + static_cast<std::ptrdiff_t>(offset),
                                               ids.begin() +
                                                   static_cast<std::ptrdiff_t>(offset + count)};
                const auto method = javelin::jmap::api::calendarEventNotificationGet(
                    {.accountId = accountId,
                     .ids = std::move(batch),
                     .idsReference = std::nullopt,
                     .properties = notificationProperties()});
                if (!method)
                    co_return CallFailure{.message = QStringLiteral(
                                              "Unable to serialize CalendarEventNotification/get")};
                auto response = co_await call(protocolClient, settings, session, accountId, *method,
                                              "calendar-invitation-notification-get");
                if (const auto* failure = std::get_if<CallFailure>(&response))
                    co_return *failure;
                auto page = std::get<javelin::jmap::api::CalendarEventNotificationGetResponse>(
                    std::move(response));
                if (first)
                {
                    combined.state = page.state;
                    first = false;
                }
                else if (combined.state != page.state)
                {
                    co_return CallFailure{
                        .message =
                            QStringLiteral("CalendarEventNotification/get batches changed state")};
                }
                combined.list.insert(combined.list.end(),
                                     std::make_move_iterator(page.list.begin()),
                                     std::make_move_iterator(page.list.end()));
                combined.notFound.insert(combined.notFound.end(),
                                         std::make_move_iterator(page.notFound.begin()),
                                         std::make_move_iterator(page.notFound.end()));
                offset += count;
            } while (offset < ids.size());
            co_return combined;
        }

        template <typename Response, typename MakeRequest>
        QCoro::Task<CallResult<std::vector<std::string>>>
        queryIds(javelin::jmap::calendar::CalendarProtocolClient& protocolClient,
                 const javelin::jmap::LiveConnectionSettings& settings,
                 const javelin::jmap::api::Session& session, const std::string& accountId,
                 MakeRequest makeRequest)
        {
            constexpr std::uint64_t pageLimit = 256;
            std::vector<std::string> ids;
            std::optional<std::string> queryState;
            std::optional<std::uint64_t> total;
            for (;;)
            {
                const auto method = makeRequest(static_cast<std::uint64_t>(ids.size()), pageLimit,
                                                !total.has_value());
                if (!method)
                    co_return CallFailure{.message = QStringLiteral("Unable to serialize query")};
                auto response = co_await call(protocolClient, settings, session, accountId, *method,
                                              "calendar-invitation-query");
                if (const auto* failure = std::get_if<CallFailure>(&response))
                    co_return *failure;
                auto page = std::get<Response>(std::move(response));
                if (!queryState)
                    queryState = page.queryState;
                else if (*queryState != page.queryState)
                    co_return CallFailure{.message =
                                              QStringLiteral("Query state changed while paging")};
                if (!total)
                    total = page.total;
                const auto pageSize = page.ids.size();
                ids.insert(ids.end(), std::make_move_iterator(page.ids.begin()),
                           std::make_move_iterator(page.ids.end()));
                if (total && ids.size() >= *total)
                    break;
                if (pageSize < pageLimit)
                    break;
                if (pageSize == 0)
                    co_return CallFailure{.message =
                                              QStringLiteral("Query pagination stopped early")};
            }
            co_return ids;
        }

        [[nodiscard]] const javelin::jmap::calendar::Attendee*
        selfParticipant(const javelin::jmap::calendar::CalendarEvent& event,
                        const std::vector<javelin::jmap::calendar::ParticipantIdentity>& identities)
        {
            const auto match = [&event](const auto& identity) -> std::optional<std::size_t>
            {
                return javelin::jmap::calendar::participantIndexForAddress(
                    event, identity.calendarAddress);
            };
            for (const auto& identity : identities)
            {
                if (!identity.isDefault)
                    continue;
                if (const auto index = match(identity))
                    return &event.attendees[*index];
            }
            for (const auto& identity : identities)
                if (const auto index = match(identity))
                    return &event.attendees[*index];
            return nullptr;
        }

        [[nodiscard]] bool rsvpAllowed(
            const javelin::jmap::calendar::CalendarEvent& event,
            const std::unordered_map<std::string, javelin::jmap::calendar::Calendar>& calendars)
        {
            bool hasMembership = false;
            for (const auto& [calendarId, present] : event.calendarIds)
            {
                if (!present)
                    continue;
                hasMembership = true;
                const auto calendar = calendars.find(calendarId);
                if (calendar == calendars.end() || !calendar->second.myRights.mayRSVP)
                    return false;
            }
            return hasMembership;
        }

        [[nodiscard]] bool
        hasCurrentOrFutureOccurrence(const javelin::jmap::calendar::CalendarEvent& event)
        {
            const auto nowUtc = QDateTime::currentDateTimeUtc();
            if (event.utcEnd)
            {
                const auto end =
                    QDateTime::fromString(QString::fromStdString(event.utcEnd->value), Qt::ISODate);
                if (end.isValid() && end >= nowUtc)
                    return true;
                if (!event.recurrenceRule)
                    return false;
            }
            if (event.recurrenceRule)
            {
                if (!event.recurrenceRule->until)
                    return true;
                const auto until = QString::fromStdString(event.recurrenceRule->until->value);
                return until.left(10) >= QDate::currentDate().toString(Qt::ISODate);
            }
            return QString::fromStdString(event.start.value).left(10) >=
                   QDate::currentDate().toString(Qt::ISODate);
        }

        [[nodiscard]] std::optional<javelin::jmap::cache::CalendarInvitationProjection>
        pendingProjection(
            const javelin::jmap::calendar::CalendarEvent& event,
            const std::vector<javelin::jmap::calendar::ParticipantIdentity>& identities,
            const std::unordered_map<std::string, javelin::jmap::calendar::Calendar>& calendars,
            std::optional<std::string> sourceNotificationId, bool enqueueDesktopNotification,
            bool knownFutureFromQuery)
        {
            if (event.id.empty() || event.isDraft || event.isOrigin ||
                event.status == std::optional<std::string>{"cancelled"})
                return std::nullopt;
            const auto* participant = selfParticipant(event, identities);
            if (participant == nullptr || participant->isOwner ||
                participant->participationStatus != "needs-action")
                return std::nullopt;
            if (!rsvpAllowed(event, calendars))
                return std::nullopt;
            if (!knownFutureFromQuery && !hasCurrentOrFutureOccurrence(event))
                return std::nullopt;
            return javelin::jmap::cache::CalendarInvitationProjection{
                .eventId = event.id,
                .selfParticipantId = participant->id,
                .sourceNotificationId = std::move(sourceNotificationId),
                .enqueueDesktopNotification = enqueueDesktopNotification,
            };
        }

        [[nodiscard]] QString notificationBody(
            const javelin::jmap::cache::CalendarInvitationDispatchCandidate& invitation)
        {
            const auto display =
                invitation.utcStart
                    ? QDateTime::fromString(QString::fromStdString(invitation.utcStart->value),
                                            Qt::ISODate)
                          .toLocalTime()
                    : QDateTime::fromString(QString::fromStdString(invitation.start.value),
                                            Qt::ISODate);
            QStringList lines;
            if (!invitation.organizer.empty())
                lines.push_back(i18n("From %1", QString::fromStdString(invitation.organizer)));
            if (display.isValid())
                lines.push_back(QLocale{}.toString(display, QLocale::ShortFormat));
            if (invitation.location && invitation.location->size() <= 120)
                lines.push_back(QString::fromStdString(*invitation.location));
            return lines.join(QLatin1Char('\n'));
        }
    } // namespace

    CalendarInvitationService::CalendarInvitationService(
        javelin::jmap::cache::DatabaseConnection& connection,
        javelin::jmap::calendar::CalendarProtocolClient& protocolClient,
        javelin::jmap::calendar::CalendarReader& reader, AccountRuntimeManager& accountRuntime,
        CalendarApplicationService& calendarApplication, QObject* parent)
        : QObject(parent), m_connection(connection), m_protocolClient(protocolClient),
          m_reader(reader), m_accountRuntime(accountRuntime),
          m_calendarApplication(calendarApplication), m_repository(connection)
    {
        m_syncTimer.setSingleShot(true);
        m_syncTimer.setInterval(150);
        connect(&m_syncTimer, &QTimer::timeout, this,
                &CalendarInvitationService::synchronizeQueuedOwners);
        m_dispatchRetryTimer.setSingleShot(true);
        m_dispatchRetryTimer.setInterval(60000);
        connect(&m_dispatchRetryTimer, &QTimer::timeout, this,
                &CalendarInvitationService::dispatchPending);
        connect(&m_accountRuntime, &AccountRuntimeManager::sessionRefreshed, this,
                [this](const QString& ownerAccountId)
                { scheduleOwner(ownerAccountId.toStdString()); });
        connect(&m_accountRuntime, &AccountRuntimeManager::accountConfigured, this,
                [this](const QString& ownerAccountId)
                { scheduleOwner(ownerAccountId.toStdString()); });
        connect(&m_accountRuntime, &AccountRuntimeManager::calendarStateChanged, this,
                [this](const QString& ownerAccountId,
                       const javelin::jmap::sync::AccountTypeStateMap& changedStates)
                {
                    const bool relevant = std::ranges::any_of(
                        changedStates,
                        [](const auto& account)
                        {
                            return account.second.contains("Calendar") ||
                                   account.second.contains("CalendarEvent") ||
                                   account.second.contains("CalendarEventNotification");
                        });
                    if (relevant)
                        scheduleOwner(ownerAccountId.toStdString());
                });
        connect(&m_calendarApplication, &CalendarApplicationService::calendarCacheCommitted, this,
                [this](const CalendarCacheChange&) { refreshPresentationState(); });
    }

    void CalendarInvitationService::start()
    {
        if (m_started)
            return;
        m_started = true;
        if (const auto error = m_repository.recoverDispatches())
            qWarning().noquote() << "Recover calendar invitation dispatches:" << error->message;
        for (const auto& ownerAccountId : m_accountRuntime.configuredAccountIds())
            scheduleOwner(ownerAccountId);
        dispatchPending();
        refreshPresentationState();
    }

    void CalendarInvitationService::scheduleOwner(std::string ownerAccountId)
    {
        if (!m_started || ownerAccountId.empty())
            return;
        m_pendingOwners.insert(std::move(ownerAccountId));
        if (!m_syncTimer.isActive())
            m_syncTimer.start();
    }

    void CalendarInvitationService::synchronizeQueuedOwners()
    {
        auto owners = std::exchange(m_pendingOwners, {});
        for (auto& ownerAccountId : owners)
        {
            if (!m_runningOwners.insert(ownerAccountId).second)
            {
                m_pendingOwners.insert(std::move(ownerAccountId));
                continue;
            }
            const auto key = ownerAccountId;
            auto task = synchronizeOwner(ownerAccountId);
            QCoro::connect(std::move(task), this,
                           [this, key]
                           {
                               m_runningOwners.erase(key);
                               if (m_pendingOwners.contains(key) && !m_syncTimer.isActive())
                                   m_syncTimer.start();
                           });
        }
    }

    QCoro::Task<void> CalendarInvitationService::synchronizeOwner(std::string ownerAccountId)
    {
        const auto configuration = m_accountRuntime.configurationFor(ownerAccountId);
        if (!configuration)
            co_return;
        javelin::jmap::cache::SessionRepository sessions{m_connection};
        const auto loadedSession = sessions.load(ownerAccountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&loadedSession))
        {
            qWarning().noquote() << "Load calendar invitation session:" << error->message;
            co_return;
        }
        const auto& session = std::get<std::optional<javelin::jmap::api::Session>>(loadedSession);
        if (!session || !session->capabilities.calendars)
            co_return;
        const auto settings = liveSettings(configuration->second.settings);
        const javelin::jmap::calendar::TimeZoneId zone{
            .value = QTimeZone::systemTimeZoneId().toStdString()};
        const javelin::jmap::calendar::LocalDateTime nowLocal{
            .value = QDateTime::currentDateTime()
                         .toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss"))
                         .toStdString()};

        for (const auto& [accountId, account] : session->accounts)
        {
            if (!account.accountCapabilities.calendars)
                continue;

            const auto calendarMethod =
                javelin::jmap::api::calendarGet({.accountId = accountId,
                                                 .ids = std::nullopt,
                                                 .idsReference = std::nullopt,
                                                 .properties = calendarProperties()});
            const auto identityMethod = javelin::jmap::api::participantIdentityGet(
                {.accountId = accountId,
                 .ids = std::nullopt,
                 .idsReference = std::nullopt,
                 .properties = participantIdentityProperties()});
            if (!calendarMethod || !identityMethod)
            {
                qWarning() << "Unable to serialize invitation metadata request";
                continue;
            }
            auto calendarsResponse =
                co_await call(m_protocolClient, settings, *session, accountId, *calendarMethod,
                              "calendar-invitation-calendars");
            auto identitiesResponse =
                co_await call(m_protocolClient, settings, *session, accountId, *identityMethod,
                              "calendar-invitation-identities");
            if (const auto* failure = std::get_if<CallFailure>(&calendarsResponse))
            {
                qWarning().noquote() << "Refresh invitation calendars:" << failure->message;
                continue;
            }
            if (const auto* failure = std::get_if<CallFailure>(&identitiesResponse))
            {
                qWarning().noquote() << "Refresh participant identities:" << failure->message;
                continue;
            }
            auto calendarGet =
                std::get<javelin::jmap::api::CalendarGetResponse>(std::move(calendarsResponse));
            auto identityGet = std::get<javelin::jmap::api::ParticipantIdentityGetResponse>(
                std::move(identitiesResponse));
            if (const auto error =
                    m_repository.replaceParticipantIdentities(accountId, identityGet.list))
            {
                qWarning().noquote() << "Store participant identities:" << error->message;
                continue;
            }
            javelin::jmap::cache::CalendarRepository calendarRepository{m_connection};
            if (const auto error = calendarRepository.replaceCalendars(accountId, calendarGet.state,
                                                                       calendarGet.list))
            {
                qWarning().noquote() << "Store invitation calendar metadata:" << error->message;
                continue;
            }
            std::unordered_map<std::string, javelin::jmap::calendar::Calendar> calendars;
            for (const auto& calendar : calendarGet.list)
                calendars.insert_or_assign(calendar.id, calendar);

            const auto stateResult =
                calendarRepository.stateToken(accountId, "CalendarEventNotification");
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&stateResult))
            {
                qWarning().noquote() << "Read invitation notification state:" << error->message;
                continue;
            }
            const auto previousNotificationState =
                std::get<std::optional<std::string>>(stateResult);
            const bool baseline = !previousNotificationState.has_value();

            std::vector<std::string> notificationIds;
            std::vector<std::string> deletedNotificationIds;
            std::string notificationState;
            bool fullNotificationReconciliation = baseline;
            if (!baseline)
            {
                auto sinceState = *previousNotificationState;
                for (;;)
                {
                    const auto changesMethod = javelin::jmap::api::calendarEventNotificationChanges(
                        {.accountId = accountId, .sinceState = sinceState, .maxChanges = 256});
                    if (!changesMethod)
                    {
                        fullNotificationReconciliation = true;
                        break;
                    }
                    auto changesResponse =
                        co_await call(m_protocolClient, settings, *session, accountId,
                                      *changesMethod, "calendar-invitation-notification-changes");
                    if (const auto* failure = std::get_if<CallFailure>(&changesResponse))
                    {
                        if (failure->cannotCalculateChanges)
                        {
                            fullNotificationReconciliation = true;
                            break;
                        }
                        qWarning().noquote()
                            << "Synchronize invitation notification changes:" << failure->message;
                        notificationState.clear();
                        break;
                    }
                    auto changes =
                        std::get<javelin::jmap::api::CalendarEventNotificationChangesResponse>(
                            std::move(changesResponse));
                    notificationIds.insert(notificationIds.end(), changes.created.begin(),
                                           changes.created.end());
                    notificationIds.insert(notificationIds.end(), changes.updated.begin(),
                                           changes.updated.end());
                    deletedNotificationIds.insert(deletedNotificationIds.end(),
                                                  changes.destroyed.begin(),
                                                  changes.destroyed.end());
                    notificationState = changes.newState;
                    sinceState = changes.newState;
                    if (!changes.hasMoreChanges)
                        break;
                }
                if (!fullNotificationReconciliation && notificationState.empty())
                    continue;
            }

            if (fullNotificationReconciliation)
            {
                auto queried =
                    co_await queryIds<javelin::jmap::api::CalendarEventNotificationQueryResponse>(
                        m_protocolClient, settings, *session, accountId,
                        [accountId](const std::uint64_t position, const std::uint64_t limit,
                                    const bool calculateTotal)
                        {
                            return javelin::jmap::api::calendarEventNotificationQuery(
                                {.accountId = accountId,
                                 .filter = {},
                                 .position = position,
                                 .limit = limit,
                                 .calculateTotal = calculateTotal});
                        });
                if (const auto* failure = std::get_if<CallFailure>(&queried))
                {
                    qWarning().noquote()
                        << "Query calendar event notifications:" << failure->message;
                    continue;
                }
                notificationIds = std::get<std::vector<std::string>>(std::move(queried));
                deletedNotificationIds.clear();
            }
            std::ranges::sort(notificationIds);
            notificationIds.erase(std::unique(notificationIds.begin(), notificationIds.end()),
                                  notificationIds.end());
            auto notificationsResponse = co_await getNotifications(
                m_protocolClient, settings, *session, accountId, notificationIds);
            if (const auto* failure = std::get_if<CallFailure>(&notificationsResponse))
            {
                qWarning().noquote() << "Fetch calendar event notifications:" << failure->message;
                continue;
            }
            auto notifications = std::get<javelin::jmap::api::CalendarEventNotificationGetResponse>(
                std::move(notificationsResponse));
            if (!fullNotificationReconciliation && notifications.state != notificationState)
            {
                qWarning() << "CalendarEventNotification state moved during reconciliation";
                scheduleOwner(ownerAccountId);
                continue;
            }
            notificationState = notifications.state;

            std::vector<std::string> eventIds;
            std::unordered_map<std::string, std::string> createdSourceByEvent;
            std::unordered_set<std::string> createdEventIds;
            for (const auto& notification : notifications.list)
            {
                if (notification.calendarEventId.empty())
                {
                    qWarning().noquote() << "CalendarEventNotification has no calendarEventId"
                                         << QString::fromStdString(notification.id);
                    continue;
                }
                eventIds.push_back(notification.calendarEventId);
                if (notification.type ==
                    javelin::jmap::calendar::CalendarEventNotificationType::Created)
                {
                    createdSourceByEvent.insert_or_assign(notification.calendarEventId,
                                                          notification.id);
                    createdEventIds.insert(notification.calendarEventId);
                }
            }
            const auto anchored = m_repository.pendingEventIds(accountId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&anchored))
            {
                qWarning().noquote() << "Read anchored invitations:" << error->message;
                continue;
            }
            const auto& anchoredIds = std::get<std::vector<std::string>>(anchored);
            eventIds.insert(eventIds.end(), anchoredIds.begin(), anchoredIds.end());

            std::unordered_set<std::string> knownFutureIds;
            if (baseline)
            {
                auto futureIds = co_await queryIds<javelin::jmap::api::CalendarEventQueryResponse>(
                    m_protocolClient, settings, *session, accountId,
                    [accountId, nowLocal, zone](const std::uint64_t position,
                                                const std::uint64_t limit,
                                                const bool calculateTotal)
                    {
                        return javelin::jmap::api::calendarEventQuery(
                            {.accountId = accountId,
                             .filter = {.after = nowLocal},
                             .expandRecurrences = false,
                             .timeZone = zone,
                             .position = position,
                             .limit = limit,
                             .calculateTotal = calculateTotal});
                    });
                if (const auto* failure = std::get_if<CallFailure>(&futureIds))
                {
                    qWarning().noquote() << "Query pending invitation events:" << failure->message;
                    continue;
                }
                const auto baselineEventIds =
                    std::get<std::vector<std::string>>(std::move(futureIds));
                knownFutureIds.insert(baselineEventIds.begin(), baselineEventIds.end());
                eventIds.insert(eventIds.end(), baselineEventIds.begin(), baselineEventIds.end());
            }
            std::ranges::sort(eventIds);
            eventIds.erase(std::unique(eventIds.begin(), eventIds.end()), eventIds.end());

            auto eventsResponse =
                co_await getEvents(m_protocolClient, settings, *session, accountId, eventIds, zone);
            if (const auto* failure = std::get_if<CallFailure>(&eventsResponse))
            {
                qWarning().noquote() << "Fetch current invitation events:" << failure->message;
                continue;
            }
            auto events =
                std::get<javelin::jmap::api::CalendarEventGetResponse>(std::move(eventsResponse));
            std::vector<javelin::jmap::cache::CalendarInvitationProjection> pending;
            for (const auto& event : events.list)
            {
                const auto source = createdSourceByEvent.find(event.id);
                const auto projection =
                    pendingProjection(event, identityGet.list, calendars,
                                      source == createdSourceByEvent.end()
                                          ? std::nullopt
                                          : std::optional<std::string>{source->second},
                                      baseline || createdEventIds.contains(event.id),
                                      knownFutureIds.contains(event.id));
                if (projection)
                    pending.push_back(*projection);
            }
            std::vector<std::string> destroyedEventIds;
            for (const auto& eventId : events.notFound)
                destroyedEventIds.push_back(eventId);

            std::vector<std::string> consideredEventIds = eventIds;
            if (baseline)
            {
                for (const auto& event : events.list)
                {
                    if (!knownFutureIds.contains(event.id) && !createdEventIds.contains(event.id) &&
                        std::ranges::find(anchoredIds, event.id) == anchoredIds.end())
                    {
                        std::erase(consideredEventIds, event.id);
                    }
                }
            }
            if (const auto error =
                    m_repository.reconcile({.accountId = accountId,
                                            .notificationState = notificationState,
                                            .eventState = events.state,
                                            .replaceNotifications = fullNotificationReconciliation,
                                            .notifications = notifications.list,
                                            .deletedNotificationIds = deletedNotificationIds,
                                            .events = events.list,
                                            .nonRecurringOccurrences = {},
                                            .destroyedEventIds = destroyedEventIds,
                                            .consideredEventIds = consideredEventIds,
                                            .pendingInvitations = pending}))
            {
                qWarning().noquote() << "Reconcile calendar invitations:" << error->message;
                continue;
            }
            Q_EMIT pendingInvitationCacheChanged();
        }
        refreshPresentationState();
        dispatchPending();
    }

    void CalendarInvitationService::requestDispatch()
    {
        if (!m_dispatchRetryTimer.isActive())
            m_dispatchRetryTimer.start(0);
    }

    void CalendarInvitationService::dispatchPending()
    {
        const auto claimed = m_repository.claimPendingDispatches();
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&claimed))
        {
            qWarning().noquote() << "Claim calendar invitation delivery:" << error->message;
            m_dispatchRetryTimer.start();
            return;
        }
        for (const auto& invitation :
             std::get<std::vector<javelin::jmap::cache::CalendarInvitationDispatchCandidate>>(
                 claimed))
        {
            const auto key = QString::fromStdString(invitation.invitationKey);
            m_liveInvitations.insert_or_assign(
                invitation.invitationKey,
                LiveInvitation{.accountId = invitation.accountId, .eventId = invitation.eventId});
            const auto start = QString::fromStdString(invitation.start.value);
            auto navigationDate =
                start.size() >= 10 ? QDate::fromString(start.left(10), Qt::ISODate) : QDate{};
            if (!navigationDate.isValid() ||
                (invitation.recurring && navigationDate < QDate::currentDate()))
                navigationDate = QDate::currentDate();
            const auto navigationDateText = navigationDate.toString(Qt::ISODate);
            Q_EMIT invitationReady(key, QString::fromStdString(invitation.accountId),
                                   QString::fromStdString(invitation.eventId),
                                   invitation.recurrenceId
                                       ? QString::fromStdString(invitation.recurrenceId->value)
                                       : QString{},
                                   navigationDateText, QString::fromStdString(invitation.title),
                                   notificationBody(invitation));
        }
    }

    void CalendarInvitationService::deliveryAccepted(const QString& invitationKey)
    {
        if (const auto error = m_repository.markDelivered(invitationKey.toStdString(),
                                                          QDateTime::currentDateTimeUtc()))
            qWarning().noquote() << "Record calendar invitation delivery:" << error->message;
    }

    void CalendarInvitationService::deliveryFailed(const QString& invitationKey)
    {
        if (const auto error = m_repository.releaseDispatch(invitationKey.toStdString()))
            qWarning().noquote() << "Release calendar invitation delivery:" << error->message;
        m_liveInvitations.erase(invitationKey.toStdString());
        m_dispatchRetryTimer.start();
    }

    void CalendarInvitationService::refreshPresentationState()
    {
        const auto pending = m_reader.pendingInvitations();
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&pending))
        {
            qWarning().noquote() << "Refresh pending invitation presentation:" << error->message;
            return;
        }
        std::unordered_set<std::string> pendingPairs;
        for (const auto& invitation :
             std::get<std::vector<javelin::jmap::calendar::PendingCalendarInvitation>>(pending))
            pendingPairs.insert(invitation.accountId + '\0' + invitation.eventId);
        for (auto it = m_liveInvitations.begin(); it != m_liveInvitations.end();)
        {
            if (pendingPairs.contains(it->second.accountId + '\0' + it->second.eventId))
            {
                ++it;
                continue;
            }
            Q_EMIT invitationResolved(QString::fromStdString(it->first));
            it = m_liveInvitations.erase(it);
        }
        Q_EMIT pendingInvitationCacheChanged();
    }
} // namespace javelin::app
