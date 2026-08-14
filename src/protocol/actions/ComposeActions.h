#pragma once

#include "protocol/actions/ActionDescriptor.h"

#include "app/ComposeApplicationPorts.h"

#include <optional>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace javelin::protocol::actions
{
    namespace detail
    {
        constexpr auto composeContent = changedDomains(ChangedDomain::MessageContent);
        constexpr auto composeSend =
            changedDomains(ChangedDomain::MailQueryWindows, ChangedDomain::MessageMetadata,
                           ChangedDomain::MessageContent, ChangedDomain::History);
    } // namespace detail

    using ComposeOpen = Descriptor<
        12, ActionDomain::Compose, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
        detail::composeContent,
        std::tuple<javelin::app::AccountConnectionSettings,
                   javelin::jmap::submission::OpenComposeRequest>,
        std::variant<javelin::jmap::submission::DraftSnapshot, javelin::jmap::OperationError>>;
    using ComposeLoadSenderIdentities = Descriptor<
        13, ActionDomain::Compose, AdmissionSemantics::Asynchronous, ReplayPolicy::Reexecute, 0,
        std::tuple<javelin::app::AccountConnectionSettings, std::string>,
        std::variant<std::vector<javelin::jmap::domain::Identity>, javelin::jmap::OperationError>>;
    using ComposeSaveDraft = Descriptor<
        14, ActionDomain::Compose, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
        detail::composeSend,
        std::tuple<javelin::app::AccountConnectionSettings,
                   javelin::jmap::submission::DraftSnapshot>,
        std::variant<javelin::jmap::submission::DraftSaveSummary, javelin::jmap::OperationError>>;
    using ComposeSend = Descriptor<
        15, ActionDomain::Compose, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
        detail::composeSend,
        std::tuple<javelin::app::AccountConnectionSettings,
                   javelin::jmap::submission::DraftSnapshot>,
        std::variant<javelin::jmap::submission::SendSummary, javelin::jmap::OperationError>>;
    using ComposeLoadWorkingCopy =
        Descriptor<16, ActionDomain::Compose, AdmissionSemantics::Immediate,
                   ReplayPolicy::Reexecute, 0, std::tuple<std::string>,
                   std::variant<std::optional<javelin::jmap::submission::DraftSnapshot>,
                                javelin::jmap::OperationError>>;
    using ComposeStoreWorkingCopy =
        Descriptor<17, ActionDomain::Compose, AdmissionSemantics::Immediate,
                   ReplayPolicy::Reexecute, detail::composeContent,
                   std::tuple<javelin::jmap::submission::DraftSnapshot>,
                   std::optional<javelin::jmap::OperationError>>;
    using ComposeDiscard =
        Descriptor<18, ActionDomain::Compose, AdmissionSemantics::Immediate, ReplayPolicy::Never,
                   detail::composeContent, std::tuple<std::string>,
                   std::optional<javelin::jmap::OperationError>>;
    using ComposeScheduleSend = Descriptor<
        86, ActionDomain::Compose, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
        detail::composeSend,
        std::tuple<javelin::app::AccountConnectionSettings,
                   javelin::jmap::submission::ScheduledSendRequest>,
        std::variant<javelin::jmap::submission::SendSummary, javelin::jmap::OperationError>>;
    using ComposeCancelDeferredSend =
        Descriptor<89, ActionDomain::Compose, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
                   detail::composeSend, std::tuple<QString>,
                   std::variant<bool, javelin::jmap::OperationError>>;

    using ComposeActionTypes =
        std::tuple<RegisteredAction<ComposeOpen, "ComposeOpen">,
                   RegisteredAction<ComposeLoadSenderIdentities, "ComposeLoadSenderIdentities">,
                   RegisteredAction<ComposeSaveDraft, "ComposeSaveDraft">,
                   RegisteredAction<ComposeSend, "ComposeSend">,
                   RegisteredAction<ComposeLoadWorkingCopy, "ComposeLoadWorkingCopy">,
                   RegisteredAction<ComposeStoreWorkingCopy, "ComposeStoreWorkingCopy">,
                   RegisteredAction<ComposeDiscard, "ComposeDiscard">,
                   RegisteredAction<ComposeScheduleSend, "ComposeScheduleSend">,
                   RegisteredAction<ComposeCancelDeferredSend, "ComposeCancelDeferredSend">>;
} // namespace javelin::protocol::actions
