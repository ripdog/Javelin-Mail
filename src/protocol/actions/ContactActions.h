#pragma once

#include "app/ContactApplicationPorts.h"
#include "protocol/actions/ActionDescriptor.h"
#include <QByteArray>
#include <string>
#include <tuple>

namespace javelin::protocol::actions
{
    namespace detail
    {
        constexpr auto contactsOnly = changedDomains(ChangedDomain::Contacts);
        constexpr auto contactsHistory =
            changedDomains(ChangedDomain::Contacts, ChangedDomain::History);
    } // namespace detail

    using ContactRequestRefresh =
        Descriptor<19, ActionDomain::Contact, AdmissionSemantics::Asynchronous,
                   ReplayPolicy::Reexecute, detail::contactsOnly, std::tuple<std::string>,
                   javelin::jmap::contacts::ContactRefreshResult>;
    using ContactMutateAddressBook =
        Descriptor<20, ActionDomain::Contact, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
                   detail::contactsHistory,
                   std::tuple<std::string, javelin::app::AddressBookCommand>,
                   javelin::jmap::contacts::ContactMutationResult>;
    using ContactSave = Descriptor<21, ActionDomain::Contact, AdmissionSemantics::Asynchronous,
                                   ReplayPolicy::Never, detail::contactsHistory,
                                   std::tuple<std::string, javelin::app::SaveContactCommand>,
                                   javelin::jmap::contacts::ContactMutationResult>;
    using ContactSetStarred =
        Descriptor<22, ActionDomain::Contact, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
                   detail::contactsHistory,
                   std::tuple<std::string, javelin::app::SetContactsStarredCommand>,
                   javelin::jmap::contacts::ContactMutationResult>;
    using ContactDelete = Descriptor<23, ActionDomain::Contact, AdmissionSemantics::Asynchronous,
                                     ReplayPolicy::Never, detail::contactsHistory,
                                     std::tuple<std::string, javelin::app::DeleteContactsCommand>,
                                     javelin::jmap::contacts::ContactMutationResult>;
    using ContactCreateGroup =
        Descriptor<24, ActionDomain::Contact, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
                   detail::contactsHistory,
                   std::tuple<std::string, javelin::app::CreateContactGroupCommand>,
                   javelin::jmap::contacts::ContactMutationResult>;
    using ContactDeleteGroup =
        Descriptor<25, ActionDomain::Contact, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
                   detail::contactsHistory,
                   std::tuple<std::string, javelin::app::DeleteContactGroupCommand>,
                   javelin::jmap::contacts::ContactMutationResult>;
    using ContactSetGroupMembership =
        Descriptor<26, ActionDomain::Contact, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
                   detail::contactsHistory,
                   std::tuple<std::string, javelin::app::SetContactGroupMembershipCommand>,
                   javelin::jmap::contacts::ContactMutationResult>;
    using ContactCopy = Descriptor<27, ActionDomain::Contact, AdmissionSemantics::Asynchronous,
                                   ReplayPolicy::Never, detail::contactsHistory,
                                   std::tuple<std::string, javelin::app::CopyContactCommand>,
                                   javelin::jmap::contacts::ContactMutationResult>;
    using ContactImport = Descriptor<28, ActionDomain::Contact, AdmissionSemantics::Asynchronous,
                                     ReplayPolicy::Never, detail::contactsHistory,
                                     std::tuple<std::string, javelin::app::ImportContactsCommand>,
                                     javelin::jmap::contacts::ContactMutationResult>;
    using ContactMerge = Descriptor<29, ActionDomain::Contact, AdmissionSemantics::Asynchronous,
                                    ReplayPolicy::Never, detail::contactsHistory,
                                    std::tuple<std::string, javelin::app::MergeContactsCommand>,
                                    javelin::jmap::contacts::ContactMutationResult>;
    using ContactUploadMedia =
        Descriptor<30, ActionDomain::Contact, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
                   0, std::tuple<std::string, std::string, QByteArray, std::string>,
                   javelin::jmap::contacts::ContactUploadResult>;
    using ContactDownloadMedia =
        Descriptor<31, ActionDomain::Contact, AdmissionSemantics::Asynchronous,
                   ReplayPolicy::Reexecute, 0,
                   std::tuple<std::string, std::string, std::string, std::string>,
                   javelin::jmap::contacts::ContactDownloadResult>;

    using ContactActionTypes =
        std::tuple<RegisteredAction<ContactRequestRefresh, "ContactRequestRefresh">,
                   RegisteredAction<ContactMutateAddressBook, "ContactMutateAddressBook">,
                   RegisteredAction<ContactSave, "ContactSave">,
                   RegisteredAction<ContactSetStarred, "ContactSetStarred">,
                   RegisteredAction<ContactDelete, "ContactDelete">,
                   RegisteredAction<ContactCreateGroup, "ContactCreateGroup">,
                   RegisteredAction<ContactDeleteGroup, "ContactDeleteGroup">,
                   RegisteredAction<ContactSetGroupMembership, "ContactSetGroupMembership">,
                   RegisteredAction<ContactCopy, "ContactCopy">,
                   RegisteredAction<ContactImport, "ContactImport">,
                   RegisteredAction<ContactMerge, "ContactMerge">,
                   RegisteredAction<ContactUploadMedia, "ContactUploadMedia">,
                   RegisteredAction<ContactDownloadMedia, "ContactDownloadMedia">>;
} // namespace javelin::protocol::actions
