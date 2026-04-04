#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace javelin::jmap::domain
{

    struct EmailAddress
    {
        std::optional<std::string> name;
        std::string email;
    };

    struct MailboxRights
    {
        bool mayReadItems = false;
        bool mayAddItems = false;
        bool mayRemoveItems = false;
        bool maySetSeen = false;
        bool maySetKeywords = false;
        bool mayCreateChild = false;
        bool mayRename = false;
        bool mayDelete = false;
        bool maySubmit = false;
    };

    struct Mailbox
    {
        std::string id;
        std::string name;
        std::optional<std::string> parentId;
        std::optional<std::string> role;
        std::uint64_t sortOrder = 0;
        std::uint64_t totalEmails = 0;
        std::uint64_t unreadEmails = 0;
        std::uint64_t totalThreads = 0;
        std::uint64_t unreadThreads = 0;
        bool isSubscribed = false;
        MailboxRights myRights;
    };

    struct Thread
    {
        std::string id;
        std::vector<std::string> emailIds;
    };

    struct Email
    {
        std::string id;
        std::string blobId;
        std::string threadId;
        std::vector<std::string> mailboxIds;
        std::vector<std::string> keywords;
        std::uint64_t size = 0;
        std::string receivedAt;
        std::optional<std::string> sentAt;
        bool hasAttachment = false;
        std::optional<std::string> subject;
        std::vector<EmailAddress> from;
        std::vector<EmailAddress> to;
        std::vector<EmailAddress> cc;
        std::vector<EmailAddress> bcc;
        std::vector<EmailAddress> replyTo;
        std::optional<std::string> preview;
    };

    struct Identity
    {
        std::string id;
        std::string name;
        std::string email;
        std::vector<EmailAddress> replyTo;
        std::vector<EmailAddress> bcc;
        std::optional<std::string> textSignature;
        std::optional<std::string> htmlSignature;
        bool mayDelete = false;
    };

} // namespace javelin::jmap::domain
