#pragma once

#include "app/SieveApplicationPorts.h"
#include "protocol/actions/ActionDescriptor.h"

#include <QByteArray>
#include <string>
#include <tuple>

namespace javelin::protocol::actions
{
    namespace detail
    {
        constexpr auto sieveHistory = changedDomains(ChangedDomain::History);
    }

    using SieveList = Descriptor<38, ActionDomain::Sieve, AdmissionSemantics::Asynchronous,
                                 ReplayPolicy::Reexecute, 0, std::tuple<std::string>,
                                 javelin::jmap::sieve::SieveListResult>;
    using SieveGet = Descriptor<39, ActionDomain::Sieve, AdmissionSemantics::Asynchronous,
                                ReplayPolicy::Reexecute, 0,
                                std::tuple<std::string, javelin::jmap::sieve::SieveScript>,
                                javelin::jmap::sieve::SieveContentResult>;
    using SieveValidate =
        Descriptor<40, ActionDomain::Sieve, AdmissionSemantics::Asynchronous,
                   ReplayPolicy::Reexecute, 0, std::tuple<std::string, QByteArray>,
                   javelin::jmap::sieve::SieveValidationResult>;
    using SieveSave = Descriptor<41, ActionDomain::Sieve, AdmissionSemantics::Asynchronous,
                                 ReplayPolicy::Never, detail::sieveHistory,
                                 std::tuple<std::string, javelin::jmap::sieve::SieveScript,
                                            QByteArray, javelin::app::undo::CommandOrigin>,
                                 javelin::jmap::sieve::SieveSaveResult>;
    using SieveDelete = Descriptor<42, ActionDomain::Sieve, AdmissionSemantics::Asynchronous,
                                   ReplayPolicy::Never, detail::sieveHistory,
                                   std::tuple<std::string, javelin::jmap::sieve::SieveScript,
                                              javelin::app::undo::CommandOrigin>,
                                   javelin::jmap::sieve::SieveDeleteResult>;
    using SieveActivate = Descriptor<43, ActionDomain::Sieve, AdmissionSemantics::Asynchronous,
                                     ReplayPolicy::Never, detail::sieveHistory,
                                     std::tuple<std::string, javelin::jmap::sieve::SieveScript,
                                                bool, javelin::app::undo::CommandOrigin>,
                                     javelin::jmap::sieve::SieveActivationResult>;

    using SieveActionTypes =
        std::tuple<SieveList, SieveGet, SieveValidate, SieveSave, SieveDelete, SieveActivate>;
} // namespace javelin::protocol::actions
