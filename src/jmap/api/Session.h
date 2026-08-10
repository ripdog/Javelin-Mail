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
    constexpr std::string_view contactsCapabilityUri = "urn:ietf:params:jmap:contacts";
    constexpr std::string_view calendarsCapabilityUri = "urn:ietf:params:jmap:calendars";
    constexpr std::string_view sieveCapabilityUri = "urn:ietf:params:jmap:sieve";
    constexpr std::string_view websocketCapabilityUri = "urn:ietf:params:jmap:websocket";

    struct WebSocketCapability
    {
        std::string url;
        bool supportsPush = false;
    };

    struct CoreCapability
    {
        std::optional<std::uint64_t> maxSizeUpload;
        std::optional<std::uint64_t> maxConcurrentUpload;
        std::optional<std::uint64_t> maxSizeRequest;
        std::optional<std::uint64_t> maxConcurrentRequests;
        std::optional<std::uint64_t> maxCallsInRequest;
        std::optional<std::uint64_t> maxObjectsInGet;
        std::optional<std::uint64_t> maxObjectsInSet;
        std::vector<std::string> collationAlgorithms;
    };

    struct CoreRequestLimits
    {
        std::uint64_t maxSizeRequest = 0;
        std::uint64_t maxConcurrentRequests = 0;
        std::uint64_t maxCallsInRequest = 0;
        std::uint64_t maxObjectsInGet = 0;
        std::uint64_t maxObjectsInSet = 0;
    };

    struct SessionCapabilities
    {
        bool core = false;
        std::optional<CoreCapability> coreDetails;
        bool mail = false;
        bool submission = false;
        bool contacts = false;
        bool calendars = false;
        bool sieve = false;
        std::optional<WebSocketCapability> websocket;
    };

    struct MailAccountCapability
    {
        bool mayCreateTopLevelMailbox = false;
    };

    struct SubmissionCapability
    {
        std::uint64_t maxDelayedSend = 0;
    };

    struct ContactsCapability
    {
        std::optional<std::uint64_t> maxAddressBooksPerCard;
        bool mayCreateAddressBook = false;
    };

    struct CalendarsCapability
    {
        std::optional<std::uint64_t> maxCalendarsPerEvent;
        std::string minDateTime;
        std::string maxDateTime;
        std::string maxExpandedQueryDuration;
        std::optional<std::uint64_t> maxParticipantsPerEvent;
        bool mayCreateCalendar = false;
    };

    struct AccountCapabilities
    {
        bool mail = false;
        std::optional<MailAccountCapability> mailDetails = std::nullopt;
        std::optional<SubmissionCapability> submission;
        std::optional<ContactsCapability> contacts;
        std::optional<CalendarsCapability> calendars;
        bool sieve = false;
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
        std::optional<std::string> contactsAccountId;
        std::optional<std::string> calendarsAccountId;
        std::optional<std::string> sieveAccountId;
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
        InvalidCoreCapability,
        MissingMailCapability,
        MissingSubmissionCapability,
        MissingPrimaryMailAccount,
        MissingPrimarySubmissionAccount,
        MissingMailAccountCapability,
        MissingSubmissionAccountCapability,
        MissingCalendarsCapability,
        MissingPrimaryCalendarsAccount,
        MissingCalendarsAccountCapability,
        MissingSieveCapability,
        MissingPrimarySieveAccount,
        MissingSieveAccountCapability,
    };

    struct RequiredCapabilities
    {
        bool mail = false;
        bool submission = false;
        bool calendars = false;
        bool sieve = false;
    };

    struct CapabilityValidationResult
    {
        std::vector<CapabilityError> errors;

        [[nodiscard]] bool ok() const;
    };

    [[nodiscard]] CapabilityValidationResult
    validateSessionCapabilities(const Session& session, const RequiredCapabilities& required);
    [[nodiscard]] std::optional<CoreRequestLimits> coreRequestLimits(const Session& session);

    [[nodiscard]] std::string_view toString(CapabilityError error);

} // namespace javelin::jmap::api
