#pragma once

#include <optional>
#include <string>
#include <vector>

namespace javelin::jmap::sync
{

    struct CommittedDomainState
    {
        std::string accountId;
        std::string dataType;
        std::optional<std::string> oldState;
        std::string newState;
    };

    enum class MutationReconciliationDisposition
    {
        Applied,
        Superseded,
    };

    struct ReconciledMutation
    {
        std::string accountId;
        std::string dataType;
        std::string objectId;
        std::optional<std::string> operationGroupId;
        MutationReconciliationDisposition disposition = MutationReconciliationDisposition::Applied;
    };

    struct MutationCommitReceipt
    {
        std::vector<CommittedDomainState> domains;
        std::vector<std::string> acceptedObjectIds;
        std::vector<std::string> rejectedObjectIds;
        std::vector<std::string> affectedCacheViews;
        bool incompleteMaterialization = false;
    };

} // namespace javelin::jmap::sync
