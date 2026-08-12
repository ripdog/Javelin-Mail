#include "app/MessageSelection.h"

#include <unordered_set>

namespace javelin::app
{

    ResolvedMessageSelection
    resolveMessageSelection(const javelin::jmap::cache::MailboxMessageReader& mailboxMessageReader,
                            const javelin::jmap::cache::ThreadReader& threadReader,
                            const javelin::jmap::cache::ThreadRepository& threadRepository,
                            const std::string_view accountId,
                            const std::optional<std::string_view> mailboxId,
                            const MessageSelection& selection)
    {
        std::vector<std::string> emailIds;
        std::unordered_set<std::string> seen;
        std::optional<bool> completeOfflineMailbox;
        const auto appendEmailId = [&emailIds, &seen](const std::string_view emailId)
        {
            if (!emailId.empty() && seen.emplace(emailId).second)
            {
                emailIds.emplace_back(emailId);
            }
        };

        for (const auto& item : selection)
        {
            if (const auto* email = std::get_if<SelectedEmail>(&item))
            {
                appendEmailId(email->emailId);
                continue;
            }

            const auto& thread = std::get<SelectedCollapsedThread>(item);
            if (mailboxId.has_value() && !completeOfflineMailbox.has_value())
            {
                const auto stateResult =
                    mailboxMessageReader.offlineMailboxComplete(accountId, *mailboxId);
                if (const auto* error =
                        std::get_if<javelin::jmap::cache::DatabaseError>(&stateResult))
                    return error->message;
                completeOfflineMailbox = std::get<bool>(stateResult);
            }
            if (completeOfflineMailbox.value_or(false))
            {
                const auto messagesResult = threadReader.listMailboxThreadMessages(
                    accountId, *mailboxId, thread.threadId,
                    javelin::jmap::cache::MailboxThreadMembershipSource::CompleteOfflineMailbox);
                if (const auto* error =
                        std::get_if<javelin::jmap::cache::DatabaseError>(&messagesResult))
                    return error->message;
                const auto& messages =
                    std::get<std::vector<javelin::jmap::cache::MessageListItem>>(messagesResult);
                for (const auto& member : messages)
                    appendEmailId(member.emailId);
                continue;
            }
            const auto coverageResult = threadRepository.coverage(accountId, thread.threadId);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&coverageResult))
            {
                return error->message;
            }
            const auto& coverage =
                std::get<std::optional<javelin::jmap::cache::ThreadCoverage>>(coverageResult);
            if (!coverage.has_value() || !coverage->childEmailsComplete)
            {
                return QStringLiteral("The selected conversation is not fully available in the "
                                      "local cache.");
            }
            const auto messagesResult =
                mailboxId.has_value()
                    ? threadReader.listMailboxThreadMessages(accountId, *mailboxId, thread.threadId)
                    : threadReader.listThreadMessages(accountId, thread.threadId);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&messagesResult))
            {
                return error->message;
            }

            const auto& messages =
                std::get<std::vector<javelin::jmap::cache::MessageListItem>>(messagesResult);
            for (const auto& message : messages)
            {
                appendEmailId(message.emailId);
            }
        }

        return emailIds;
    }

} // namespace javelin::app
