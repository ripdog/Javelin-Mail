#include "jmap/sync/SyncReconciler.h"

namespace javelin::jmap::sync
{

    SyncReconciler::SyncReconciler(javelin::jmap::cache::MailboxRepository& mailboxRepository,
                                   javelin::jmap::cache::EmailRepository& emailRepository,
                                   javelin::jmap::cache::SyncStateRepository& syncStateRepository)
        : m_mailboxRepository(mailboxRepository), m_emailRepository(emailRepository),
          m_syncStateRepository(syncStateRepository)
    {
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    SyncReconciler::applyMailboxChanges(const javelin::jmap::cache::SyncStateKey& key,
                                        const javelin::jmap::api::ChangesResponse& changes,
                                        const javelin::jmap::api::MailboxGetResponse& fetched) const
    {
        if (const auto error = m_mailboxRepository.upsertMany(key.accountId, fetched.list))
        {
            return error;
        }

        if (const auto error = m_mailboxRepository.removeMany(key.accountId, changes.destroyed))
        {
            return error;
        }

        return m_syncStateRepository.upsert(key, changes.newState);
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    SyncReconciler::applyEmailChanges(const javelin::jmap::cache::SyncStateKey& key,
                                      const javelin::jmap::api::ChangesResponse& changes,
                                      const javelin::jmap::api::EmailGetResponse& fetched) const
    {
        if (const auto error = m_emailRepository.upsertMany(key.accountId, fetched.list))
        {
            return error;
        }

        if (const auto error = m_emailRepository.removeMany(key.accountId, changes.destroyed))
        {
            return error;
        }

        return m_syncStateRepository.upsert(key, changes.newState);
    }

} // namespace javelin::jmap::sync
