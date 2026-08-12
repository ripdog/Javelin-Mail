#pragma once

#include "protocol/actions/ActionDescriptor.h"

#include "app/AccountRefreshApplicationPorts.h"
#include "app/OnboardingTypes.h"
#include "storage/DatabaseError.h"

#include <QString>
#include <QStringList>

#include <optional>
#include <tuple>
#include <variant>

namespace javelin::protocol::actions
{
    namespace detail
    {
        constexpr auto accountBootstrapDomains = changedDomains(
            ChangedDomain::MailboxTree, ChangedDomain::MailQueryWindows,
            ChangedDomain::MessageMetadata, ChangedDomain::Contacts, ChangedDomain::Calendars);
        constexpr auto removeAccountDomains = changedDomains(
            ChangedDomain::MailboxTree, ChangedDomain::MailQueryWindows,
            ChangedDomain::MessageMetadata, ChangedDomain::MessageContent, ChangedDomain::Contacts,
            ChangedDomain::Calendars, ChangedDomain::History, ChangedDomain::BackgroundJobs);
        constexpr auto reloadSettingsDomains =
            changedDomains(ChangedDomain::MailboxTree, ChangedDomain::BackgroundJobs);
    } // namespace detail

    using RemoveConfiguredAccount =
        Descriptor<0, ActionDomain::Account, AdmissionSemantics::Immediate, ReplayPolicy::Never,
                   detail::removeAccountDomains, std::tuple<QString, QString, QStringList>,
                   std::optional<javelin::jmap::cache::DatabaseError>>;

    using AccountBootstrap = Descriptor<47, ActionDomain::Account, AdmissionSemantics::Asynchronous,
                                        ReplayPolicy::Reexecute, detail::accountBootstrapDomains,
                                        std::tuple<javelin::app::AccountBootstrapIntent>,
                                        javelin::jmap::LiveRefreshResult>;

    using ReloadSettings =
        Descriptor<61, ActionDomain::Account, AdmissionSemantics::Immediate, ReplayPolicy::Never,
                   detail::reloadSettingsDomains, std::tuple<>, std::monostate>;

    using OnboardingDiscover =
        Descriptor<67, ActionDomain::Account, AdmissionSemantics::Asynchronous,
                   ReplayPolicy::Reexecute, 0, std::tuple<javelin::app::AccountDiscoveryRequest>,
                   javelin::app::AccountDiscoveryResult>;
    using OnboardingStartOAuth =
        Descriptor<68, ActionDomain::Account, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
                   0, std::tuple<javelin::app::OAuthStartRequest>, javelin::app::OAuthStartResult>;
    using OnboardingFinishOAuth =
        Descriptor<69, ActionDomain::Account, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
                   0, std::tuple<javelin::app::OAuthFinishRequest>,
                   javelin::app::AccountAuthenticationResult>;
    using OnboardingAuthenticateManually =
        Descriptor<70, ActionDomain::Account, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
                   0, std::tuple<javelin::app::ManualAuthenticationRequest>,
                   javelin::app::AccountAuthenticationResult>;
    using OnboardingRevokeOAuth =
        Descriptor<71, ActionDomain::Account, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
                   0, std::tuple<javelin::app::OAuthRevocationRequest>,
                   javelin::app::OAuthRevocationResult>;
    using OnboardingCancelOAuth =
        Descriptor<72, ActionDomain::Account, AdmissionSemantics::Asynchronous, ReplayPolicy::Never,
                   0, std::tuple<javelin::app::OAuthCancelRequest>,
                   javelin::app::OAuthCancelResult>;

    using AccountActionTypes =
        std::tuple<RemoveConfiguredAccount, AccountBootstrap, ReloadSettings, OnboardingDiscover,
                   OnboardingStartOAuth, OnboardingFinishOAuth, OnboardingAuthenticateManually,
                   OnboardingRevokeOAuth, OnboardingCancelOAuth>;
} // namespace javelin::protocol::actions
