#pragma once

#include "protocol/actions/ActionDescriptor.h"

#include "app/RemoteActionTypes.h"
#include "app/UndoApplicationPorts.h"
#include "storage/DatabaseError.h"

#include <QString>

#include <optional>
#include <tuple>
#include <vector>

namespace javelin::protocol::actions
{
    namespace detail
    {
        constexpr auto historyAll = changedDomains(
            ChangedDomain::MailboxTree, ChangedDomain::MailQueryWindows,
            ChangedDomain::MessageMetadata, ChangedDomain::MessageContent, ChangedDomain::Contacts,
            ChangedDomain::Calendars, ChangedDomain::History);
        constexpr auto historyOnly = changedDomains(ChangedDomain::History);
    } // namespace detail

    using Undo =
        Descriptor<56, ActionDomain::History, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
                   detail::historyAll, std::tuple<>, javelin::app::RemoteUndoExecutionResult>;
    using Redo =
        Descriptor<57, ActionDomain::History, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
                   detail::historyAll, std::tuple<>, javelin::app::RemoteUndoExecutionResult>;
    using UndoAcknowledgeRemove =
        Descriptor<58, ActionDomain::History, AdmissionSemantics::Immediate, ReplayPolicy::Never,
                   detail::historyOnly, std::tuple<QString>,
                   std::optional<javelin::jmap::cache::DatabaseError>>;
    using UndoForget = Descriptor<59, ActionDomain::History, AdmissionSemantics::Immediate,
                                  ReplayPolicy::Never, detail::historyOnly, std::tuple<QString>,
                                  std::optional<javelin::jmap::cache::DatabaseError>>;
    using UndoSnapshot = Descriptor<60, ActionDomain::History, AdmissionSemantics::Immediate,
                                    ReplayPolicy::Reexecute, 0, std::tuple<>,
                                    std::tuple<javelin::app::undo::HistoryState,
                                               std::vector<javelin::app::undo::HistoryEntry>>>;

    using HistoryActionTypes =
        std::tuple<RegisteredAction<Undo, "Undo">, RegisteredAction<Redo, "Redo">,
                   RegisteredAction<UndoAcknowledgeRemove, "UndoAcknowledgeRemove">,
                   RegisteredAction<UndoForget, "UndoForget">,
                   RegisteredAction<UndoSnapshot, "UndoSnapshot">>;
} // namespace javelin::protocol::actions
