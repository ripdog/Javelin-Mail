#pragma once

#include "jmap/api/LiveConnectionSettings.h"
#include "jmap/contacts/ContactResults.h"

#include <QCoroTask>

#include <QByteArray>

#include <string>

namespace javelin::jmap::api
{
    class AbstractTransport;
}
namespace javelin::jmap::cache
{
    class DatabaseConnection;
}
namespace javelin::jmap::contacts
{
    class ContactMediaService
    {
      public:
        ContactMediaService(cache::DatabaseConnection& connection,
                            api::AbstractTransport& resourceTransport);

        [[nodiscard]] QCoro::Task<ContactUploadResult>
        uploadMedia(LiveConnectionSettings settings, std::string ownerAccountId,
                    std::string accountId, QByteArray payload, std::string mediaType);
        [[nodiscard]] QCoro::Task<ContactDownloadResult>
        downloadMedia(LiveConnectionSettings settings, std::string ownerAccountId,
                      std::string accountId, std::string blobId, std::string mediaType);

      private:
        cache::DatabaseConnection& m_connection;
        api::AbstractTransport& m_resourceTransport;
    };
} // namespace javelin::jmap::contacts
