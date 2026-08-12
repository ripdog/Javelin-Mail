#pragma once

#include "app/IdentityApplicationPorts.h"
#include "protocol/actions/ActionDescriptor.h"

#include <string>
#include <tuple>

namespace javelin::protocol::actions
{
    namespace detail
    {
        constexpr auto senderIdentities = changedDomains(ChangedDomain::SenderIdentities);
    }

    using IdentityList = Descriptor<44, ActionDomain::Identity, AdmissionSemantics::Asynchronous,
                                    ReplayPolicy::Reexecute, 0, std::tuple<std::string>,
                                    javelin::jmap::identity::IdentityListResult>;
    using IdentitySave = Descriptor<45, ActionDomain::Identity, AdmissionSemantics::Asynchronous,
                                    ReplayPolicy::Never, detail::senderIdentities,
                                    std::tuple<std::string, javelin::jmap::domain::Identity>,
                                    javelin::jmap::identity::IdentitySaveResult>;
    using IdentityDelete = Descriptor<46, ActionDomain::Identity, AdmissionSemantics::Asynchronous,
                                      ReplayPolicy::Never, detail::senderIdentities,
                                      std::tuple<std::string, std::string>,
                                      javelin::jmap::identity::IdentityDeleteResult>;

    using IdentityActionTypes = std::tuple<IdentityList, IdentitySave, IdentityDelete>;
} // namespace javelin::protocol::actions
