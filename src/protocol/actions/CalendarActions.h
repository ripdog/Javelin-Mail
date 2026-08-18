#pragma once

#include "protocol/actions/ActionDescriptor.h"

#include "app/CalendarApplicationPorts.h"
#include "jmap/calendar/CalendarReader.h"

#include <string>
#include <tuple>

namespace javelin::protocol::actions
{
    namespace detail
    {
        constexpr auto calendarOnly = changedDomains(ChangedDomain::Calendars);
        constexpr auto calendarHistory =
            changedDomains(ChangedDomain::Calendars, ChangedDomain::History);
    } // namespace detail

    using CalendarReadCached =
        Descriptor<1, ActionDomain::Calendar, AdmissionSemantics::Immediate,
                   ReplayPolicy::Reexecute, 0,
                   std::tuple<std::string, javelin::jmap::calendar::VisibleInterval,
                              javelin::jmap::calendar::TimeZoneId>,
                   javelin::jmap::calendar::CalendarLoadResult>;
    using CalendarReadAccounts =
        Descriptor<2, ActionDomain::Calendar, AdmissionSemantics::Immediate,
                   ReplayPolicy::Reexecute, 0, std::tuple<>,
                   javelin::jmap::calendar::CalendarAccountsResult>;
    using CalendarReadCalendars =
        Descriptor<3, ActionDomain::Calendar, AdmissionSemantics::Immediate,
                   ReplayPolicy::Reexecute, 0, std::tuple<std::string>,
                   javelin::jmap::calendar::CalendarListResult>;
    using CalendarRequestRange =
        Descriptor<4, ActionDomain::Calendar, AdmissionSemantics::Asynchronous,
                   ReplayPolicy::Reexecute, detail::calendarOnly,
                   std::tuple<std::string, javelin::jmap::calendar::VisibleInterval,
                              javelin::jmap::calendar::TimeZoneId>,
                   javelin::jmap::calendar::CalendarRefreshResult>;
    using CalendarCreateEvent =
        Descriptor<5, ActionDomain::Calendar, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
                   detail::calendarHistory,
                   std::tuple<std::string, javelin::jmap::calendar::CreateEventCommand,
                              javelin::app::undo::CommandOrigin>,
                   javelin::jmap::calendar::CalendarMutationResult>;
    using CalendarUpdateEvent =
        Descriptor<6, ActionDomain::Calendar, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
                   detail::calendarHistory,
                   std::tuple<std::string, javelin::jmap::calendar::UpdateEventCommand,
                              javelin::app::undo::CommandOrigin>,
                   javelin::jmap::calendar::CalendarMutationResult>;
    using CalendarDeleteEvent =
        Descriptor<7, ActionDomain::Calendar, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
                   detail::calendarHistory,
                   std::tuple<std::string, javelin::jmap::calendar::DeleteEventCommand,
                              javelin::app::undo::CommandOrigin>,
                   javelin::jmap::calendar::CalendarMutationResult>;
    using CalendarSetDefault = Descriptor<
        8, ActionDomain::Calendar, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
        detail::calendarHistory,
        std::tuple<std::string, std::string, std::string, javelin::app::undo::CommandOrigin>,
        javelin::jmap::calendar::CalendarMutationResult>;
    using CalendarCreate =
        Descriptor<9, ActionDomain::Calendar, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
                   detail::calendarHistory,
                   std::tuple<std::string, javelin::jmap::calendar::CreateCalendarCommand>,
                   javelin::jmap::calendar::CalendarMutationResult>;
    using CalendarDelete =
        Descriptor<10, ActionDomain::Calendar, AdmissionSemantics::Asynchronous,
                   ReplayPolicy::Never, detail::calendarHistory,
                   std::tuple<std::string, javelin::jmap::calendar::DeleteCalendarCommand>,
                   javelin::jmap::calendar::CalendarMutationResult>;
    using CalendarSetVisible =
        Descriptor<11, ActionDomain::Calendar, AdmissionSemantics::Immediate, ReplayPolicy::Never,
                   detail::calendarHistory,
                   std::tuple<std::string, std::string, bool, javelin::app::undo::CommandOrigin>,
                   javelin::jmap::calendar::CalendarPreferenceResult>;
    using CalendarSetSubscribed =
        Descriptor<73, ActionDomain::Calendar, AdmissionSemantics::Asynchronous,
                   ReplayPolicy::Never, detail::calendarHistory,
                   std::tuple<std::string, std::string, std::string, bool>,
                   javelin::jmap::calendar::CalendarMutationResult>;
    using CalendarRespondEvent =
        Descriptor<79, ActionDomain::Calendar, AdmissionSemantics::Asynchronous,
                   ReplayPolicy::Never, detail::calendarOnly,
                   std::tuple<std::string, javelin::jmap::calendar::RespondToEventCommand>,
                   javelin::jmap::calendar::CalendarMutationResult>;
    using CalendarReadPendingInvitations =
        Descriptor<90, ActionDomain::Calendar, AdmissionSemantics::Immediate,
                   ReplayPolicy::Reexecute, 0, std::tuple<>,
                   javelin::jmap::calendar::PendingCalendarInvitationsResult>;
    using CalendarReadEvent =
        Descriptor<91, ActionDomain::Calendar, AdmissionSemantics::Immediate,
                   ReplayPolicy::Reexecute, 0, std::tuple<std::string, std::string>,
                   javelin::jmap::calendar::CalendarEventReadResult>;
    using CalendarReadParticipantIdentities =
        Descriptor<92, ActionDomain::Calendar, AdmissionSemantics::Immediate,
                   ReplayPolicy::Reexecute, 0, std::tuple<std::string>,
                   javelin::jmap::calendar::ParticipantIdentityListResult>;
    using CalendarReadRangeSnapshot =
        Descriptor<93, ActionDomain::Calendar, AdmissionSemantics::Immediate,
                   ReplayPolicy::Reexecute, 0,
                   std::tuple<std::string, javelin::jmap::calendar::VisibleInterval,
                              javelin::jmap::calendar::TimeZoneId>,
                   javelin::jmap::calendar::CalendarLoadResult>;
    using CalendarReadDaySnapshot = Descriptor<
        95, ActionDomain::Calendar, AdmissionSemantics::Immediate, ReplayPolicy::Reexecute, 0,
        std::tuple<javelin::jmap::calendar::VisibleInterval, javelin::jmap::calendar::TimeZoneId>,
        javelin::jmap::calendar::CalendarDaySnapshotResult>;

