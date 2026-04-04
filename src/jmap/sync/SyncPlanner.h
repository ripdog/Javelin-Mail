#pragma once

#include "jmap/cache/SyncStateRepository.h"

#include <span>
#include <variant>
#include <vector>

namespace javelin::jmap::sync
{

    enum class SyncPlanKind
    {
        InitialFetch,
        IncrementalChanges,
    };

    struct SyncPlan
    {
        javelin::jmap::cache::SyncStateKey key;
        SyncPlanKind kind = SyncPlanKind::InitialFetch;
        std::optional<std::string> sinceState;
    };

    class SyncPlanner
    {
      public:
        explicit SyncPlanner(const javelin::jmap::cache::SyncStateRepository& syncStateRepository);

        [[nodiscard]] std::variant<SyncPlan, javelin::jmap::cache::DatabaseError>
        plan(const javelin::jmap::cache::SyncStateKey& key) const;
        [[nodiscard]] std::variant<std::vector<SyncPlan>, javelin::jmap::cache::DatabaseError>
        planMany(std::span<const javelin::jmap::cache::SyncStateKey> keys) const;

      private:
        const javelin::jmap::cache::SyncStateRepository& m_syncStateRepository;
    };

} // namespace javelin::jmap::sync
