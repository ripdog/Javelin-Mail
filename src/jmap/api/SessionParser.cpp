#include "jmap/api/SessionParser.h"

#include <glaze/glaze.hpp>

#include <string>
#include <unordered_map>
#include <utility>

namespace javelin::jmap::api::detail
{

    struct RawAccount
    {
        std::string name;
        bool isPersonal = false;
        bool isReadOnly = false;
        std::unordered_map<std::string, glz::generic> accountCapabilities;
    };

    struct RawSession
    {
        std::string username;
        std::string apiUrl;
        std::string downloadUrl;
        std::string uploadUrl;
        std::optional<std::string> eventSourceUrl;
        std::string state;
        std::unordered_map<std::string, glz::generic> capabilities;
        std::unordered_map<std::string, RawAccount> accounts;
        std::unordered_map<std::string, std::string> primaryAccounts;
    };

} // namespace javelin::jmap::api::detail

template <> struct glz::meta<javelin::jmap::api::CoreCapability>
{
    using T = javelin::jmap::api::CoreCapability;

    static constexpr auto value = glz::object(
        "maxSizeUpload", &T::maxSizeUpload, "maxConcurrentUpload", &T::maxConcurrentUpload,
        "maxConcurrentRequests", &T::maxConcurrentRequests, "maxCallsInRequest",
        &T::maxCallsInRequest, "maxObjectsInGet", &T::maxObjectsInGet, "maxObjectsInSet",
        &T::maxObjectsInSet, "collationAlgorithms", &T::collationAlgorithms);
};

template <> struct glz::meta<javelin::jmap::api::ContactsCapability>
{
    using T = javelin::jmap::api::ContactsCapability;
    static constexpr auto value = glz::object("maxAddressBooksPerCard", &T::maxAddressBooksPerCard,
                                              "mayCreateAddressBook", &T::mayCreateAddressBook);
};

template <> struct glz::meta<javelin::jmap::api::CalendarsCapability>
{
    using T = javelin::jmap::api::CalendarsCapability;
    static constexpr auto value =
        glz::object("maxCalendarsPerEvent", &T::maxCalendarsPerEvent, "minDateTime",
                    &T::minDateTime, "maxDateTime", &T::maxDateTime, "maxExpandedQueryDuration",
                    &T::maxExpandedQueryDuration, "maxParticipantsPerEvent",
                    &T::maxParticipantsPerEvent, "mayCreateCalendar", &T::mayCreateCalendar);
};

template <> struct glz::meta<javelin::jmap::api::WebSocketCapability>
{
    using T = javelin::jmap::api::WebSocketCapability;
    static constexpr auto value = glz::object("url", &T::url, "supportsPush", &T::supportsPush);
};

template <> struct glz::meta<javelin::jmap::api::detail::RawAccount>
{
    using T = javelin::jmap::api::detail::RawAccount;

    static constexpr auto value =
        glz::object("name", &T::name, "isPersonal", &T::isPersonal, "isReadOnly", &T::isReadOnly,
                    "accountCapabilities", &T::accountCapabilities);
};

template <> struct glz::meta<javelin::jmap::api::detail::RawSession>
{
    using T = javelin::jmap::api::detail::RawSession;

    static constexpr auto value = glz::object(
        "username", &T::username, "apiUrl", &T::apiUrl, "downloadUrl", &T::downloadUrl, "uploadUrl",
        &T::uploadUrl, "eventSourceUrl", &T::eventSourceUrl, "state", &T::state, "capabilities",
        &T::capabilities, "accounts", &T::accounts, "primaryAccounts", &T::primaryAccounts);
};

namespace javelin::jmap::api
{

    namespace
    {

        [[nodiscard]] bool
        capabilityPresent(const std::unordered_map<std::string, glz::generic>& capabilities,
                          const std::string_view uri)
        {
            return capabilities.contains(std::string{uri});
        }

        [[nodiscard]] std::optional<CoreCapability>
        parseCoreCapability(const std::unordered_map<std::string, glz::generic>& capabilities)
        {
            const auto it = capabilities.find(std::string{coreCapabilityUri});
            if (it == capabilities.end())
            {
                return std::nullopt;
            }

            std::string buffer;
            const auto writeError = glz::write_json(it->second, buffer);
            if (writeError)
            {
                return std::nullopt;
            }

            CoreCapability coreCapability;
            const auto readError =
                glz::read<glz::opts{.error_on_unknown_keys = false}>(coreCapability, buffer);
            if (readError)
            {
                return std::nullopt;
            }

            return coreCapability;
        }

        [[nodiscard]] std::optional<ContactsCapability>
        parseContactsCapability(const std::unordered_map<std::string, glz::generic>& capabilities)
        {
            const auto it = capabilities.find(std::string{contactsCapabilityUri});
            if (it == capabilities.end())
            {
                return std::nullopt;
            }
            std::string buffer;
            if (glz::write_json(it->second, buffer))
            {
                return std::nullopt;
            }
            ContactsCapability capability;
            if (glz::read<glz::opts{.error_on_unknown_keys = false}>(capability, buffer))
            {
                return std::nullopt;
            }
            return capability;
        }

        [[nodiscard]] std::optional<CalendarsCapability>
        parseCalendarsCapability(const std::unordered_map<std::string, glz::generic>& capabilities)
        {
            const auto it = capabilities.find(std::string{calendarsCapabilityUri});
            if (it == capabilities.end())
            {
                return std::nullopt;
            }
            std::string buffer;
            if (glz::write_json(it->second, buffer))
            {
                return std::nullopt;
            }
            CalendarsCapability capability;
            if (glz::read<glz::opts{.error_on_unknown_keys = false}>(capability, buffer) ||
                capability.minDateTime.empty() || capability.maxDateTime.empty() ||
                capability.maxExpandedQueryDuration.empty())
            {
                return std::nullopt;
            }
            return capability;
        }

