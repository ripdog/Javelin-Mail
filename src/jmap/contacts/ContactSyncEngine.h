#pragma once

#include "jmap/api/LiveConnectionSettings.h"
#include "jmap/contacts/ContactResults.h"

#include <QCoroTask>

#include <string>

namespace javelin::jmap::cache
{
    class ContactRepository;
    class DatabaseConnection;
} // namespace javelin::jmap::cache
namespace javelin::jmap::contacts
{
    class ContactProtocolClient;

    class ContactSyncEngine
    {
      public:
        ContactSyncEngine(cache::DatabaseConnection& connection,
                          cache::ContactRepository& repository,
                          ContactProtocolClient& protocolClient);

        [[nodiscard]] QCoro::Task<ContactRefreshResult> refreshAll(LiveConnectionSettings settings,
                                                                   std::string ownerAccountId);

      private:
        cache::DatabaseConnection& m_connection;
        cache::ContactRepository& m_repository;
        ContactProtocolClient& m_protocolClient;
    };
} // namespace javelin::jmap::contacts
