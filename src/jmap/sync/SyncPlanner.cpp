#include "jmap/sync/SyncPlanner.h"

namespace javelin::jmap::sync
{

    SyncPlanner::SyncPlanner(const javelin::jmap::cache::SyncStateRepository& syncStateRepository)
        : m_syncStateRepository(syncStateRepository)
    {
    }

    std::variant<SyncPlan, javelin::jmap::cache::DatabaseError>
    SyncPlanner::plan(const javelin::jmap::cache::SyncStateKey& key) const
    {
        const auto stateResult = m_syncStateRepository.find(key);
        if (std::holds_alternative<javelin::jmap::cache::DatabaseError>(stateResult))
        {
            return std::get<javelin::jmap::cache::DatabaseError>(stateResult);
        }

        const auto& stateRecord =
            std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(stateResult);
        if (!stateRecord.has_value())
        {
            return SyncPlan{
                .key = key,
                .kind = SyncPlanKind::InitialFetch,
                .sinceState = std::nullopt,
            };
        }

        return SyncPlan{
            .key = key,
            .kind = SyncPlanKind::IncrementalChanges,
            .sinceState = stateRecord->stateToken,
        };
    }

    std::variant<std::vector<SyncPlan>, javelin::jmap::cache::DatabaseError>
    SyncPlanner::planMany(const std::span<const javelin::jmap::cache::SyncStateKey> keys) const
    {
        std::vector<SyncPlan> plans;
        plans.reserve(keys.size());

        for (const auto& key : keys)
        {
            const auto planResult = plan(key);
            if (std::holds_alternative<javelin::jmap::cache::DatabaseError>(planResult))
            {
                return std::get<javelin::jmap::cache::DatabaseError>(planResult);
            }

            plans.push_back(std::get<SyncPlan>(planResult));
        }

        return plans;
    }

} // namespace javelin::jmap::sync