    using CalendarActionTypes = std::tuple<
        RegisteredAction<CalendarReadCached, "CalendarReadCached">,
        RegisteredAction<CalendarReadAccounts, "CalendarReadAccounts">,
        RegisteredAction<CalendarReadCalendars, "CalendarReadCalendars">,
        RegisteredAction<CalendarRequestRange, "CalendarRequestRange">,
        RegisteredAction<CalendarCreateEvent, "CalendarCreateEvent">,
        RegisteredAction<CalendarUpdateEvent, "CalendarUpdateEvent">,
        RegisteredAction<CalendarDeleteEvent, "CalendarDeleteEvent">,
        RegisteredAction<CalendarSetDefault, "CalendarSetDefault">,
        RegisteredAction<CalendarCreate, "CalendarCreate">,
        RegisteredAction<CalendarDelete, "CalendarDelete">,
        RegisteredAction<CalendarSetVisible, "CalendarSetVisible">,
        RegisteredAction<CalendarSetSubscribed, "CalendarSetSubscribed">,
        RegisteredAction<CalendarRespondEvent, "CalendarRespondEvent">,
        RegisteredAction<CalendarReadPendingInvitations, "CalendarReadPendingInvitations">,
        RegisteredAction<CalendarReadEvent, "CalendarReadEvent">,
        RegisteredAction<CalendarReadParticipantIdentities, "CalendarReadParticipantIdentities">,
        RegisteredAction<CalendarReadRangeSnapshot, "CalendarReadRangeSnapshot">,
        RegisteredAction<CalendarReadDaySnapshot, "CalendarReadDaySnapshot">>;
} // namespace javelin::protocol::actions
