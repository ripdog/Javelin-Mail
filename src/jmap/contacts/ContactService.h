#pragma once

#include "jmap/JmapCore.h"
#include "jmap/api/ContactsMethods.h"

#include <QCoroTask>

#include <QByteArray>

#include <cstddef>
#include <optional>
#include <string>
#include <variant>

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
    struct ContactRefreshSummary
    {
        std::size_t accountCount = 0;
        std::size_t addressBookCount = 0;
        std::size_t contactCount = 0;
    };

    struct ContactMutationSummary
    {
        std::string accountId;
        std::string newState;
        std::optional<std::string> createdId;
    };

    struct UploadedContactMedia
    {
        std::string accountId;
        std::string blobId;
        std::string mediaType;
        std::uint64_t size = 0;
    };

    struct DownloadedContactMedia
    {
        QByteArray data;
        std::string mediaType;
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
        std::string memberUid;
        bool included = true;
    };

    using ContactRefreshResult = std::variant<ContactRefreshSummary, javelin::jmap::OperationError>;
    using ContactMutationResult =
        std::variant<ContactMutationSummary, javelin::jmap::OperationError>;
    using ContactUploadResult = std::variant<UploadedContactMedia, javelin::jmap::OperationError>;
    using ContactDownloadResult =
        std::variant<DownloadedContactMedia, javelin::jmap::OperationError>;

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
