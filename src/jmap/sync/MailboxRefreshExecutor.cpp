#include "jmap/sync/MailboxRefreshExecutor.h"

#include "jmap/api/MailMethods.h"
#include "jmap/api/ResponseReader.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailboxWindowRepository.h"
#include "jmap/cache/SyncStateRepository.h"
#include "jmap/cache/ThreadRepository.h"
#include "jmap/sync/ConsistencyDomain.h"
#include "jmap/sync/EmailMutationJournal.h"
#include "jmap/sync/MailboxQueryDescriptor.h"
#include "jmap/sync/RefreshNotificationPlanner.h"
#include "jmap/sync/SyncPlanner.h"

#include <QDebug>
#include <QLoggingCategory>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <array>
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

        [[nodiscard]] std::variant<std::vector<std::string>, OperationError>
        changedMailboxIds(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                          const std::string_view accountId,
                          const std::vector<javelin::jmap::domain::Email>& emails)
        {
            std::vector<std::string> mailboxIds;
            QSqlQuery previous{databaseConnection.database()};
            previous.prepare(QStringLiteral(
                "SELECT mailbox_id FROM email_mailboxes WHERE account_id=:account_id "
                "AND email_id=:email_id"));
            for (const auto& email : emails)
            {
                std::unordered_set<std::string> previousMailboxIds;
                previous.bindValue(QStringLiteral(":account_id"),
                                   QString::fromStdString(std::string{accountId}));
                previous.bindValue(QStringLiteral(":email_id"), QString::fromStdString(email.id));
                if (!previous.exec())
                {
                    return javelin::jmap::operationError(javelin::jmap::cache::DatabaseError{
                        .code = javelin::jmap::cache::DatabaseErrorCode::QueryFailed,
                        .message = QStringLiteral("Read affected email mailboxes: ") +
                                   previous.lastError().text(),
                    });
                }
                while (previous.next())
                    previousMailboxIds.insert(previous.value(0).toString().toStdString());
                previous.finish();

                const std::unordered_set<std::string> nextMailboxIds(email.mailboxIds.begin(),
                                                                     email.mailboxIds.end());
                for (const auto& mailboxId : previousMailboxIds)
                {
                    if (!nextMailboxIds.contains(mailboxId))
                        mailboxIds.push_back(mailboxId);
                }
                for (const auto& mailboxId : nextMailboxIds)
                {
                    if (!previousMailboxIds.contains(mailboxId))
                        mailboxIds.push_back(mailboxId);
                }
            }
            return deduplicatedIds(std::move(mailboxIds));
        }

        [[nodiscard]] std::optional<OperationError>
        invalidateOtherMailboxWindows(javelin::jmap::cache::DatabaseTransaction& transaction,
                                      javelin::jmap::cache::DatabaseConnection& databaseConnection,
                                      const std::string_view accountId,
                                      const std::string_view refreshedMailboxId,
                                      const std::vector<std::string>& affectedMailboxIds)
        {
            javelin::jmap::cache::MailboxWindowRepository windows{databaseConnection};
            for (const auto& mailboxId : affectedMailboxIds)
            {
                if (mailboxId == refreshedMailboxId)
                    continue;
                if (const auto error = windows.invalidateMailbox(transaction, accountId, mailboxId))
                    return javelin::jmap::operationError(*error);
            }
            return std::nullopt;
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
                                   std::vector<std::string> emailIds,
                                   const std::string_view serverState)
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
            std::vector<std::string> acceptedMutationIds;

            const auto patchSatisfied = [](const javelin::jmap::domain::Email& email,
                                           const javelin::jmap::sync::EmailPatchMutation& patch)
            {
                const auto contains =
                    [](const std::vector<std::string>& values, const std::string& value)
                { return std::ranges::find(values, value) != values.end(); };
                return std::ranges::all_of(patch.addMailboxIds, [&email, &contains](const auto& id)
                                           { return contains(email.mailboxIds, id); }) &&
                       std::ranges::none_of(patch.removeMailboxIds,
                                            [&email, &contains](const auto& id)
                                            { return contains(email.mailboxIds, id); }) &&
                       std::ranges::all_of(patch.addKeywords,
                                           [&email, &contains](const auto& keyword)
                                           { return contains(email.keywords, keyword); }) &&
                       std::ranges::none_of(patch.removeKeywords,
                                            [&email, &contains](const auto& keyword)
                                            { return contains(email.keywords, keyword); });
            };

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

                auto pendingActions = activeEmailMutations(
                    std::get<std::vector<javelin::jmap::sync::EmailMutationRecord>>(pendingResult));
                if (pendingActions.empty())
                {
                    continue;
                }
                std::erase_if(pendingActions,
                              [&email, &acceptedMutationIds, &patchSatisfied](const auto& action)
                              {
                                  if (action.status !=
                                          javelin::jmap::sync::MutationStatus::Unknown ||
                                      action.patch.destroy || !patchSatisfied(*email, action.patch))
                                      return false;
                                  acceptedMutationIds.push_back(action.mutationId);
                                  return true;
                              });

                reconciledEmails.push_back(
                    javelin::jmap::sync::projectEmailMutations(*email, pendingActions));
            }

            if (!reconciledEmails.empty() || !acceptedMutationIds.empty())
            {
                auto transactionResult = javelin::jmap::sync::MutationProjectionTransaction::begin(
                    databaseConnection, QStringLiteral("Rebase Email mutations"));
                if (const auto* error =
                        std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
                    return javelin::jmap::operationError(*error);
                auto transaction = std::get<javelin::jmap::sync::MutationProjectionTransaction>(
                    std::move(transactionResult));
                if (!acceptedMutationIds.empty())
                {
                    const std::array domains{javelin::jmap::sync::ConsistencyDomain{
                        .accountId = std::string{accountId},
                        .dataType = "Email",
                    }};
                    if (const auto error = transaction.advance(domains))
                        return javelin::jmap::operationError(*error);
                    for (const auto& mutationId : acceptedMutationIds)
                    {
                        if (const auto error = transaction.transition(
                                mutationId, javelin::jmap::sync::MutationStatus::Accepted,
                                serverState))
                            return javelin::jmap::operationError(*error);
                        if (const auto error = transaction.remove(mutationId))
                            return javelin::jmap::operationError(*error);
                    }
                }
                if (const auto error = emailRepository.upsertMany(transaction.cacheTransaction(),
                                                                  accountId, reconciledEmails))
                    return javelin::jmap::operationError(*error);
                if (const auto error = transaction.commit())
                    return javelin::jmap::operationError(*error);
            }

            return std::nullopt;
        }

        struct CollapsedMailboxFetch
        {
            std::string queryState;
            std::string emailState;
            std::vector<std::string> representativeIds;
            std::vector<javelin::jmap::domain::Thread> threads;
            std::vector<javelin::jmap::domain::Email> emails;
            std::size_t representativeCount = 0;
            std::size_t returnedLimit = 100;
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
            std::vector<javelin::jmap::cache::MailboxWindowAddition> windowAdditions;
            std::vector<std::string> removedEmailIds;
            bool requiresNotificationScan = false;
            std::vector<std::string> destroyedEmailIds;
            bool queryMembershipChanged = false;
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
                .anchor = std::nullopt,
                .anchorOffset = 0,
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
                    .representativeIds = {},
                    .threads = {},
                    .emails = {},
                    .representativeCount = 0,
                    .returnedLimit = static_cast<std::size_t>(parsedQuery.limit.value_or(100)),
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
                .representativeIds = parsedQuery.ids,
                .threads = parsedThreads.list,
                .emails = parsedEmails.list,
                .representativeCount = parsedQuery.ids.size(),
                .returnedLimit = static_cast<std::size_t>(parsedQuery.limit.value_or(100)),
                .total = parsedQuery.total,
            };
        }

        [[nodiscard]] QCoro::Task<std::variant<IncrementalCollapsedMailboxRefresh, OperationError>>
        refreshCollapsedMailboxThreadsIncrementally(
            javelin::jmap::cache::DatabaseConnection& databaseConnection,
            javelin::jmap::api::MethodCaller& methodCaller,
            javelin::jmap::api::ApiRequestContext apiRequestContext, std::string accountId,
            std::string mailboxId, std::string sinceQueryState, std::string sinceEmailState,
            std::optional<std::string> upToId, std::function<void(const QString&)> reportProgress,
            const bool refreshAccountEmailState)
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
                .upToId = std::move(upToId),
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
                .calculateTotal = true,
            });
            if (!queryChangesRequest.has_value())
            {
                co_return OperationError{
                    .message = QStringLiteral("Failed to encode the Email/queryChanges request."),
                };
            }
            const auto queryChangesHandle =
                builder.call(*queryChangesRequest, "mailbox-query-changes");

            std::optional<javelin::jmap::api::CallHandle<javelin::jmap::api::EmailChangesResponse>>
                emailChangesHandle;
            if (refreshAccountEmailState)
            {
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
                emailChangesHandle = builder.call(*emailChangesRequest, "email-changes");
            }

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
                        .windowAdditions = {},
                        .removedEmailIds = {},
                        .requiresNotificationScan = false,
                        .destroyedEmailIds = {},
                        .queryMembershipChanged = false,
                    };
                }

                co_return operationError(*error);
            }
            const auto& queryChanges =
                std::get<javelin::jmap::api::EmailQueryChangesResponse>(queryChangesResult);

            javelin::jmap::api::EmailChangesResponse emailChanges{
                .accountId = accountId,
                .oldState = sinceEmailState,
                .newState = sinceEmailState,
                .hasMoreChanges = false,
                .created = {},
                .updated = {},
                .destroyed = {},
            };
            if (emailChangesHandle.has_value())
            {
                const auto emailChangesResult = reader.require(*emailChangesHandle);
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
                            .windowAdditions = {},
                            .removedEmailIds = {},
                            .requiresNotificationScan = false,
                            .destroyedEmailIds = {},
                            .queryMembershipChanged = false,
                        };
                    }

                    co_return operationError(*error);
                }
                emailChanges =
                    std::get<javelin::jmap::api::EmailChangesResponse>(emailChangesResult);
            }

            std::vector<std::string> addedQueryIds;
            std::vector<javelin::jmap::cache::MailboxWindowAddition> windowAdditions;
            addedQueryIds.reserve(queryChanges.added.size() + emailChanges.created.size());
            windowAdditions.reserve(queryChanges.added.size());
            for (const auto& added : queryChanges.added)
            {
                addedQueryIds.push_back(added.id);
                windowAdditions.push_back({
                    .emailId = added.id,
                    .index = static_cast<std::size_t>(added.index),
                });
            }
            addedQueryIds.insert(addedQueryIds.end(), emailChanges.created.begin(),
                                 emailChanges.created.end());
            addedQueryIds = deduplicatedIds(std::move(addedQueryIds));

            std::vector<std::string> removedIds = queryChanges.removed;
            removedIds.insert(removedIds.end(), emailChanges.destroyed.begin(),
                              emailChanges.destroyed.end());
            removedIds = deduplicatedIds(std::move(removedIds));

            const bool requiresFullFetch = queryChanges.hasMoreChanges ||
                                           emailChanges.hasMoreChanges ||
                                           !queryChanges.total.has_value();
            const auto representativeCount =
                static_cast<std::size_t>(queryChanges.total.value_or(0));

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

            auto existingIds = std::get<std::vector<std::string>>(existingIdsResult);
            for (const auto& added : queryChanges.added)
                existingIds.push_back(added.id);
            existingIds = deduplicatedIds(std::move(existingIds));

            if (existingIds.empty())
            {
                co_return IncrementalCollapsedMailboxRefresh{
                    .requiresFullFetch = requiresFullFetch,
                    .queryState = queryChanges.newQueryState,
                    .emailState = emailChanges.newState,
                    .updatedEmails = {},
                    .representativeCount = representativeCount,
                    .changedEmailIds = emailChanges.updated,
                    .insertedEmailIds = std::move(addedQueryIds),
                    .windowAdditions = std::move(windowAdditions),
                    .removedEmailIds = std::move(removedIds),
                    .requiresNotificationScan =
                        !queryChanges.added.empty() || !emailChanges.created.empty(),
                    .destroyedEmailIds = emailChanges.destroyed,
                    .queryMembershipChanged =
                        !queryChanges.added.empty() || !queryChanges.removed.empty(),
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
                .requiresFullFetch = requiresFullFetch,
                .queryState = queryChanges.newQueryState,
                .emailState = emailChanges.newState,
                .updatedEmails =
                    std::get<javelin::jmap::api::EmailGetResponse>(updatedEmailsResult).list,
                .representativeCount = representativeCount,
                .changedEmailIds = emailChanges.updated,
                .insertedEmailIds = std::move(addedQueryIds),
                .windowAdditions = std::move(windowAdditions),
                .removedEmailIds = std::move(removedIds),
                .requiresNotificationScan =
                    !queryChanges.added.empty() || !emailChanges.created.empty(),
                .destroyedEmailIds = emailChanges.destroyed,
                .queryMembershipChanged =
                    !queryChanges.added.empty() || !queryChanges.removed.empty(),
            };
        }

    } // namespace

    std::optional<OperationError>
    rebaseActiveEmailProjections(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                                 const std::string_view accountId,
                                 std::vector<std::string> emailIds,
                                 const std::string_view serverState)
    {
        return reapplyPendingEmailPatches(databaseConnection, accountId, std::move(emailIds),
                                          serverState);
    }

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
        std::function<void(const QString&)> reportProgress, const bool forceFullRefresh,
        const bool refreshAccountEmailState) const
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

        constexpr std::size_t canonicalWindowLimit = 100;
        const auto canonicalQueryKey = mailboxQueryKey(collapsedMailboxQueryDescriptor(mailboxId));
        javelin::jmap::cache::MailboxWindowRepository canonicalWindows{m_databaseConnection};
        const auto canonicalWindowResult =
            canonicalWindows.find(accountId, canonicalQueryKey, 0, canonicalWindowLimit);
        const auto* canonicalWindow =
            std::get_if<std::optional<javelin::jmap::cache::MailboxWindowRecord>>(
                &canonicalWindowResult);
        if (canonicalWindow == nullptr)
        {
            co_return javelin::jmap::operationError(
                std::get<javelin::jmap::cache::DatabaseError>(canonicalWindowResult));
        }
        const bool canonicalWindowIsAuthoritative =
            canonicalWindow->has_value() &&
            javelin::jmap::cache::isPaginationAuthoritative((*canonicalWindow)->coverage);
        const bool requireFullMaterialization = forceFullRefresh || !canonicalWindowIsAuthoritative;
        std::optional<std::string> cachedPrefixTail;
        {
            QSqlQuery cachedTailQuery{m_databaseConnection.database()};
            cachedTailQuery.prepare(QStringLiteral(
                "SELECT i.email_id FROM mailbox_query_windows w INNER JOIN "
                "mailbox_query_window_items i ON i.account_id=w.account_id AND "
                "i.query_key=w.query_key AND i.requested_offset=w.requested_offset AND "
                "i.requested_limit=w.requested_limit WHERE w.account_id=:account AND "
                "w.mailbox_id=:mailbox AND w.query_key=:query_key "
                "AND w.coverage='server' ORDER BY "
                "w.requested_offset DESC,i.position DESC LIMIT 1"));
            cachedTailQuery.bindValue(QStringLiteral(":account"),
                                      QString::fromStdString(accountId));
            cachedTailQuery.bindValue(QStringLiteral(":mailbox"),
                                      QString::fromStdString(mailboxId));
            cachedTailQuery.bindValue(QStringLiteral(":query_key"),
                                      QString::fromStdString(canonicalQueryKey));
            if (!cachedTailQuery.exec())
                co_return javelin::jmap::operationError(javelin::jmap::cache::DatabaseError{
                    .code = javelin::jmap::cache::DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("Read cached mailbox prefix tail: ") +
                               cachedTailQuery.lastError().text(),
                });
            if (cachedTailQuery.next())
                cachedPrefixTail = cachedTailQuery.value(0).toString().toStdString();
        }

        std::size_t representativeCount = 0;
        bool usedIncrementalRefresh = false;
        std::vector<std::string> changedEmailIds;
        std::vector<std::string> insertedEmailIds;
        std::vector<std::string> removedEmailIds;
        std::vector<std::string> destroyedEmailIds;
        bool requiresNotificationScan = false;
        std::vector<RefreshNotificationCandidate> notificationCandidates;
        std::optional<std::vector<std::string>> previousMailboxEmailIds;

        const bool hasMailboxBaseline =
            queryPlan.kind == javelin::jmap::sync::SyncPlanKind::IncrementalChanges &&
            queryPlan.sinceState.has_value();

        if (!requireFullMaterialization &&
            queryPlan.kind == javelin::jmap::sync::SyncPlanKind::IncrementalChanges &&
            queryPlan.sinceState.has_value() &&
            emailPlan.kind == javelin::jmap::sync::SyncPlanKind::IncrementalChanges &&
            emailPlan.sinceState.has_value())
        {
            emitProgress(QStringLiteral("Checking for mailbox deltas..."));
            const auto incrementalResult = co_await refreshCollapsedMailboxThreadsIncrementally(
                m_databaseConnection, m_methodCaller, m_apiRequestContext, accountId, mailboxId,
                *queryPlan.sinceState, *emailPlan.sinceState, cachedPrefixTail, reportProgress,
                refreshAccountEmailState);
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
            destroyedEmailIds = incremental.destroyedEmailIds;
            requiresNotificationScan = incremental.requiresNotificationScan;
            const auto affectedMailboxIdsResult =
                changedMailboxIds(m_databaseConnection, accountId, incremental.updatedEmails);
            if (const auto* error = std::get_if<OperationError>(&affectedMailboxIdsResult))
                co_return *error;
            const auto& affectedMailboxes =
                std::get<std::vector<std::string>>(affectedMailboxIdsResult);
            if (!destroyedEmailIds.empty() || !incremental.updatedEmails.empty() ||
                !incremental.requiresFullFetch)
            {
                auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
                    m_databaseConnection, QStringLiteral("Reconcile mailbox query membership"));
                if (const auto* error =
                        std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
                    co_return javelin::jmap::operationError(*error);
                auto transaction = std::get<javelin::jmap::cache::DatabaseTransaction>(
                    std::move(transactionResult));
                if (!destroyedEmailIds.empty())
                {
                    if (const auto error =
                            emailRepository.removeMany(transaction, accountId, destroyedEmailIds))
                        co_return javelin::jmap::operationError(*error);
                }
                if (!incremental.updatedEmails.empty())
                {
                    if (const auto error = emailRepository.upsertMany(transaction, accountId,
                                                                      incremental.updatedEmails))
                        co_return javelin::jmap::operationError(*error);
                    if (const auto error =
                            invalidateOtherMailboxWindows(transaction, m_databaseConnection,
                                                          accountId, mailboxId, affectedMailboxes))
                        co_return *error;
                }
                if (!incremental.requiresFullFetch)
                {
                    javelin::jmap::cache::MailboxWindowRepository windowRepository{
                        m_databaseConnection};
                    if (const auto error = windowRepository.rebaseContiguousPrefix(
                            transaction, accountId, mailboxId, canonicalQueryKey,
                            *queryPlan.sinceState, incremental.queryState,
                            incremental.windowAdditions, incremental.removedEmailIds,
                            incremental.representativeCount))
                        co_return javelin::jmap::operationError(*error);
                }
                if (const auto error = transaction.commit())
                    co_return javelin::jmap::operationError(*error);
            }
            if (!incremental.updatedEmails.empty())
            {
                std::vector<std::string> updatedEmailIds;
                updatedEmailIds.reserve(incremental.updatedEmails.size());
                for (const auto& email : incremental.updatedEmails)
                    updatedEmailIds.push_back(email.id);

                if (const auto error = reapplyPendingEmailPatches(m_databaseConnection, accountId,
                                                                  std::move(updatedEmailIds),
                                                                  incremental.emailState))
                    co_return *error;
            }

            if (!incremental.requiresFullFetch)
            {
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
                    .canonicalWindowMaterialized = false,
                    .superseded = true,
                    .changedEmailIds = {},
                    .insertedEmailIds = {},
                    .removedEmailIds = {},
                    .requiresNotificationScan = false,
                    .notificationCandidates = {},
                };
            }
            const bool fetchedCompleteMailbox =
                fetch.total.has_value() && *fetch.total == fetch.representativeCount;
            if (hasMailboxBaseline && fetchedCompleteMailbox)
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
            const auto currentFetchedMailboxEmailIds =
                fetchedMailboxEmailIds(fetch.emails, mailboxId);

            javelin::jmap::cache::ThreadRepository threadRepository{m_databaseConnection};
            if (const auto error = threadRepository.upsertMany(accountId, fetch.threads))
            {
                co_return javelin::jmap::operationError(*error);
            }

            const auto affectedMailboxIdsResult =
                changedMailboxIds(m_databaseConnection, accountId, fetch.emails);
            if (const auto* error = std::get_if<OperationError>(&affectedMailboxIdsResult))
                co_return *error;
            auto emailTransactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
                m_databaseConnection, QStringLiteral("Materialize mailbox thread emails"));
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&emailTransactionResult))
                co_return javelin::jmap::operationError(*error);
            auto emailTransaction = std::get<javelin::jmap::cache::DatabaseTransaction>(
                std::move(emailTransactionResult));
            if (const auto error =
                    emailRepository.upsertMany(emailTransaction, accountId, fetch.emails))
            {
                co_return javelin::jmap::operationError(*error);
            }
            if (const auto error = invalidateOtherMailboxWindows(
                    emailTransaction, m_databaseConnection, accountId, mailboxId,
                    std::get<std::vector<std::string>>(affectedMailboxIdsResult)))
                co_return *error;
            if (const auto error = emailTransaction.commit())
                co_return javelin::jmap::operationError(*error);

            std::vector<std::string> fetchedEmailIds;
            fetchedEmailIds.reserve(fetch.emails.size());
            for (const auto& email : fetch.emails)
            {
                fetchedEmailIds.push_back(email.id);
            }
            if (const auto error = reapplyPendingEmailPatches(
                    m_databaseConnection, accountId, std::move(fetchedEmailIds), fetch.emailState))
            {
                co_return *error;
            }

            if (const auto error = canonicalWindows.replace({
                    .accountId = accountId,
                    .mailboxId = mailboxId,
                    .queryKey = canonicalQueryKey,
                    .requestedOffset = 0,
                    .requestedLimit = canonicalWindowLimit,
                    .position = 0,
                    .returnedLimit = fetch.returnedLimit,
                    .total = fetch.total,
                    .queryState = fetch.queryState,
                    .coverage = javelin::jmap::cache::QueryWindowCoverage::Server,
                    .emailIds = fetch.representativeIds,
                }))
            {
                co_return javelin::jmap::operationError(*error);
            }

            representativeCount = fetch.representativeCount;
            emitProgress(
                QStringLiteral("Cached %1 threaded conversations for the selected mailbox.")
                    .arg(representativeCount));

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
            .canonicalWindowMaterialized = !usedIncrementalRefresh,
            .changedEmailIds = std::move(changedEmailIds),
            .insertedEmailIds = std::move(insertedEmailIds),
            .removedEmailIds = std::move(removedEmailIds),
            .requiresNotificationScan = requiresNotificationScan,
            .notificationCandidates = std::move(notificationCandidates),
        };
    }

} // namespace javelin::jmap::sync
