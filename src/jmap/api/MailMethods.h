#pragma once

#include "jmap/api/MethodEnvelope.h"
#include "jmap/domain/MailEntities.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace javelin::jmap::api
{

    struct GetRequest
    {
        std::string accountId;
        std::optional<std::vector<std::string>> ids;
        std::optional<std::vector<std::string>> properties;
    };

    struct MailboxGetResponse
    {
        std::string accountId;
        std::string state;
        std::vector<javelin::jmap::domain::Mailbox> list;
        std::vector<std::string> notFound;
    };

    struct EmailGetResponse
    {
        std::string accountId;
        std::string state;
        std::vector<javelin::jmap::domain::Email> list;
        std::vector<std::string> notFound;
    };

    struct ChangesRequest
    {
        std::string accountId;
        std::string sinceState;
        std::optional<std::uint64_t> maxChanges;
    };

    struct ChangesResponse
    {
        std::string accountId;
        std::string oldState;
        std::string newState;
        bool hasMoreChanges = false;
        std::vector<std::string> created;
        std::vector<std::string> updated;
        std::vector<std::string> destroyed;
    };

    [[nodiscard]] std::optional<std::string> serializeGetRequest(const GetRequest& request);
    [[nodiscard]] std::optional<std::string> serializeChangesRequest(const ChangesRequest& request);

    [[nodiscard]] ParsedEnvelope<MailboxGetResponse> parseMailboxGetResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<EmailGetResponse> parseEmailGetResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<ChangesResponse> parseChangesResponse(std::string_view json);

} // namespace javelin::jmap::api
