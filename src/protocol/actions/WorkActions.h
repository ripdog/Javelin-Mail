#pragma once

#include "protocol/actions/ActionDescriptor.h"

#include "app/WorkTaskPort.h"
#include "storage/DatabaseError.h"

#include <QString>

#include <optional>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace javelin::protocol::actions
{
    namespace detail
    {
        constexpr auto backgroundJobs = changedDomains(ChangedDomain::BackgroundJobs);
    }

    using WorkPause =
        Descriptor<62, ActionDomain::Work, AdmissionSemantics::Immediate, ReplayPolicy::Never,
                   detail::backgroundJobs, std::tuple<std::string>,
                   std::optional<javelin::jmap::cache::DatabaseError>>;
    using WorkResume =
        Descriptor<63, ActionDomain::Work, AdmissionSemantics::Immediate, ReplayPolicy::Never,
                   detail::backgroundJobs, std::tuple<std::string>,
                   std::optional<javelin::jmap::cache::DatabaseError>>;
    using WorkRetry =
        Descriptor<64, ActionDomain::Work, AdmissionSemantics::Immediate, ReplayPolicy::Never,
                   detail::backgroundJobs, std::tuple<std::string>,
                   std::optional<javelin::jmap::cache::DatabaseError>>;
    using WorkList = Descriptor<
        65, ActionDomain::Work, AdmissionSemantics::Immediate, ReplayPolicy::Reexecute, 0,
        std::tuple<>,
        std::variant<std::vector<javelin::app::WorkRecord>, javelin::jmap::cache::DatabaseError>>;
    using WorkSummary = Descriptor<66, ActionDomain::Work, AdmissionSemantics::Immediate,
                                   ReplayPolicy::Reexecute, 0, std::tuple<>, QString>;

    using WorkActionTypes =
        std::tuple<RegisteredAction<WorkPause, "WorkPause">,
                   RegisteredAction<WorkResume, "WorkResume">,
                   RegisteredAction<WorkRetry, "WorkRetry">, RegisteredAction<WorkList, "WorkList">,
                   RegisteredAction<WorkSummary, "WorkSummary">>;
} // namespace javelin::protocol::actions
