#include "jmap/sync/MailboxRefreshExecutor.h"

#include "jmap/api/MailMethods.h"
#include "jmap/api/ResponseReader.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/QueryService.h"
#include "jmap/cache/SyncStateRepository.h"
#include "jmap/cache/ThreadRepository.h"
#include "jmap/sync/ConsistencyDomain.h"
#include "jmap/sync/EmailMutationJournal.h"
#include "jmap/sync/MailboxQueryDescriptor.h"
#include "jmap/sync/RefreshNotificationPlanner.h"
#include "jmap/sync/SyncPlanner.h"

#include <QDebug>
#include <QLoggingCategory>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <unordered_set>

namespace javelin::jmap::sync
{
    Q_LOGGING_CATEGORY(logMailboxSync, "jmap.sync.mailbox")

    namespace
    {

        [[nodiscard]] MailboxQueryDescriptor
        collapsedMailboxQueryDescriptor(const std::string_view mailboxId)
        {
            return MailboxQueryDescriptor{
                .mailboxId = std::string{mailboxId},
                .sortProperty = "receivedAt",
                .isAscending = false,
                .collapseThreads = true,
                .limit = 100,
                .offset = 0,
            };
        }

        [[nodiscard]] javelin::jmap::cache::SyncStateKey
        mailboxQuerySyncKey(const std::string_view accountId, const std::string_view mailboxId)
        {
            return javelin::jmap::cache::SyncStateKey{
                .accountId = std::string{accountId},
                .objectType = "EmailQuery",
                .queryKey = mailboxQueryKey(collapsedMailboxQueryDescriptor(mailboxId)),
            };
        }

        [[nodiscard]] javelin::jmap::cache::SyncStateKey
        emailSyncKey(const std::string_view accountId)
        {
            return javelin::jmap::cache::SyncStateKey{
                .accountId = std::string{accountId},
                .objectType = "Email",
                .queryKey = {},
            };
        }

        [[nodiscard]] bool
        isRecoverableIncrementalError(const javelin::jmap::api::ResponseReaderError& error)
        {
            if (error.code != javelin::jmap::api::ResponseReaderErrorCode::MethodError ||
                !error.methodError.has_value())
            {
                return false;
            }

            return error.methodError->type == "cannotCalculateChanges" ||
                   error.methodError->type == "tooManyChanges";
        }

        [[nodiscard]] std::vector<std::string> deduplicatedIds(std::vector<std::string> ids)
        {
            std::vector<std::string> deduplicated;
            deduplicated.reserve(ids.size());
            std::unordered_set<std::string> seen;
            seen.reserve(ids.size());

            for (auto& id : ids)
            {
                if (seen.insert(id).second)
                {
                    deduplicated.push_back(std::move(id));
                }
            }

            return deduplicated;
        }

        [[nodiscard]] QString joinIds(const std::vector<std::string>& ids)
        {
            QStringList parts;
            for (const auto& id : ids)
            {
                parts.push_back(QString::fromStdString(id));
            }
            return parts.join(QStringLiteral(","));
        }

        [[nodiscard]] QString
        emailMailboxSummary(const std::vector<javelin::jmap::domain::Email>& emails)
        {
            QStringList parts;
            for (const auto& email : emails)
            {
                QStringList mailboxIds;
                for (const auto& mailboxId : email.mailboxIds)
                {
                    mailboxIds.push_back(QString::fromStdString(mailboxId));
                }
                parts.push_back(QStringLiteral("%1[%2]").arg(QString::fromStdString(email.id),
                                                             mailboxIds.join(QStringLiteral(","))));
            }
            return parts.join(QStringLiteral(";"));
        }

        [[nodiscard]] std::vector<std::string>
        fetchedMailboxEmailIds(const std::vector<javelin::jmap::domain::Email>& emails,
                               const std::string_view mailboxId)
        {
            std::vector<std::string> emailIds;
            emailIds.reserve(emails.size());
            for (const auto& email : emails)
            {
                if (std::ranges::find(email.mailboxIds, std::string{mailboxId}) !=
                    email.mailboxIds.end())
                {
                    emailIds.push_back(email.id);
                }
            }

            return deduplicatedIds(std::move(emailIds));
        }

