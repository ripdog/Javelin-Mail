#pragma once

#include "jmap/api/MailMethods.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailboxRepository.h"
#include "jmap/cache/SyncStateRepository.h"

#include <optional>
#include <string_view>

namespace javelin::jmap::sync
{

    class SyncReconciler
    {
      public:
        SyncReconciler(javelin::jmap::cache::MailboxRepository& mailboxRepository,
                       javelin::jmap::cache::EmailRepository& emailRepository,
                       javelin::jmap::cache::SyncStateRepository& syncStateRepository);

        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        applyMailboxChanges(const javelin::jmap::cache::SyncStateKey& key,
                            const javelin::jmap::api::ChangesResponse& changes,
                            const javelin::jmap::api::MailboxGetResponse& fetched) const;

        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        applyEmailChanges(const javelin::jmap::cache::SyncStateKey& key,
                          const javelin::jmap::api::ChangesResponse& changes,
                          const javelin::jmap::api::EmailGetResponse& fetched) const;

      private:
        javelin::jmap::cache::MailboxRepository& m_mailboxRepository;
        javelin::jmap::cache::EmailRepository& m_emailRepository;
        javelin::jmap::cache::SyncStateRepository& m_syncStateRepository;
    };

} // namespace javelin::jmap::sync
