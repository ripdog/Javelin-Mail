#include "app/MessageSelection.h"

#include "app/ThreadMaterializationCoordinator.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <KLocalizedString>

#include <algorithm>
#include <ranges>
#include <unordered_set>

namespace javelin::app
{
    QCoro::Task<std::optional<javelin::jmap::OperationError>> ensureMessageSelectionMaterialized(
        javelin::jmap::cache::DatabaseConnection& databaseConnection,
        ThreadMaterializationCoordinator* threadMaterializationCoordinator, std::string accountId,
        std::optional<std::string> sourceMailboxId, MessageSelection selection,
        const WorkPriority priority)
    {
        // Mailbox-scoped Thread actions resolve directly from the cached Email/mailbox projection.
        // They must not add a synchronous Thread/get before the operation.
        if (sourceMailboxId.has_value())
            co_return std::nullopt;

        std::vector<std::string> threadIds;
        for (const auto& item : selection)
        {
            if (const auto* thread = std::get_if<SelectedCollapsedThread>(&item);
                thread != nullptr && !thread->threadId.empty())
                threadIds.push_back(thread->threadId);
        }
        std::ranges::sort(threadIds);
        threadIds.erase(std::unique(threadIds.begin(), threadIds.end()), threadIds.end());
        if (threadIds.empty())
            co_return std::nullopt;

        javelin::jmap::cache::ThreadRepository threads{databaseConnection};
        bool incomplete = false;
        for (const auto& threadId : threadIds)
        {
            const auto coverageResult = threads.coverage(accountId, threadId);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&coverageResult))
                co_return javelin::jmap::operationError(*error);
            const auto& coverage =
                std::get<std::optional<javelin::jmap::cache::ThreadCoverage>>(coverageResult);
            incomplete = incomplete || !coverage.has_value() || !coverage->childEmailsComplete;
        }
        if (!incomplete)
            co_return std::nullopt;
        if (threadMaterializationCoordinator == nullptr)
        {
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::NetworkUnavailable,
                .message = i18n("The selected conversation is not fully available. Connect to "
                                "the network and try again."),
            };
        }

        auto result = co_await threadMaterializationCoordinator->waitForThreads(
            std::move(accountId), std::move(threadIds), priority);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
            co_return *error;
        co_return std::nullopt;
    }

    ResolvedMessageSelection
    resolveMessageSelection(const javelin::jmap::cache::ThreadReader& threadReader,
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
                emailIds.emplace_back(emailId);
        };

        for (const auto& item : selection)
        {
            if (const auto* email = std::get_if<SelectedEmail>(&item))
            {
                appendEmailId(email->emailId);
                continue;
            }

            const auto& thread = std::get<SelectedCollapsedThread>(item);
            if (mailboxId.has_value())
            {
                const auto messagesResult = threadReader.listMailboxThreadMessages(
                    accountId, *mailboxId, thread.threadId,
                    javelin::jmap::cache::MailboxThreadMembershipSource::CachedMailbox);
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
                return error->message;
            const auto& coverage =
                std::get<std::optional<javelin::jmap::cache::ThreadCoverage>>(coverageResult);
            if (!coverage.has_value() || !coverage->childEmailsComplete)
            {
                return QStringLiteral("The selected conversation is not fully available in the "
                                      "local cache.");
            }

            const auto messagesResult = threadReader.listThreadMessages(accountId, thread.threadId);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&messagesResult))
                return error->message;
            const auto& messages =
                std::get<std::vector<javelin::jmap::cache::MessageListItem>>(messagesResult);
            for (const auto& message : messages)
                appendEmailId(message.emailId);
        }

        return emailIds;
    }

} // namespace javelin::app
