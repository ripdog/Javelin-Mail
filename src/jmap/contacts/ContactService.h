#pragma once

#include "jmap/JmapCore.h"
#include "jmap/api/ContactsMethods.h"
#include "jmap/contacts/ContactResults.h"

#include <QCoroTask>

#include <QByteArray>

#include <string>
#include <vector>

namespace javelin::jmap::api
{
    class AbstractTransport;
    class JmapMethodTransport;
} // namespace javelin::jmap::api
namespace javelin::jmap::cache
{
    class ContactRepository;
    class DatabaseConnection;
} // namespace javelin::jmap::cache

namespace javelin::jmap::contacts
{
    using PreparedContactCardMutation =
        std::variant<javelin::jmap::api::ContactCardSetRequest, javelin::jmap::OperationError>;

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

    class ContactService
    {
      public:
        ContactService(javelin::jmap::cache::DatabaseConnection& connection,
                       javelin::jmap::cache::ContactRepository& repository,
                       javelin::jmap::api::AbstractTransport& resourceTransport,
                       javelin::jmap::api::JmapMethodTransport& methodTransport);

        [[nodiscard]] QCoro::Task<ContactRefreshResult>
        refreshAll(javelin::jmap::LiveConnectionSettings settings, std::string ownerAccountId);
        [[nodiscard]] QCoro::Task<ContactMutationResult>
        setAddressBooks(javelin::jmap::LiveConnectionSettings settings, std::string ownerAccountId,
                        javelin::jmap::api::AddressBookSetRequest request);
        [[nodiscard]] QCoro::Task<ContactMutationResult>
        setContactCards(javelin::jmap::LiveConnectionSettings settings, std::string ownerAccountId,
                        javelin::jmap::api::ContactCardSetRequest request);
        [[nodiscard]] PreparedContactCardMutation
        prepareCreateGroup(CreateContactGroupCommand command) const;
        [[nodiscard]] PreparedContactCardMutation
        prepareGroupMembership(SetContactGroupMembershipCommand command) const;
        [[nodiscard]] QCoro::Task<ContactMutationResult>
        createGroup(javelin::jmap::LiveConnectionSettings settings, std::string ownerAccountId,
                    CreateContactGroupCommand command);
        [[nodiscard]] QCoro::Task<ContactMutationResult>
        setGroupMembership(javelin::jmap::LiveConnectionSettings settings,
                           std::string ownerAccountId, SetContactGroupMembershipCommand command);
        [[nodiscard]] QCoro::Task<ContactMutationResult>
        copyContactCards(javelin::jmap::LiveConnectionSettings settings, std::string ownerAccountId,
                         javelin::jmap::api::ContactCardCopyRequest request);
        [[nodiscard]] QCoro::Task<ContactUploadResult>
        uploadMedia(javelin::jmap::LiveConnectionSettings settings, std::string ownerAccountId,
                    std::string accountId, QByteArray payload, std::string mediaType);
        [[nodiscard]] QCoro::Task<ContactDownloadResult>
        downloadMedia(javelin::jmap::LiveConnectionSettings settings, std::string ownerAccountId,
                      std::string accountId, std::string blobId, std::string mediaType);

      private:
        javelin::jmap::cache::DatabaseConnection& m_connection;
        javelin::jmap::cache::ContactRepository& m_repository;
        javelin::jmap::api::AbstractTransport& m_resourceTransport;
        javelin::jmap::api::JmapMethodTransport& m_methodTransport;
    };
} // namespace javelin::jmap::contacts
