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

    using ContactRefreshResult =
        std::variant<ContactRefreshSummary, javelin::jmap::LiveRefreshError>;
    using ContactMutationResult =
        std::variant<ContactMutationSummary, javelin::jmap::LiveRefreshError>;
    using ContactUploadResult = std::variant<UploadedContactMedia, javelin::jmap::LiveRefreshError>;

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
        copyContactCards(javelin::jmap::LiveConnectionSettings settings, std::string ownerAccountId,
                         javelin::jmap::api::ContactCardCopyRequest request);
        [[nodiscard]] QCoro::Task<ContactUploadResult>
        uploadMedia(javelin::jmap::LiveConnectionSettings settings, std::string ownerAccountId,
                    std::string accountId, QByteArray payload, std::string mediaType);

      private:
        javelin::jmap::cache::DatabaseConnection& m_connection;
        javelin::jmap::cache::ContactRepository& m_repository;
        javelin::jmap::api::AbstractTransport& m_resourceTransport;
        javelin::jmap::api::JmapMethodTransport& m_methodTransport;
    };
} // namespace javelin::jmap::contacts
