#pragma once

#include <string>

namespace javelin::jmap::cache
{
    // Stable local identity used by cache tables and application routing. It is deliberately
    // independent of the server-provided JMAP account id, which is scoped to one connection.
    struct MailAccountKey
    {
        std::string value;

        friend bool operator==(const MailAccountKey&, const MailAccountKey&) = default;
    };

    struct MailAccountLocator
    {
        std::string connectionId;
        std::string remoteAccountId;

        friend bool operator==(const MailAccountLocator&, const MailAccountLocator&) = default;
    };

} // namespace javelin::jmap::cache
