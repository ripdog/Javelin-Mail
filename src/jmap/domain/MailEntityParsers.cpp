#include "jmap/domain/MailEntityParsers.h"

#include <glaze/glaze.hpp>

#include <string>
#include <unordered_map>
#include <utility>

namespace
{

    template <typename T> struct RawParsedObject
    {
        std::optional<T> value;
        std::optional<std::string> error;
    };

    struct RawEmail
    {
        std::string id;
        std::string blobId;
        std::string threadId;
        std::unordered_map<std::string, bool> mailboxIds;
        std::unordered_map<std::string, bool> keywords;
        std::uint64_t size = 0;
        std::string receivedAt;
        std::optional<std::string> sentAt;
        bool hasAttachment = false;
        std::optional<std::string> subject;
        std::vector<javelin::jmap::domain::EmailAddress> from;
        std::vector<javelin::jmap::domain::EmailAddress> to;
        std::vector<javelin::jmap::domain::EmailAddress> cc;
        std::vector<javelin::jmap::domain::EmailAddress> bcc;
        std::vector<javelin::jmap::domain::EmailAddress> replyTo;
        std::optional<std::string> preview;
    };

    [[nodiscard]] std::vector<std::string>
    enabledKeys(const std::unordered_map<std::string, bool>& values)
    {
        std::vector<std::string> keys;
        keys.reserve(values.size());

        for (const auto& [key, enabled] : values)
        {
            if (enabled)
            {
                keys.push_back(key);
            }
        }

        return keys;
    }

    template <typename RawType, typename DomainType, typename Convert>
    [[nodiscard]] javelin::jmap::domain::ParsedObject<DomainType>
    parseWithGlaze(std::string_view json, Convert&& convert)
    {
        std::string buffer{json};
        RawType raw{};
        const auto readError = glz::read<glz::opts{.error_on_unknown_keys = false}>(raw, buffer);
        if (readError)
        {
            return {
                .value = std::nullopt,
                .error = glz::format_error(readError, buffer),
            };
        }

        return {
            .value = std::forward<Convert>(convert)(std::move(raw)),
            .error = std::nullopt,
        };
    }

} // namespace

template <> struct glz::meta<javelin::jmap::domain::EmailAddress>
{
    using T = javelin::jmap::domain::EmailAddress;

    static constexpr auto value = glz::object("name", &T::name, "email", &T::email);
};

template <> struct glz::meta<javelin::jmap::domain::MailboxRights>
{
    using T = javelin::jmap::domain::MailboxRights;

    static constexpr auto value = glz::object(
        "mayReadItems", &T::mayReadItems, "mayAddItems", &T::mayAddItems, "mayRemoveItems",
        &T::mayRemoveItems, "maySetSeen", &T::maySetSeen, "maySetKeywords", &T::maySetKeywords,
        "mayCreateChild", &T::mayCreateChild, "mayRename", &T::mayRename, "mayDelete",
        &T::mayDelete, "maySubmit", &T::maySubmit);
};

template <> struct glz::meta<javelin::jmap::domain::Mailbox>
{
    using T = javelin::jmap::domain::Mailbox;

    static constexpr auto value =
        glz::object("id", &T::id, "name", &T::name, "parentId", &T::parentId, "role", &T::role,
                    "sortOrder", &T::sortOrder, "totalEmails", &T::totalEmails, "unreadEmails",
                    &T::unreadEmails, "totalThreads", &T::totalThreads, "unreadThreads",
                    &T::unreadThreads, "isSubscribed", &T::isSubscribed, "myRights", &T::myRights);
};

template <> struct glz::meta<javelin::jmap::domain::Thread>
{
    using T = javelin::jmap::domain::Thread;

    static constexpr auto value = glz::object("id", &T::id, "emailIds", &T::emailIds);
};

template <> struct glz::meta<RawEmail>
{
    using T = RawEmail;

    static constexpr auto value = glz::object(
        "id", &T::id, "blobId", &T::blobId, "threadId", &T::threadId, "mailboxIds", &T::mailboxIds,
        "keywords", &T::keywords, "size", &T::size, "receivedAt", &T::receivedAt, "sentAt",
        &T::sentAt, "hasAttachment", &T::hasAttachment, "subject", &T::subject, "from", &T::from,
        "to", &T::to, "cc", &T::cc, "bcc", &T::bcc, "replyTo", &T::replyTo, "preview", &T::preview);
};

template <> struct glz::meta<javelin::jmap::domain::Identity>
{
    using T = javelin::jmap::domain::Identity;

    static constexpr auto value =
        glz::object("id", &T::id, "name", &T::name, "email", &T::email, "replyTo", &T::replyTo,
                    "bcc", &T::bcc, "textSignature", &T::textSignature, "htmlSignature",
                    &T::htmlSignature, "mayDelete", &T::mayDelete);
};

namespace javelin::jmap::domain
{

    ParsedObject<Mailbox> parseMailbox(std::string_view json)
    {
        return parseWithGlaze<Mailbox, Mailbox>(json, [](Mailbox mailbox) { return mailbox; });
    }

    ParsedObject<Thread> parseThread(std::string_view json)
    {
        return parseWithGlaze<Thread, Thread>(json, [](Thread thread) { return thread; });
    }

    ParsedObject<Email> parseEmail(std::string_view json)
    {
        return parseWithGlaze<RawEmail, Email>(json,
                                               [](RawEmail rawEmail)
                                               {
                                                   return Email{
                                                       .id = std::move(rawEmail.id),
                                                       .blobId = std::move(rawEmail.blobId),
                                                       .threadId = std::move(rawEmail.threadId),
                                                       .mailboxIds =
                                                           enabledKeys(rawEmail.mailboxIds),
                                                       .keywords = enabledKeys(rawEmail.keywords),
                                                       .size = rawEmail.size,
                                                       .receivedAt = std::move(rawEmail.receivedAt),
                                                       .sentAt = std::move(rawEmail.sentAt),
                                                       .hasAttachment = rawEmail.hasAttachment,
                                                       .subject = std::move(rawEmail.subject),
                                                       .from = std::move(rawEmail.from),
                                                       .to = std::move(rawEmail.to),
                                                       .cc = std::move(rawEmail.cc),
                                                       .bcc = std::move(rawEmail.bcc),
                                                       .replyTo = std::move(rawEmail.replyTo),
                                                       .preview = std::move(rawEmail.preview),
                                                   };
                                               });
    }

    ParsedObject<Identity> parseIdentity(std::string_view json)
    {
        return parseWithGlaze<Identity, Identity>(json, [](Identity identity) { return identity; });
    }

} // namespace javelin::jmap::domain