        [[nodiscard]] std::variant<std::vector<std::string>, OperationError>
        mailboxEmailIds(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                        const std::string_view accountId, const std::string_view mailboxId)
        {
            javelin::jmap::cache::EmailRepository emailRepository{databaseConnection};
            const auto result = emailRepository.listMailboxEmailIds(accountId, mailboxId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
            {
                return javelin::jmap::operationError(*error);
            }

            return std::get<std::vector<std::string>>(result);
        }

        [[nodiscard]] std::variant<std::size_t, OperationError>
        currentRepresentativeCount(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                                   const std::string_view accountId,
                                   const std::string_view mailboxId)
        {
            javelin::jmap::cache::QueryService queryService{databaseConnection};
            const auto result = queryService.listMailboxMessages(accountId, mailboxId, 100);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
            {
                return javelin::jmap::operationError(*error);
            }

            return std::get<std::vector<javelin::jmap::cache::MessageListItem>>(result).size();
        }

        [[nodiscard]] std::vector<javelin::jmap::sync::EmailMutationRecord>
        activeEmailMutations(const std::vector<javelin::jmap::sync::EmailMutationRecord>& actions)
        {
            std::vector<javelin::jmap::sync::EmailMutationRecord> filtered;
            filtered.reserve(actions.size());
            for (const auto& action : actions)
            {
                if (javelin::jmap::sync::projectsOptimistically(action.status))
                {
                    filtered.push_back(action);
                }
            }

            return filtered;
        }

        [[nodiscard]] std::optional<OperationError>
        reapplyPendingEmailPatches(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                                   const std::string_view accountId,
                                   std::vector<std::string> emailIds)
        {
            const auto ids = deduplicatedIds(std::move(emailIds));
            if (ids.empty())
            {
                return std::nullopt;
            }

            javelin::jmap::cache::EmailRepository emailRepository{databaseConnection};
            javelin::jmap::sync::EmailMutationJournal emailMutationJournal{databaseConnection};
            std::vector<javelin::jmap::domain::Email> reconciledEmails;
            reconciledEmails.reserve(ids.size());

            for (const auto& emailId : ids)
            {
                const auto emailResult = emailRepository.find(accountId, emailId);
                if (const auto* error =
                        std::get_if<javelin::jmap::cache::DatabaseError>(&emailResult))
                {
                    return javelin::jmap::operationError(*error);
                }

                const auto& email =
                    std::get<std::optional<javelin::jmap::domain::Email>>(emailResult);
                if (!email.has_value())
                {
                    continue;
                }

                const auto pendingResult = emailMutationJournal.listForEmail(accountId, emailId);
                if (const auto* error =
                        std::get_if<javelin::jmap::cache::DatabaseError>(&pendingResult))
                {
                    return javelin::jmap::operationError(*error);
                }

                const auto pendingActions = activeEmailMutations(
                    std::get<std::vector<javelin::jmap::sync::EmailMutationRecord>>(pendingResult));
                if (pendingActions.empty())
                {
                    continue;
                }

                reconciledEmails.push_back(
                    javelin::jmap::sync::projectEmailMutations(*email, pendingActions));
            }

            if (!reconciledEmails.empty())
            {
                if (const auto error = emailRepository.upsertMany(accountId, reconciledEmails))
                {
                    return javelin::jmap::operationError(*error);
                }
            }

            return std::nullopt;
        }

        struct CollapsedMailboxFetch
        {
            std::string queryState;
            std::string emailState;
            std::vector<javelin::jmap::domain::Thread> threads;
            std::vector<javelin::jmap::domain::Email> emails;
            std::size_t representativeCount = 0;
            std::optional<std::size_t> total;
        };

        struct IncrementalCollapsedMailboxRefresh
        {
            bool requiresFullFetch = false;
            std::string queryState;
            std::string emailState;
            std::vector<javelin::jmap::domain::Email> updatedEmails;
            std::size_t representativeCount = 0;
            std::vector<std::string> changedEmailIds;
            std::vector<std::string> insertedEmailIds;
            std::vector<std::string> removedEmailIds;
            bool requiresNotificationScan = false;
        };

        [[nodiscard]] QCoro::Task<std::variant<CollapsedMailboxFetch, OperationError>>
        fetchCollapsedMailboxThreads(javelin::jmap::api::MethodCaller& methodCaller,
                                     javelin::jmap::api::ApiRequestContext apiRequestContext,
                                     std::string accountId, std::string mailboxId,
                                     std::function<void(const QString&)> reportProgress)
        {
            const auto emitProgress = [&reportProgress](const QString& message)
            {
                if (reportProgress)
                {
                    reportProgress(message);
                }
            };

            javelin::jmap::api::RequestBuilder builder;
            builder.useCore().useMail();
            const auto queryRequest = javelin::jmap::api::emailQuery({
                .accountId = std::string{accountId},
                .filter =
                    javelin::jmap::api::EmailQueryFilter{
                        .inMailbox = std::string{mailboxId},
                        .text = std::nullopt,
                    },
                .sort =
                    {
                        javelin::jmap::api::EmailQuerySort{
                            .property = "receivedAt",
                            .isAscending = false,
                        },
                    },
                .position = 0,
                .limit = 100,
                .collapseThreads = true,
                .calculateTotal = true,
            });
            if (!queryRequest.has_value())
            {
                co_return OperationError{
                    .message = QStringLiteral("Failed to encode the mailbox Email/query request."),
                };
            }
            const auto queryHandle = builder.call(*queryRequest, "mailbox-query");

            const auto representativeRequest = javelin::jmap::api::emailGet(
                javelin::jmap::api::getRequestFrom(std::string{accountId}, queryHandle, "/ids",
                                                   std::vector<std::string>{"threadId"}));
            if (!representativeRequest.has_value())
            {
                co_return OperationError{
                    .message =
                        QStringLiteral("Failed to encode the representative Email/get request."),
                };
            }
            const auto representativeHandle =
                builder.call(*representativeRequest, "thread-ids-get");

            const auto threadRequest =
                javelin::jmap::api::threadGet(javelin::jmap::api::getRequestFrom(
                    std::string{accountId}, representativeHandle, "/list/*/threadId"));
            if (!threadRequest.has_value())
            {
                co_return OperationError{
                    .message = QStringLiteral("Failed to encode the Thread/get request."),
                };
            }
            const auto threadHandle = builder.call(*threadRequest, "threads-get");

            const auto emailRequest =
                javelin::jmap::api::emailGet(javelin::jmap::api::getRequestFrom(
                    std::string{accountId}, threadHandle, "/list/*/emailIds"));
            if (!emailRequest.has_value())
            {
                co_return OperationError{
                    .message = QStringLiteral("Failed to encode the mailbox Email/get request."),
                };
            }
            const auto emailHandle = builder.call(*emailRequest, "mailbox-emails-get");

            const auto envelopeResult = co_await methodCaller.call(apiRequestContext, builder);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::TransportError>(&envelopeResult))
            {
                co_return operationError(*error);
            }
            if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&envelopeResult))
            {
                co_return operationError(*error);
            }
            if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&envelopeResult))
            {
                co_return operationError(*error);
            }

            const auto& envelope = std::get<javelin::jmap::api::ResponseEnvelope>(envelopeResult);
            const javelin::jmap::api::ResponseReader reader{envelope};

            const auto queryResult = reader.require(queryHandle);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::ResponseReaderError>(&queryResult))
            {
                co_return operationError(*error);
            }
            const auto& parsedQuery = std::get<javelin::jmap::api::EmailQueryResponse>(queryResult);
            qCDebug(logMailboxSync).noquote()
                << "query result" << QString::fromStdString(accountId)
                << QString::fromStdString(mailboxId) << "state"
                << QString::fromStdString(parsedQuery.queryState) << "ids"
                << joinIds(parsedQuery.ids) << "total"
                << (parsedQuery.total.has_value() ? QString::number(*parsedQuery.total)
                                                  : QStringLiteral("unknown"));

            emitProgress(QStringLiteral("Fetched %1 conversation ids for the selected mailbox.")
                             .arg(parsedQuery.ids.size()));

            const auto representativeResult = reader.require(representativeHandle);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::ResponseReaderError>(&representativeResult))
            {
                co_return operationError(*error);
            }
            const auto& parsedRepresentatives =
                std::get<javelin::jmap::api::EmailGetResponse>(representativeResult);

            if (parsedQuery.ids.empty())
            {
                co_return CollapsedMailboxFetch{
                    .queryState = parsedQuery.queryState,
                    .emailState = {},
                    .threads = {},
                    .emails = {},
                    .representativeCount = 0,
                    .total = parsedQuery.total,
                };
            }

            emitProgress(
                QStringLiteral("Fetched %1 representative emails for the selected mailbox.")
                    .arg(parsedRepresentatives.list.size()));

            const auto threadResult = reader.require(threadHandle);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::ResponseReaderError>(&threadResult))
            {
                co_return operationError(*error);
            }
            const auto& parsedThreads =
                std::get<javelin::jmap::api::ThreadGetResponse>(threadResult);

            emitProgress(
                QStringLiteral("Fetched %1 thread records.").arg(parsedThreads.list.size()));

            const auto emailResult = reader.require(emailHandle);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::ResponseReaderError>(&emailResult))
            {
                co_return operationError(*error);
            }
            const auto& parsedEmails = std::get<javelin::jmap::api::EmailGetResponse>(emailResult);
            qCDebug(logMailboxSync).noquote()
                << "fetched thread emails" << QString::fromStdString(accountId)
                << QString::fromStdString(mailboxId) << "state"
                << QString::fromStdString(parsedEmails.state) << "emails"
                << emailMailboxSummary(parsedEmails.list);

            emitProgress(
                QStringLiteral("Fetched %1 thread messages.").arg(parsedEmails.list.size()));

            co_return CollapsedMailboxFetch{
                .queryState = parsedQuery.queryState,
                .emailState = parsedEmails.state,
                .threads = parsedThreads.list,
                .emails = parsedEmails.list,
                .representativeCount = parsedQuery.ids.size(),
                .total = parsedQuery.total,
            };
        }

        [[nodiscard]] QCoro::Task<std::variant<IncrementalCollapsedMailboxRefresh, OperationError>>
        refreshCollapsedMailboxThreadsIncrementally(
            javelin::jmap::cache::DatabaseConnection& databaseConnection,
            javelin::jmap::api::MethodCaller& methodCaller,
            javelin::jmap::api::ApiRequestContext apiRequestContext, std::string accountId,
            std::string mailboxId, std::string sinceQueryState, std::string sinceEmailState,
            std::function<void(const QString&)> reportProgress)
        {
            const auto emitProgress = [&reportProgress](const QString& message)
            {
                if (reportProgress)
                {
                    reportProgress(message);
                }
            };

            javelin::jmap::api::RequestBuilder builder;
            builder.useCore().useMail();

            const auto queryChangesRequest = javelin::jmap::api::emailQueryChanges({
                .accountId = std::string{accountId},
                .sinceQueryState = std::string{sinceQueryState},
                .maxChanges = std::nullopt,
                .upToId = std::nullopt,
                .filter =
                    javelin::jmap::api::EmailQueryFilter{
                        .inMailbox = std::string{mailboxId},
                        .text = std::nullopt,
                    },
                .sort =
                    {
                        javelin::jmap::api::EmailQuerySort{
                            .property = "receivedAt",
                            .isAscending = false,
                        },
                    },
                .collapseThreads = true,
            });
            if (!queryChangesRequest.has_value())
            {
                co_return OperationError{
                    .message = QStringLiteral("Failed to encode the Email/queryChanges request."),
                };
            }
            const auto queryChangesHandle =
                builder.call(*queryChangesRequest, "mailbox-query-changes");

            const auto emailChangesRequest = javelin::jmap::api::emailChanges({
                .accountId = std::string{accountId},
                .sinceState = std::string{sinceEmailState},
                .maxChanges = std::nullopt,
            });
            if (!emailChangesRequest.has_value())
            {
                co_return OperationError{
                    .message = QStringLiteral("Failed to encode the Email/changes request."),
                };
            }
            const auto emailChangesHandle = builder.call(*emailChangesRequest, "email-changes");

            const auto envelopeResult = co_await methodCaller.call(apiRequestContext, builder);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::TransportError>(&envelopeResult))
            {
                co_return operationError(*error);
            }
            if (const auto* error = std::get_if<javelin::jmap::api::AuthError>(&envelopeResult))
            {
                co_return operationError(*error);
            }
            if (const auto* error = std::get_if<javelin::jmap::api::ProtocolError>(&envelopeResult))
            {
                co_return operationError(*error);
            }

            const auto& envelope = std::get<javelin::jmap::api::ResponseEnvelope>(envelopeResult);
            const javelin::jmap::api::ResponseReader reader{envelope};

            const auto queryChangesResult = reader.require(queryChangesHandle);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::ResponseReaderError>(&queryChangesResult))
            {
                if (isRecoverableIncrementalError(*error))
                {
                    co_return IncrementalCollapsedMailboxRefresh{
                        .requiresFullFetch = true,
                        .queryState = {},
                        .emailState = {},
                        .updatedEmails = {},
                        .representativeCount = 0,
                        .changedEmailIds = {},
                        .insertedEmailIds = {},
                        .removedEmailIds = {},
                        .requiresNotificationScan = false,
                    };
                }

                co_return operationError(*error);
            }
            const auto& queryChanges =
                std::get<javelin::jmap::api::EmailQueryChangesResponse>(queryChangesResult);

            const auto emailChangesResult = reader.require(emailChangesHandle);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::ResponseReaderError>(&emailChangesResult))
            {
                if (isRecoverableIncrementalError(*error))
                {
                    co_return IncrementalCollapsedMailboxRefresh{
                        .requiresFullFetch = true,
                        .queryState = {},
                        .emailState = {},
                        .updatedEmails = {},
                        .representativeCount = 0,
                        .changedEmailIds = {},
                        .insertedEmailIds = {},
                        .removedEmailIds = {},
                        .requiresNotificationScan = false,
                    };
                }

                co_return operationError(*error);
            }
            const auto& emailChanges =
                std::get<javelin::jmap::api::EmailChangesResponse>(emailChangesResult);

            std::vector<std::string> addedQueryIds;
            addedQueryIds.reserve(queryChanges.added.size() + emailChanges.created.size());
            for (const auto& added : queryChanges.added)
            {
                addedQueryIds.push_back(added.id);
            }
            addedQueryIds.insert(addedQueryIds.end(), emailChanges.created.begin(),
                                 emailChanges.created.end());
            addedQueryIds = deduplicatedIds(std::move(addedQueryIds));

            std::vector<std::string> removedIds = queryChanges.removed;
            removedIds.insert(removedIds.end(), emailChanges.destroyed.begin(),
                              emailChanges.destroyed.end());
            removedIds = deduplicatedIds(std::move(removedIds));

            if (queryChanges.hasMoreChanges || !queryChanges.added.empty() ||
                !queryChanges.removed.empty() || emailChanges.hasMoreChanges ||
                !emailChanges.created.empty() || !emailChanges.destroyed.empty())
            {
                co_return IncrementalCollapsedMailboxRefresh{
                    .requiresFullFetch = true,
                    .queryState = {},
                    .emailState = {},
                    .updatedEmails = {},
                    .representativeCount = 0,
                    .changedEmailIds = emailChanges.updated,
                    .insertedEmailIds = std::move(addedQueryIds),
                    .removedEmailIds = std::move(removedIds),
                    .requiresNotificationScan =
                        !queryChanges.added.empty() || !emailChanges.created.empty(),
                };
            }

            const auto representativeCountResult =
                currentRepresentativeCount(databaseConnection, accountId, mailboxId);
            if (const auto* error = std::get_if<OperationError>(&representativeCountResult))
            {
                co_return *error;
            }

            emitProgress(QStringLiteral("Mailbox delta contains %1 updated messages.")
                             .arg(static_cast<qulonglong>(emailChanges.updated.size())));

            javelin::jmap::cache::EmailRepository emailRepository{databaseConnection};
            const auto existingIdsResult =
                emailRepository.existingIds(accountId, emailChanges.updated);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&existingIdsResult))
            {
                co_return javelin::jmap::operationError(*error);
            }

            const auto& existingIds = std::get<std::vector<std::string>>(existingIdsResult);
            std::unordered_set<std::string> existingIdSet(existingIds.begin(), existingIds.end());
            const auto updatedIds = deduplicatedIds(emailChanges.updated);
            const bool hasMissingUpdatedIds =
                std::ranges::any_of(updatedIds, [&existingIdSet](const auto& emailId)
                                    { return !existingIdSet.contains(emailId); });
            if (hasMissingUpdatedIds)
            {
                co_return IncrementalCollapsedMailboxRefresh{
                    .requiresFullFetch = true,
                    .queryState = {},
                    .emailState = {},
                    .updatedEmails = {},
                    .representativeCount = 0,
                    .changedEmailIds = emailChanges.updated,
                    .insertedEmailIds = {},
                    .removedEmailIds = {},
                    .requiresNotificationScan = false,
                };
            }

            if (existingIds.empty())
            {
                co_return IncrementalCollapsedMailboxRefresh{
                    .requiresFullFetch = false,
                    .queryState = queryChanges.newQueryState,
                    .emailState = emailChanges.newState,
                    .updatedEmails = {},
                    .representativeCount = std::get<std::size_t>(representativeCountResult),
                    .changedEmailIds = emailChanges.updated,
                    .insertedEmailIds = {},
                    .removedEmailIds = {},
                    .requiresNotificationScan = false,
                };
            }

            const auto updatedEmailsRequest = javelin::jmap::api::emailGet({
                .accountId = std::string{accountId},
                .ids = existingIds,
                .idsReference = std::nullopt,
                .properties = std::nullopt,
            });
            if (!updatedEmailsRequest.has_value())
            {
                co_return OperationError{
                    .message = QStringLiteral("Failed to encode the Email/get delta request."),
                };
            }

            javelin::jmap::api::RequestBuilder deltaBuilder;
            deltaBuilder.useCore().useMail();
            const auto updatedEmailsHandle =
                deltaBuilder.call(*updatedEmailsRequest, "updated-emails-get");

            const auto updatedEnvelopeResult =
                co_await methodCaller.call(apiRequestContext, deltaBuilder);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::TransportError>(&updatedEnvelopeResult))
            {
                co_return operationError(*error);
            }
            if (const auto* error =
                    std::get_if<javelin::jmap::api::AuthError>(&updatedEnvelopeResult))
            {
                co_return operationError(*error);
            }
            if (const auto* error =
                    std::get_if<javelin::jmap::api::ProtocolError>(&updatedEnvelopeResult))
            {
                co_return operationError(*error);
            }

            const auto& updatedEnvelope =
                std::get<javelin::jmap::api::ResponseEnvelope>(updatedEnvelopeResult);
            const javelin::jmap::api::ResponseReader updatedReader{updatedEnvelope};
            const auto updatedEmailsResult = updatedReader.require(updatedEmailsHandle);
            if (const auto* error =
                    std::get_if<javelin::jmap::api::ResponseReaderError>(&updatedEmailsResult))
            {
                co_return operationError(*error);
            }

            co_return IncrementalCollapsedMailboxRefresh{
                .requiresFullFetch = false,
                .queryState = queryChanges.newQueryState,
                .emailState = emailChanges.newState,
                .updatedEmails =
                    std::get<javelin::jmap::api::EmailGetResponse>(updatedEmailsResult).list,
                .representativeCount = std::get<std::size_t>(representativeCountResult),
                .changedEmailIds = emailChanges.updated,
                .insertedEmailIds = {},
                .removedEmailIds = {},
                .requiresNotificationScan = false,
            };
        }

    } // namespace

    MailboxRefreshExecutor::MailboxRefreshExecutor(
        javelin::jmap::cache::DatabaseConnection& databaseConnection,
        javelin::jmap::api::MethodCaller& methodCaller,
        javelin::jmap::api::ApiRequestContext apiRequestContext)
        : m_databaseConnection(databaseConnection), m_methodCaller(methodCaller),
          m_apiRequestContext(std::move(apiRequestContext))
    {
    }

    QCoro::Task<MailboxRefreshResult> MailboxRefreshExecutor::refreshCollapsedMailbox(
        std::string accountId, std::string mailboxId,
        std::function<void(const QString&)> reportProgress, const bool forceFullRefresh) const
    {
        const auto emitProgress = [&reportProgress](const QString& message)
        {
            if (reportProgress)
            {
                reportProgress(message);
            }
        };

        javelin::jmap::cache::SyncStateRepository syncStateRepository{m_databaseConnection};
        javelin::jmap::cache::EmailRepository emailRepository{m_databaseConnection};
        javelin::jmap::sync::ConsistencyDomainRepository consistencyRepository{
            m_databaseConnection};
        const auto refreshFenceResult = consistencyRepository.captureRefresh({
            .accountId = accountId,
            .dataType = "Email",
        });
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&refreshFenceResult))
        {
            co_return javelin::jmap::operationError(*error);
        }
        const auto refreshFence = std::get<javelin::jmap::sync::RefreshFence>(refreshFenceResult);
        const auto refreshIsCurrent = [&consistencyRepository,
                                       &refreshFence]() -> std::variant<bool, OperationError>
        {
            const auto current = consistencyRepository.isCurrent(refreshFence);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&current))
            {
                return javelin::jmap::operationError(*error);
            }
            return std::get<bool>(current);
        };
        const javelin::jmap::sync::SyncPlanner syncPlanner{syncStateRepository};
        const auto queryKey = mailboxQuerySyncKey(accountId, mailboxId);
        const auto emailKey = emailSyncKey(accountId);

        const auto queryPlanResult = syncPlanner.plan(queryKey);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&queryPlanResult))
        {
            co_return javelin::jmap::operationError(*error);
        }

        const auto emailPlanResult = syncPlanner.plan(emailKey);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&emailPlanResult))
        {
            co_return javelin::jmap::operationError(*error);
        }

        const auto& queryPlan = std::get<javelin::jmap::sync::SyncPlan>(queryPlanResult);
        const auto& emailPlan = std::get<javelin::jmap::sync::SyncPlan>(emailPlanResult);

        std::size_t representativeCount = 0;
        bool usedIncrementalRefresh = false;
        std::vector<std::string> changedEmailIds;
        std::vector<std::string> insertedEmailIds;
        std::vector<std::string> removedEmailIds;
        bool requiresNotificationScan = false;
        std::vector<RefreshNotificationCandidate> notificationCandidates;
        std::optional<std::vector<std::string>> previousMailboxEmailIds;

        if (forceFullRefresh ||
            (queryPlan.kind == javelin::jmap::sync::SyncPlanKind::IncrementalChanges &&
             emailPlan.kind == javelin::jmap::sync::SyncPlanKind::IncrementalChanges))
        {
            const auto previousIdsResult =
                mailboxEmailIds(m_databaseConnection, accountId, mailboxId);
            if (const auto* error = std::get_if<OperationError>(&previousIdsResult))
            {
                co_return *error;
            }

            previousMailboxEmailIds =
                std::get<std::vector<std::string>>(std::move(previousIdsResult));
        }

        if (!forceFullRefresh &&
            queryPlan.kind == javelin::jmap::sync::SyncPlanKind::IncrementalChanges &&
            queryPlan.sinceState.has_value() &&
            emailPlan.kind == javelin::jmap::sync::SyncPlanKind::IncrementalChanges &&
            emailPlan.sinceState.has_value())
        {
            emitProgress(QStringLiteral("Checking for mailbox deltas..."));
            const auto incrementalResult = co_await refreshCollapsedMailboxThreadsIncrementally(
                m_databaseConnection, m_methodCaller, m_apiRequestContext, accountId, mailboxId,
                *queryPlan.sinceState, *emailPlan.sinceState, reportProgress);
            if (const auto* error = std::get_if<OperationError>(&incrementalResult))
            {
                co_return *error;
            }

            const auto& incremental =
                std::get<IncrementalCollapsedMailboxRefresh>(incrementalResult);
            const auto current = refreshIsCurrent();
            if (const auto* error = std::get_if<OperationError>(&current))
            {
                co_return *error;
            }
            if (!std::get<bool>(current))
            {
                co_return MailboxRefreshSummary{
                    .representativeCount = 0,
                    .usedIncrementalRefresh = false,
                    .superseded = true,
                    .changedEmailIds = {},
                    .insertedEmailIds = {},
                    .removedEmailIds = {},
                    .requiresNotificationScan = false,
                    .notificationCandidates = {},
                };
            }
            changedEmailIds = incremental.changedEmailIds;
            insertedEmailIds = incremental.insertedEmailIds;
            removedEmailIds = incremental.removedEmailIds;
            requiresNotificationScan = incremental.requiresNotificationScan;
            if (!incremental.requiresFullFetch)
            {
                if (!incremental.updatedEmails.empty())
                {
                    if (const auto error =
                            emailRepository.upsertMany(accountId, incremental.updatedEmails))
                    {
                        co_return javelin::jmap::operationError(*error);
                    }

                    std::vector<std::string> updatedEmailIds;
                    updatedEmailIds.reserve(incremental.updatedEmails.size());
                    for (const auto& email : incremental.updatedEmails)
                    {
                        updatedEmailIds.push_back(email.id);
                    }

                    if (const auto error = reapplyPendingEmailPatches(
                            m_databaseConnection, accountId, std::move(updatedEmailIds)))
                    {
                        co_return *error;
                    }
                }

                if (const auto error = syncStateRepository.upsert(queryKey, incremental.queryState))
                {
                    co_return javelin::jmap::operationError(*error);
                }
                if (const auto error = syncStateRepository.upsert(emailKey, incremental.emailState))
                {
                    co_return javelin::jmap::operationError(*error);
                }

                representativeCount = incremental.representativeCount;
                usedIncrementalRefresh = true;
                emitProgress(
                    QStringLiteral("Applied mailbox delta without rebuilding the mailbox cache."));
            }
        }

        if (!usedIncrementalRefresh)
        {
            emitProgress(QStringLiteral("Refreshing mailbox window from the server..."));
            const auto fetchResult = co_await fetchCollapsedMailboxThreads(
                m_methodCaller, m_apiRequestContext, accountId, mailboxId, reportProgress);
            if (const auto* error = std::get_if<OperationError>(&fetchResult))
            {
                co_return *error;
            }

            auto fetch = std::get<CollapsedMailboxFetch>(std::move(fetchResult));
            const auto current = refreshIsCurrent();
            if (const auto* error = std::get_if<OperationError>(&current))
            {
                co_return *error;
            }
            if (!std::get<bool>(current))
            {
                co_return MailboxRefreshSummary{
                    .representativeCount = 0,
                    .usedIncrementalRefresh = false,
                    .superseded = true,
                    .changedEmailIds = {},
                    .insertedEmailIds = {},
                    .removedEmailIds = {},
                    .requiresNotificationScan = false,
                    .notificationCandidates = {},
                };
            }
            const auto currentFetchedMailboxEmailIds =
                fetchedMailboxEmailIds(fetch.emails, mailboxId);

            javelin::jmap::cache::ThreadRepository threadRepository{m_databaseConnection};
            if (const auto error = threadRepository.upsertMany(accountId, fetch.threads))
            {
                co_return javelin::jmap::operationError(*error);
            }

            if (const auto error = emailRepository.upsertMany(accountId, fetch.emails))
            {
                co_return javelin::jmap::operationError(*error);
            }

            std::vector<std::string> fetchedEmailIds;
            fetchedEmailIds.reserve(fetch.emails.size());
            for (const auto& email : fetch.emails)
            {
                fetchedEmailIds.push_back(email.id);
            }
            if (const auto error = reapplyPendingEmailPatches(m_databaseConnection, accountId,
                                                              std::move(fetchedEmailIds)))
            {
                co_return *error;
            }

            representativeCount = fetch.representativeCount;
            emitProgress(
                QStringLiteral("Cached %1 threaded conversations for the selected mailbox.")
                    .arg(representativeCount));

            const bool fetchedCompleteMailbox =
                fetch.total.has_value() && *fetch.total == fetch.representativeCount;
            if (previousMailboxEmailIds.has_value() && fetchedCompleteMailbox)
            {
                std::unordered_set<std::string> previousIdsSet(previousMailboxEmailIds->begin(),
                                                               previousMailboxEmailIds->end());
                std::unordered_set<std::string> currentIdsSet(currentFetchedMailboxEmailIds.begin(),
                                                              currentFetchedMailboxEmailIds.end());

                std::vector<std::string> insertedAfterFullFetch;
                insertedAfterFullFetch.reserve(currentFetchedMailboxEmailIds.size());
                for (const auto& emailId : currentFetchedMailboxEmailIds)
                {
                    if (!previousIdsSet.contains(emailId))
                    {
                        insertedAfterFullFetch.push_back(emailId);
                    }
                }

                std::vector<std::string> removedAfterFullFetch;
                removedAfterFullFetch.reserve(previousMailboxEmailIds->size());
                for (const auto& emailId : *previousMailboxEmailIds)
                {
                    if (!currentIdsSet.contains(emailId))
                    {
                        removedAfterFullFetch.push_back(emailId);
                    }
                }

                if (!insertedAfterFullFetch.empty())
                {
                    insertedAfterFullFetch.insert(insertedAfterFullFetch.end(),
                                                  insertedEmailIds.begin(), insertedEmailIds.end());
                    insertedEmailIds = deduplicatedIds(std::move(insertedAfterFullFetch));
                    requiresNotificationScan = true;
                }

                if (!removedAfterFullFetch.empty())
                {
                    const auto removedMailboxEmailIds = removedAfterFullFetch;
                    qCDebug(logMailboxSync).noquote()
                        << "removing stale mailbox membership" << QString::fromStdString(accountId)
                        << QString::fromStdString(mailboxId) << "emailIds"
                        << joinIds(removedMailboxEmailIds);
                    removedAfterFullFetch.insert(removedAfterFullFetch.end(),
                                                 removedEmailIds.begin(), removedEmailIds.end());
                    if (const auto error = emailRepository.removeFromMailbox(
                            accountId, mailboxId, removedMailboxEmailIds))
                    {
                        co_return javelin::jmap::operationError(*error);
                    }
                    removedEmailIds = deduplicatedIds(std::move(removedAfterFullFetch));
                }
            }

            if (const auto error = syncStateRepository.upsert(queryKey, fetch.queryState))
            {
                co_return javelin::jmap::operationError(*error);
            }

            if (!fetch.emailState.empty())
            {
                if (const auto error = syncStateRepository.upsert(emailKey, fetch.emailState))
                {
                    co_return javelin::jmap::operationError(*error);
                }
            }
        }

        if (requiresNotificationScan && !insertedEmailIds.empty())
        {
            const RefreshNotificationPlanner planner{m_databaseConnection};
            const auto candidatesResult =
                planner.plan(accountId, mailboxId,
                             MailboxRefreshSummary{
                                 .representativeCount = representativeCount,
                                 .usedIncrementalRefresh = usedIncrementalRefresh,
                                 .changedEmailIds = changedEmailIds,
                                 .insertedEmailIds = insertedEmailIds,
                                 .removedEmailIds = removedEmailIds,
                                 .requiresNotificationScan = requiresNotificationScan,
                                 .notificationCandidates = {},
                             });
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&candidatesResult))
            {
                co_return javelin::jmap::operationError(*error);
            }

            notificationCandidates =
                std::get<std::vector<RefreshNotificationCandidate>>(candidatesResult);
        }

        co_return MailboxRefreshSummary{
            .representativeCount = representativeCount,
            .usedIncrementalRefresh = usedIncrementalRefresh,
            .changedEmailIds = std::move(changedEmailIds),
            .insertedEmailIds = std::move(insertedEmailIds),
            .removedEmailIds = std::move(removedEmailIds),
            .requiresNotificationScan = requiresNotificationScan,
            .notificationCandidates = std::move(notificationCandidates),
        };
    }

} // namespace javelin::jmap::sync
