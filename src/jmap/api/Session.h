#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace javelin::jmap::api
{

    constexpr std::string_view coreCapabilityUri = "urn:ietf:params:jmap:core";
    constexpr std::string_view mailCapabilityUri = "urn:ietf:params:jmap:mail";
    constexpr std::string_view submissionCapabilityUri = "urn:ietf:params:jmap:submission";

    struct CoreCapability
    {
        std::optional<std::uint64_t> maxSizeUpload;
        std::optional<std::uint64_t> maxConcurrentUpload;
        std::optional<std::uint64_t> maxConcurrentRequests;
        std::optional<std::uint64_t> maxCallsInRequest;
        std::optional<std::uint64_t> maxObjectsInGet;
        std::optional<std::uint64_t> maxObjectsInSet;
        std::vector<std::string> collationAlgorithms;
    };

    struct SessionCapabilities
    {
        bool core = false;
        std::optional<CoreCapability> coreDetails;
        bool mail = false;
        bool submission = false;
    };

    struct AccountCapabilities
    {
        bool mail = false;
        bool submission = false;
    };

    struct Account
    {
        std::string id;
        std::string name;
        bool isPersonal = false;
        bool isReadOnly = false;
        AccountCapabilities accountCapabilities;
    };

    struct PrimaryAccounts
    {
        std::optional<std::string> mailAccountId;
        std::optional<std::string> submissionAccountId;
    };

    struct Session
    {
        std::string username;
        std::string apiUrl;
        std::string downloadUrl;
        std::string uploadUrl;
        std::optional<std::string> eventSourceUrl;
        std::string state;
        SessionCapabilities capabilities;
        std::unordered_map<std::string, Account> accounts;
        PrimaryAccounts primaryAccounts;
    };

    enum class CapabilityError
    {
        MissingCoreCapability,
        MissingMailCapability,
        MissingSubmissionCapability,
        MissingPrimaryMailAccount,
        MissingPrimarySubmissionAccount,
        MissingMailAccountCapability,
        MissingSubmissionAccountCapability,
    };

    struct RequiredCapabilities
    {
        bool mail = false;
        bool submission = false;
    };

    struct CapabilityValidationResult
    {
        std::vector<CapabilityError> errors;

        [[nodiscard]] bool ok() const;
    };

    [[nodiscard]] CapabilityValidationResult
    validateSessionCapabilities(const Session& session, const RequiredCapabilities& required);

    [[nodiscard]] std::string_view toString(CapabilityError error);

} // namespace javelin::jmap::api
