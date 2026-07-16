#pragma once

#include "jmap/domain/MailEntities.h"

#include <optional>
#include <string>
#include <string_view>

namespace javelin::jmap::domain
{

    template <typename T> struct ParsedObject
    {
        std::optional<T> value;
        std::optional<std::string> error;

        [[nodiscard]] bool ok() const
        {
            return value.has_value();
        }
    };

    [[nodiscard]] ParsedObject<Mailbox> parseMailbox(std::string_view json);
    [[nodiscard]] ParsedObject<Thread> parseThread(std::string_view json);
    [[nodiscard]] ParsedObject<Email> parseEmail(std::string_view json);
    [[nodiscard]] std::optional<std::string> serializeEmail(const Email& email);
    [[nodiscard]] ParsedObject<Identity> parseIdentity(std::string_view json);

} // namespace javelin::jmap::domain
