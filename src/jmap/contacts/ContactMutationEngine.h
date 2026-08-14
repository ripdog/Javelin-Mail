#pragma once

#include "jmap/api/ContactsMethods.h"
#include "jmap/api/LiveConnectionSettings.h"
#include "jmap/contacts/ContactResults.h"

#include <QCoroTask>

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{
    class ContactRepository;
    class DatabaseConnection;
} // namespace javelin::jmap::cache
namespace javelin::jmap::contacts
{
    class ContactProtocolClient;
    class ContactSyncEngine;

    using PreparedContactCardMutation = std::variant<api::ContactCardSetRequest, OperationError>;

    struct ContactSetOptions
    {
        bool refreshAndRetryStateMismatch = false;
        std::optional<std::string> traceId = std::nullopt;
    };

    struct CreateContactGroupCommand
    {
        std::string accountId;
        std::string addressBookId;
        std::string name;
    };

    struct SetContactGroupMembershipCommand
    {
        std::string accountId;
        std::string groupId;
        std::vector<std::string> memberUids;
        bool included = true;
    };

    class ContactMutationEngine
    {
      public:
        ContactMutationEngine(cache::DatabaseConnection& connection,
                              cache::ContactRepository& repository,
                              ContactProtocolClient& protocolClient, ContactSyncEngine& syncEngine);

        [[nodiscard]] QCoro::Task<ContactMutationResult>
        setAddressBooks(LiveConnectionSettings settings, std::string ownerAccountId,
                        api::AddressBookSetRequest request);
        [[nodiscard]] QCoro::Task<ContactMutationResult>
        setContactCards(LiveConnectionSettings settings, std::string ownerAccountId,
                        api::ContactCardSetRequest request, ContactSetOptions options = {});
        [[nodiscard]] PreparedContactCardMutation
        prepareCreateGroup(CreateContactGroupCommand command) const;
        [[nodiscard]] PreparedContactCardMutation
        prepareGroupMembership(SetContactGroupMembershipCommand command) const;
        [[nodiscard]] QCoro::Task<ContactMutationResult>
        createGroup(LiveConnectionSettings settings, std::string ownerAccountId,
                    CreateContactGroupCommand command);
        [[nodiscard]] QCoro::Task<ContactMutationResult>
        setGroupMembership(LiveConnectionSettings settings, std::string ownerAccountId,
                           SetContactGroupMembershipCommand command);
        [[nodiscard]] QCoro::Task<ContactMutationResult>
        copyContactCards(LiveConnectionSettings settings, std::string ownerAccountId,
                         api::ContactCardCopyRequest request);

      private:
        cache::DatabaseConnection& m_connection;
        cache::ContactRepository& m_repository;
        ContactProtocolClient& m_protocolClient;
        ContactSyncEngine& m_syncEngine;
    };
} // namespace javelin::jmap::contacts
