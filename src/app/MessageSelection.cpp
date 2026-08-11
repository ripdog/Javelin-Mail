#include "app/MessageSelection.h"

#include <unordered_set>

namespace javelin::app
{

    ResolvedMessageSelection
    resolveMessageSelection(const javelin::jmap::cache::QueryReader& queryReader,
                            const javelin::jmap::cache::ThreadRepository& threadRepository,
                            const std::string_view accountId,
                            const std::optional<std::string_view> mailboxId,
                            const MessageSelection& selection)
    {
        std::vector<std::string> emailIds;
        std::unordered_set<std::string> seen;
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
                    ? queryReader.listMailboxThreadMessages(accountId, *mailboxId, thread.threadId)
                    : queryReader.listThreadMessages(accountId, thread.threadId);
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
