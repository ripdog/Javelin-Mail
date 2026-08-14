#pragma once

#include "protocol/actions/ActionDescriptor.h"

#include "app/DeveloperDiagnostics.h"
#include "app/DeveloperMaintenance.h"

#include <QString>

#include <tuple>

namespace javelin::protocol::actions
{
    namespace detail
    {
        constexpr auto developerMailbox =
            changedDomains(ChangedDomain::MailQueryWindows, ChangedDomain::MessageMetadata,
                           ChangedDomain::MessageContent);
    }

    using DeveloperDiagnosticsSnapshot =
        Descriptor<74, ActionDomain::Developer, AdmissionSemantics::Asynchronous,
                   ReplayPolicy::Reexecute, 0, std::tuple<>,
                   javelin::app::DeveloperDiagnosticsResult>;
    using DeveloperMailboxClear =
        Descriptor<75, ActionDomain::Developer, AdmissionSemantics::Asynchronous,
                   ReplayPolicy::Never, detail::developerMailbox,
                   std::tuple<javelin::app::DeveloperMailboxClearCommand>,
                   javelin::app::DeveloperMailboxClearResult>;
    using AcknowledgeRemoteActionResult =
        Descriptor<76, ActionDomain::Developer, AdmissionSemantics::Immediate, ReplayPolicy::Never,
                   0, std::tuple<QString>, std::monostate>;
    using DeveloperLogSetSubscribed =
        Descriptor<77, ActionDomain::Developer, AdmissionSemantics::Immediate, ReplayPolicy::Never,
                   0, std::tuple<bool>, std::monostate>;
    using DeveloperLogClear = Descriptor<78, ActionDomain::Developer, AdmissionSemantics::Immediate,
                                         ReplayPolicy::Never, 0, std::tuple<>, std::monostate>;

    using DeveloperActionTypes =
        std::tuple<RegisteredAction<DeveloperDiagnosticsSnapshot, "DeveloperDiagnosticsSnapshot">,
                   RegisteredAction<DeveloperMailboxClear, "DeveloperMailboxClear">,
                   RegisteredAction<AcknowledgeRemoteActionResult, "AcknowledgeRemoteActionResult">,
                   RegisteredAction<DeveloperLogSetSubscribed, "DeveloperLogSetSubscribed">,
                   RegisteredAction<DeveloperLogClear, "DeveloperLogClear">>;
} // namespace javelin::protocol::actions