        [[nodiscard]] std::optional<WebSocketCapability>
        parseWebSocketCapability(const std::unordered_map<std::string, glz::generic>& capabilities)
        {
            const auto it = capabilities.find(std::string{websocketCapabilityUri});
            if (it == capabilities.end())
            {
                return std::nullopt;
            }
            std::string buffer;
            if (glz::write_json(it->second, buffer))
            {
                return std::nullopt;
            }
            WebSocketCapability capability;
            if (glz::read<glz::opts{.error_on_unknown_keys = false}>(capability, buffer) ||
                capability.url.empty())
            {
                return std::nullopt;
            }
            return capability;
        }

        [[nodiscard]] PrimaryAccounts
        parsePrimaryAccounts(const std::unordered_map<std::string, std::string>& primaryAccounts)
        {
            PrimaryAccounts result;

            if (const auto mailIt = primaryAccounts.find(std::string{mailCapabilityUri});
                mailIt != primaryAccounts.end())
            {
                result.mailAccountId = mailIt->second;
            }

            if (const auto submissionIt =
                    primaryAccounts.find(std::string{submissionCapabilityUri});
                submissionIt != primaryAccounts.end())
            {
                result.submissionAccountId = submissionIt->second;
            }

            if (const auto contactsIt = primaryAccounts.find(std::string{contactsCapabilityUri});
                contactsIt != primaryAccounts.end())
            {
                result.contactsAccountId = contactsIt->second;
            }

            if (const auto calendarsIt = primaryAccounts.find(std::string{calendarsCapabilityUri});
                calendarsIt != primaryAccounts.end())
            {
                result.calendarsAccountId = calendarsIt->second;
            }

            return result;
        }

        [[nodiscard]] std::unordered_map<std::string, Account>
        parseAccounts(std::unordered_map<std::string, detail::RawAccount> rawAccounts)
        {
            std::unordered_map<std::string, Account> accounts;
            accounts.reserve(rawAccounts.size());

            for (auto& [accountId, rawAccount] : rawAccounts)
            {
                accounts.emplace(
                    accountId,
                    Account{
                        .id = accountId,
                        .name = std::move(rawAccount.name),
                        .isPersonal = rawAccount.isPersonal,
                        .isReadOnly = rawAccount.isReadOnly,
                        .accountCapabilities =
                            {
                                .mail = capabilityPresent(rawAccount.accountCapabilities,
                                                          mailCapabilityUri),
                                .submission = capabilityPresent(rawAccount.accountCapabilities,
                                                                submissionCapabilityUri),
                                .contacts = parseContactsCapability(rawAccount.accountCapabilities),
                                .calendars =
                                    parseCalendarsCapability(rawAccount.accountCapabilities),
                            },
                    });
            }

            return accounts;
        }

        [[nodiscard]] Session buildSession(detail::RawSession rawSession)
        {
            return Session{
                .username = std::move(rawSession.username),
                .apiUrl = std::move(rawSession.apiUrl),
                .downloadUrl = std::move(rawSession.downloadUrl),
                .uploadUrl = std::move(rawSession.uploadUrl),
                .eventSourceUrl = std::move(rawSession.eventSourceUrl),
                .state = std::move(rawSession.state),
                .capabilities =
                    {
                        .core = capabilityPresent(rawSession.capabilities, coreCapabilityUri),
                        .coreDetails = parseCoreCapability(rawSession.capabilities),
                        .mail = capabilityPresent(rawSession.capabilities, mailCapabilityUri),
                        .submission =
                            capabilityPresent(rawSession.capabilities, submissionCapabilityUri),
                        .contacts =
                            capabilityPresent(rawSession.capabilities, contactsCapabilityUri),
                        .calendars =
                            capabilityPresent(rawSession.capabilities, calendarsCapabilityUri),
                        .websocket = parseWebSocketCapability(rawSession.capabilities),
                    },
                .accounts = parseAccounts(std::move(rawSession.accounts)),
                .primaryAccounts = parsePrimaryAccounts(rawSession.primaryAccounts),
            };
        }

    } // namespace

    bool SessionParseResult::ok() const
    {
        return session.has_value();
    }

    SessionParseResult parseSession(std::string_view json, const RequiredCapabilities& required)
    {
        std::string buffer{json};
        detail::RawSession rawSession;
        const auto readError =
            glz::read<glz::opts{.error_on_unknown_keys = false}>(rawSession, buffer);
        if (readError)
        {
            return SessionParseResult{
                .session = std::nullopt,
                .error =
                    SessionParseError{
                        .code = SessionParseErrorCode::JsonParseFailed,
                        .message = glz::format_error(readError, buffer),
                        .capabilityErrors = {},
                    },
            };
        }

        Session session = buildSession(std::move(rawSession));
        const CapabilityValidationResult validation =
            validateSessionCapabilities(session, required);
        if (!validation.ok())
        {
            return SessionParseResult{
                .session = std::nullopt,
                .error =
                    SessionParseError{
                        .code = SessionParseErrorCode::CapabilityValidationFailed,
                        .message = "Session is missing required capabilities",
                        .capabilityErrors = std::move(validation.errors),
                    },
            };
        }

        return SessionParseResult{
            .session = std::move(session),
            .error = std::nullopt,
        };
    }

    std::string_view toString(const SessionParseErrorCode code)
    {
        switch (code)
        {
        case SessionParseErrorCode::JsonParseFailed:
            return "json_parse_failed";
        case SessionParseErrorCode::CapabilityValidationFailed:
            return "capability_validation_failed";
        }

        return "unknown_session_parse_error";
    }

} // namespace javelin::jmap::api
