#include "jmap/sync/RefreshNotificationPlanner.h"

#include "jmap/cache/EmailRepository.h"

#include <algorithm>

namespace javelin::jmap::sync
{

    RefreshNotificationPlanner::RefreshNotificationPlanner(
        javelin::jmap::cache::DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    std::variant<std::vector<RefreshNotificationCandidate>, javelin::jmap::cache::DatabaseError>
    RefreshNotificationPlanner::plan(const std::string_view accountId,
                                     const std::string_view mailboxId,
                                     const MailboxRefreshSummary& refreshSummary) const
    {
        if (refreshSummary.insertedEmailIds.empty())
        {
            return std::vector<RefreshNotificationCandidate>{};
        }

        javelin::jmap::cache::EmailRepository emailRepository{m_connection};
        std::vector<RefreshNotificationCandidate> candidates;
        candidates.reserve(refreshSummary.insertedEmailIds.size());

        for (const auto& insertedEmailId : refreshSummary.insertedEmailIds)
        {
            const auto emailResult = emailRepository.find(accountId, insertedEmailId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&emailResult))
            {
                return *error;
            }

            const auto& email = std::get<std::optional<javelin::jmap::domain::Email>>(emailResult);
            if (!email.has_value())
            {
                continue;
            }

            const bool belongsToMailbox =
                std::ranges::find(email->mailboxIds, std::string{mailboxId}) !=
                email->mailboxIds.end();
            const bool isUnread =
                std::ranges::find(email->keywords, std::string{"$seen"}) == email->keywords.end();
            if (!belongsToMailbox || !isUnread)
            {
                continue;
            }

            candidates.push_back(RefreshNotificationCandidate{
                .emailId = email->id,
                .threadId = email->threadId,
                .subject = email->subject,
                .receivedAt = email->receivedAt,
            });
        }

        return candidates;
    }

} // namespace javelin::jmap::sync
