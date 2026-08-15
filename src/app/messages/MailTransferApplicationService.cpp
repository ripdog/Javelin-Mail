#include "app/MailTransferApplicationService.h"

#include "app/MailTransferRepository.h"
#include "app/ThreadMaterializationCoordinator.h"
#include "jmap/cache/AccountRepository.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailboxReadRepository.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/cache/SyncStateRepository.h"
#include "jmap/cache/ThreadReadRepository.h"
#include "jmap/cache/ThreadRepository.h"

#include <KLocalizedString>

#include <QUuid>

#include <algorithm>
#include <ranges>
#include <utility>

namespace javelin::app
{
    namespace
    {
        [[nodiscard]] javelin::jmap::OperationError invalidState(QString message)
        {
            return {
                .code = javelin::jmap::OperationErrorCode::PreconditionFailed,
                .message = std::move(message),
            };
        }

        [[nodiscard]] std::string newId(const char* prefix)
        {
            return std::string{prefix} + '-' +
                   QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        }

        [[nodiscard]] std::variant<javelin::jmap::cache::CachedAccount,
                                   javelin::jmap::OperationError>
        requireAccount(javelin::jmap::cache::AccountRepository& accounts,
                       const std::string& localAccountId)
        {
            const auto result = accounts.findById(localAccountId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
                return javelin::jmap::operationError(*error);
            const auto& account =
                std::get<std::optional<javelin::jmap::cache::CachedAccount>>(result);
            if (!account.has_value())
            {
                return invalidState(i18n("The selected mail account is no longer available."));
            }
            return *account;
        }
    } // namespace

    MailTransferApplicationService::MailTransferApplicationService(
        javelin::jmap::cache::DatabaseConnection& databaseConnection,
        ThreadMaterializationCoordinator* threadMaterializationCoordinator)
        : m_databaseConnection(databaseConnection),
          m_threadMaterializationCoordinator(threadMaterializationCoordinator)
    {
    }

    QCoro::Task<std::optional<javelin::jmap::OperationError>>
    MailTransferApplicationService::ensureSelectionMaterialized(
        std::string accountId, std::optional<std::string> sourceMailboxId,
        MessageSelection selection)
    {
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

        javelin::jmap::cache::ThreadRepository threads{m_databaseConnection};
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
        if (m_threadMaterializationCoordinator == nullptr)
        {
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::NetworkUnavailable,
                .message = i18n("The selected conversation is not fully available. Connect to "
                                "the network and try again."),
            };
        }

        auto result = co_await m_threadMaterializationCoordinator->waitForThreads(
            std::move(accountId), std::move(threadIds), WorkPriority::Interactive);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
            co_return *error;
        co_return std::nullopt;
    }

    QCoro::Task<MailTransferPreparationResult>
    MailTransferApplicationService::prepare(MailTransferPreparationRequest request)
    {
        if (request.selection.empty())
            co_return invalidState(i18n("No messages were selected for transfer."));
        if (const auto error = co_await ensureSelectionMaterialized(
                request.intent.sourceAccountId, request.intent.sourceMailboxId, request.selection))
            co_return *error;

        javelin::jmap::cache::ThreadRepository threads{m_databaseConnection};
        javelin::jmap::cache::ThreadReadRepository threadReader{m_databaseConnection};
        auto resolved = resolveMessageSelection(
            threadReader, threads, request.intent.sourceAccountId,
            request.intent.sourceMailboxId.has_value()
                ? std::optional<std::string_view>{*request.intent.sourceMailboxId}
                : std::nullopt,
            request.selection);
        if (const auto* error = std::get_if<QString>(&resolved))
            co_return invalidState(*error);
        auto emailIds = std::get<std::vector<std::string>>(std::move(resolved));
        if (emailIds.empty())
            co_return invalidState(i18n("No messages were selected for transfer."));

        javelin::jmap::cache::AccountRepository accounts{m_databaseConnection};
        auto sourceAccountResult = requireAccount(accounts, request.intent.sourceAccountId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&sourceAccountResult))
            co_return *error;
        auto destinationAccountResult =
            requireAccount(accounts, request.intent.destinationAccountId);
        if (const auto* error =
                std::get_if<javelin::jmap::OperationError>(&destinationAccountResult))
            co_return *error;
        const auto& sourceAccount =
            std::get<javelin::jmap::cache::CachedAccount>(sourceAccountResult);
        const auto& destinationAccount =
            std::get<javelin::jmap::cache::CachedAccount>(destinationAccountResult);

