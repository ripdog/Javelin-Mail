#pragma once

#include "protocol/actions/ActionDescriptor.h"

#include "app/MailApplicationPorts.h"
#include "app/MessageContentApplicationPorts.h"
#include "app/MessageListMaterializationPort.h"

#include <QString>

#include <optional>
#include <string>
#include <tuple>

namespace javelin::protocol::actions
{
    namespace detail
    {
        constexpr auto mailMutation =
            changedDomains(ChangedDomain::MailQueryWindows, ChangedDomain::MessageMetadata,
                           ChangedDomain::History);
        constexpr auto mailMetadata = changedDomains(ChangedDomain::MessageMetadata);
        constexpr auto mailTagRemoval =
            changedDomains(ChangedDomain::MailQueryWindows, ChangedDomain::MessageMetadata,
                           ChangedDomain::BackgroundJobs);
        constexpr auto mailboxTree = changedDomains(ChangedDomain::MailboxTree);
        constexpr auto messageContent = changedDomains(ChangedDomain::MessageContent);
        constexpr auto messageWindow =
            changedDomains(ChangedDomain::MailQueryWindows, ChangedDomain::MessageMetadata);
    } // namespace detail

    using MailQueueMailboxMutation =
        Descriptor<32, ActionDomain::Mail, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
                   detail::mailMutation, std::tuple<javelin::app::MailboxSelectionMutationIntent>,
                   javelin::app::QueuedMailboxSelectionMutationResult>;
    using MailQueueDestroy = Descriptor<
        33, ActionDomain::Mail, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
        detail::mailMutation,
        std::tuple<std::string, std::optional<std::string>, javelin::app::MessageSelection>,
        javelin::app::QueuedMessageSelectionMutationResult>;
    using MailQueueMarkUnread = Descriptor<
        34, ActionDomain::Mail, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
        detail::mailMutation,
        std::tuple<std::string, std::optional<std::string>, javelin::app::MessageSelection>,
        javelin::app::QueuedMessageSelectionMutationResult>;
    using MailQueueMarkRead =
        Descriptor<35, ActionDomain::Mail, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
                   detail::mailMutation, std::tuple<std::string, std::string>,
                   javelin::app::QueuedMessageSelectionMutationResult>;
    using MailQueueSetFlagged =
        Descriptor<36, ActionDomain::Mail, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
                   detail::mailMutation, std::tuple<std::string, std::string, bool>,
                   javelin::app::QueuedMessageSelectionMutationResult>;
    using MailSubmitPending =
        Descriptor<37, ActionDomain::Mail, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
                   detail::mailMutation, std::tuple<std::string, std::optional<std::string>>,
                   javelin::jmap::SubmittedEmailMutationsResult>;
    using MessageContent = Descriptor<48, ActionDomain::Mail, AdmissionSemantics::Asynchronous,
                                      ReplayPolicy::Reexecute, detail::messageContent,
                                      std::tuple<std::string, std::string>,
                                      javelin::jmap::MessageContentRefreshResult>;
    using AttachmentDownload = Descriptor<49, ActionDomain::Mail, AdmissionSemantics::Asynchronous,
                                          ReplayPolicy::Reexecute, detail::messageContent,
                                          std::tuple<std::string, std::string, std::string>,
                                          javelin::jmap::AttachmentDownloadResult>;
    using MessageSource = Descriptor<50, ActionDomain::Mail, AdmissionSemantics::Asynchronous,
                                     ReplayPolicy::Reexecute, detail::messageContent,
                                     std::tuple<std::string, std::string>,
                                     javelin::jmap::MessageSourceDownloadResult>;
    using MailboxObserve =
        Descriptor<51, ActionDomain::Mail, AdmissionSemantics::Immediate, ReplayPolicy::Never, 0,
                   std::tuple<QString, std::string, std::string>, std::monostate>;
    using MailboxUnobserve =
        Descriptor<52, ActionDomain::Mail, AdmissionSemantics::Immediate, ReplayPolicy::Never, 0,
                   std::tuple<QString>, std::monostate>;
    using MailboxWindow = Descriptor<53, ActionDomain::Mail, AdmissionSemantics::Asynchronous,
                                     ReplayPolicy::Reexecute, detail::messageWindow,
                                     std::tuple<javelin::app::MailboxWindowIntent>,
                                     javelin::app::MailboxWindowResult>;
    using SearchWindow =
        Descriptor<54, ActionDomain::Mail, AdmissionSemantics::Asynchronous,
                   ReplayPolicy::Reexecute, detail::messageWindow,
                   std::tuple<javelin::app::SearchWindowIntent>, javelin::app::SearchWindowResult>;
    using SearchRetire =
        Descriptor<55, ActionDomain::Mail, AdmissionSemantics::Immediate, ReplayPolicy::Reexecute,
                   detail::messageWindow, std::tuple<std::string, std::string>, std::monostate>;
    using MailQueueSetTag =
        Descriptor<80, ActionDomain::Mail, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
                   detail::mailMutation,
                   std::tuple<std::string, std::optional<std::string>,
                              javelin::app::MessageSelection, std::string, bool>,
                   javelin::app::QueuedMessageSelectionMutationResult>;
    using MailSaveTagDefinition =
        Descriptor<81, ActionDomain::Mail, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
                   detail::mailMetadata, std::tuple<javelin::app::SaveMailTagDefinition>,
                   javelin::app::SaveMailTagDefinitionResult>;
    using MailDeleteTag =
        Descriptor<82, ActionDomain::Mail, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
                   detail::mailTagRemoval, std::tuple<std::string, std::string>,
                   javelin::app::QueuedMailTagDeletionResult>;
    using MailSetMailboxSubscribed =
        Descriptor<83, ActionDomain::Mail, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
                   detail::mailboxTree, std::tuple<std::string, std::string, bool>,
                   javelin::jmap::MailboxSubscriptionChangeResult>;
    using MailCreateMailbox =
        Descriptor<84, ActionDomain::Mail, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
                   detail::mailboxTree, std::tuple<std::string, std::string>,
                   javelin::jmap::MailboxCreateResult>;
    using MailDestroyMailbox =
        Descriptor<85, ActionDomain::Mail, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
                   detail::mailboxTree, std::tuple<std::string, std::string>,
                   javelin::jmap::MailboxDestroyResult>;
    using MailQueueSetSelectionFlagged = Descriptor<
        87, ActionDomain::Mail, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
        detail::mailMutation,
        std::tuple<std::string, std::optional<std::string>, javelin::app::MessageSelection, bool>,
        javelin::app::QueuedMessageSelectionMutationResult>;
    using ThreadEnsure =
        Descriptor<88, ActionDomain::Mail, AdmissionSemantics::Immediate, ReplayPolicy::Reexecute,
                   detail::messageWindow, std::tuple<javelin::app::ThreadMaterializationIntent>,
                   std::monostate>;

    using MailActionTypes = std::tuple<
        MailQueueMailboxMutation, MailQueueDestroy, MailQueueMarkUnread, MailQueueMarkRead,
        MailQueueSetFlagged, MailSubmitPending, MessageContent, AttachmentDownload, MessageSource,
        MailboxObserve, MailboxUnobserve, MailboxWindow, SearchWindow, SearchRetire,
        MailQueueSetTag, MailSaveTagDefinition, MailDeleteTag, MailSetMailboxSubscribed,
        MailCreateMailbox, MailDestroyMailbox, MailQueueSetSelectionFlagged, ThreadEnsure>;
} // namespace javelin::protocol::actions