        javelin::jmap::cache::MailboxReadRepository mailboxReader{m_databaseConnection};
        const auto sourceMailboxResult =
            mailboxReader.listMailboxTree(request.intent.sourceAccountId);
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&sourceMailboxResult))
            co_return javelin::jmap::operationError(*error);
        const auto destinationMailboxResult =
            mailboxReader.listMailboxTree(request.intent.destinationAccountId);
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&destinationMailboxResult))
            co_return javelin::jmap::operationError(*error);

        javelin::jmap::cache::EmailRepository emailRepository{m_databaseConnection};
        std::vector<javelin::jmap::domain::Email> emails;
        emails.reserve(emailIds.size());
        for (const auto& emailId : emailIds)
        {
            const auto emailResult = emailRepository.find(request.intent.sourceAccountId, emailId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&emailResult))
                co_return javelin::jmap::operationError(*error);
            const auto& email = std::get<std::optional<javelin::jmap::domain::Email>>(emailResult);
            if (!email.has_value())
            {
                co_return invalidState(i18n("Message %1 is not available in the local cache.",
                                            QString::fromStdString(emailId)));
            }
            emails.push_back(*email);
        }

        auto planResult = planMailTransfer(
            request.intent, emailIds, emails,
            std::get<std::vector<javelin::jmap::cache::MailboxTreeItem>>(sourceMailboxResult),
            std::get<std::vector<javelin::jmap::cache::MailboxTreeItem>>(destinationMailboxResult),
            sourceAccount, destinationAccount, request.sourceCleanupOverrides);
        if (const auto* error = std::get_if<QString>(&planResult))
            co_return invalidState(*error);
        auto plan = std::get<PlannedMailTransfer>(std::move(planResult));

        if (plan.topology == MailTransferTopology::SameSessionCopy)
        {
            javelin::jmap::cache::SessionRepository sessions{m_databaseConnection};
            const auto sessionResult = sessions.load(request.intent.sourceAccountId);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&sessionResult))
                co_return javelin::jmap::operationError(*error);
            const auto& session =
                std::get<std::optional<javelin::jmap::api::Session>>(sessionResult);
            if (!session.has_value() ||
                !session->accounts.contains(sourceAccount.remoteAccountId) ||
                !session->accounts.contains(destinationAccount.remoteAccountId))
            {
                co_return invalidState(
                    i18n("The two accounts are no longer available in the same JMAP session."));
            }
        }

        javelin::jmap::cache::SyncStateRepository syncStates{m_databaseConnection};
        const auto stateResult = syncStates.find(
            {.accountId = request.intent.sourceAccountId, .objectType = "Email", .queryKey = {}});
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&stateResult))
            co_return javelin::jmap::operationError(*error);
        const auto& emailState =
            std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(stateResult);
        if (!emailState.has_value() || emailState->stateToken.empty())
            co_return invalidState(i18n("Email state has not been synchronized for this account."));

        const auto operationId = newId("mail-transfer");
        std::vector<MailTransferItemRecord> itemRecords;
        itemRecords.reserve(plan.items.size());
        for (std::size_t index = 0; index < plan.items.size(); ++index)
        {
            const auto& planned = plan.items[index];
            itemRecords.push_back({
                .itemId = newId("mail-transfer-item"),
                .operationId = operationId,
                .ordinal = static_cast<std::int64_t>(index),
                .sourceEmailId = planned.sourceEmailId,
                .sourceBlobId = planned.sourceBlobId,
                .sourceEmailState = emailState->stateToken,
                .sourceMailboxIds = planned.sourceMailboxIds,
                .sourceKeywords = planned.sourceKeywords,
                .sourceMessageIds = planned.sourceMessageIds,
                .sourceReceivedAt = planned.sourceReceivedAt.empty()
                                        ? std::nullopt
                                        : std::optional<std::string>{planned.sourceReceivedAt},
                .sourceSize = planned.sourceSize,
                .sourceRemoveMailboxIds = planned.sourceRemoveMailboxIds,
                .sourceDestroy = planned.sourceDestroy,
                .rawContentHash = std::nullopt,
                .destinationCreationId = newId("mail-transfer-create"),
                .destinationUploadBlobId = std::nullopt,
                .destinationPreState = std::nullopt,
                .destinationEmailId = std::nullopt,
                .destinationBlobId = std::nullopt,
                .destinationThreadId = std::nullopt,
                .destinationSize = std::nullopt,
                .reusedExisting = false,
                .destinationPriorMailboxIds = std::nullopt,
                .phase = MailTransferItemPhase::Prepared,
                .lastError = std::nullopt,
                .createdAt = {},
                .updatedAt = {},
            });
        }

        const auto count = static_cast<int>(itemRecords.size());
        const QString title = request.intent.operation == MailTransferOperation::Move
                                  ? i18np("Move %1 message", "Move %1 messages", count)
                                  : i18np("Copy %1 message", "Copy %1 messages", count);
        MailTransferRepository repository{m_databaseConnection};
        if (const auto error = repository.create(
                {
                    .operationId = operationId,
                    .operationGroupId = operationId,
                    .sourceAccountId = request.intent.sourceAccountId,
                    .sourceMailboxId = request.intent.sourceMailboxId,
                    .destinationAccountId = request.intent.destinationAccountId,
                    .destinationMailboxId = request.intent.destinationMailboxId,
                    .operation = request.intent.operation,
                    .topology = plan.topology,
                    .status = MailTransferStatus::Preparing,
                    .title = title,
                    .lastError = std::nullopt,
                    .historyEntryId = std::nullopt,
                    .createdAt = {},
                    .updatedAt = {},
                },
                itemRecords))
            co_return javelin::jmap::operationError(*error);

        co_return PreparedMailTransfer{
            .operationId = operationId,
            .itemCount = itemRecords.size(),
            .topology = plan.topology,
        };
    }

} // namespace javelin::app
