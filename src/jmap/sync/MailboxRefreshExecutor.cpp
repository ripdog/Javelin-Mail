#include "jmap/sync/MailboxRefreshExecutor.h"

#include "jmap/api/MailMethods.h"
#include "jmap/api/ResponseReader.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailboxWindowRepository.h"
#include "jmap/cache/SyncStateRepository.h"
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
#include <unordered_map>
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

        [[nodiscard]] MailboxRefreshSummary supersededMailboxRefresh()
        {
            MailboxRefreshSummary summary;
            summary.superseded = true;
            return summary;
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

        [[nodiscard]] std::optional<OperationError>
        reapplyPendingEmailPatches(javelin::jmap::sync::MutationProjectionTransaction& transaction,
                                   javelin::jmap::cache::DatabaseConnection& databaseConnection,
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
            const auto activeResult = emailMutationJournal.listActive(accountId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&activeResult))
                return javelin::jmap::operationError(*error);

            const std::unordered_set<std::string> refreshedIds{ids.begin(), ids.end()};
            std::unordered_map<std::string, std::vector<javelin::jmap::sync::EmailMutationRecord>>
                activeByEmail;
            for (auto action :
                 std::get<std::vector<javelin::jmap::sync::EmailMutationRecord>>(activeResult))
            {
                if (!refreshedIds.contains(action.patch.emailId) ||
                    !javelin::jmap::sync::projectsOptimistically(action.status))
                    continue;
                activeByEmail[action.patch.emailId].push_back(std::move(action));
            }
            if (activeByEmail.empty())
                return std::nullopt;

            std::vector<javelin::jmap::domain::Email> reconciledEmails;
            reconciledEmails.reserve(activeByEmail.size());
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
                const auto pending = activeByEmail.find(emailId);
                if (pending == activeByEmail.end())
                    continue;

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

                auto pendingActions = std::move(pending->second);
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
                            mutationId, javelin::jmap::sync::MutationStatus::Accepted, serverState))
                        return javelin::jmap::operationError(*error);
                    if (const auto error = transaction.remove(mutationId))
                        return javelin::jmap::operationError(*error);
                }
            }
            if (!reconciledEmails.empty())
            {
                if (const auto error = emailRepository.upsertMany(transaction.cacheTransaction(),
                                                                  accountId, reconciledEmails))
                    return javelin::jmap::operationError(*error);
            }

            return std::nullopt;
        }

        struct CollapsedMailboxFetch
        {
            std::string queryState;
            std::string emailState;
            std::vector<std::string> representativeIds;
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
                                     std::string remoteAccountId, std::string mailboxId,
                                     std::function<void(const QString&)> reportProgress)
        {
            const auto emitProgress = [&reportProgress](const QString& message)
            {
                if (reportProgress)
                {
                    reportProgress(message);
                }
            };
            constexpr std::size_t nominalLimit = 100;
            const auto effectiveLimit =
                apiRequestContext.requestLimits.has_value()
                    ? std::min<std::size_t>(nominalLimit,
                                            static_cast<std::size_t>(
                                                apiRequestContext.requestLimits->maxObjectsInGet))
                    : nominalLimit;

            javelin::jmap::api::RequestBuilder builder;
            builder.useCore().useMail();
            const auto queryRequest = javelin::jmap::api::emailQuery({
                .accountId = std::string{remoteAccountId},
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
                .limit = static_cast<std::uint64_t>(effectiveLimit),
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

            const auto representativeRequest =
                javelin::jmap::api::emailGet(javelin::jmap::api::getRequestFrom(
                    std::string{remoteAccountId}, queryHandle, "/ids"));
            if (!representativeRequest.has_value())
            {
                co_return OperationError{
                    .message =
                        QStringLiteral("Failed to encode the representative Email/get request."),
                };
            }
            const auto representativeHandle =
                builder.call(*representativeRequest, "thread-ids-get");

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
            if (const auto accountError = validateResponseAccountId(
                    remoteAccountId, parsedQuery.accountId, u"Email/query"))
                co_return *accountError;
            qCDebug(logMailboxSync).noquote()
                << "query result" << QString::fromStdString(remoteAccountId)
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
            if (const auto accountError = validateResponseAccountId(
                    remoteAccountId, parsedRepresentatives.accountId, u"Email/get"))
                co_return *accountError;

            emitProgress(
                QStringLiteral("Fetched %1 representative emails for the selected mailbox.")
                    .arg(parsedRepresentatives.list.size()));

            std::unordered_map<std::string, const javelin::jmap::domain::Email*>
                representativesById;
            representativesById.reserve(parsedRepresentatives.list.size());
            for (const auto& email : parsedRepresentatives.list)
                representativesById.emplace(email.id, &email);
            std::vector<javelin::jmap::domain::Email> representatives;
            representatives.reserve(parsedQuery.ids.size());
            for (const auto& representativeId : parsedQuery.ids)
            {
                const auto representativeIt = representativesById.find(representativeId);
                if (representativeIt != representativesById.end())
                    representatives.push_back(*representativeIt->second);
            }
            if (representatives.size() != parsedQuery.ids.size())
            {
                co_return OperationError{
                    .message = QStringLiteral(
                                   "Email/get omitted %1 representatives from the mailbox window.")
                                   .arg(static_cast<qulonglong>(parsedQuery.ids.size() -
                                                                representatives.size())),
                };
            }

            co_return CollapsedMailboxFetch{
                .queryState = parsedQuery.queryState,
                .emailState = parsedRepresentatives.state,
                .representativeIds = parsedQuery.ids,
                .emails = std::move(representatives),
                .representativeCount = parsedQuery.ids.size(),
                .returnedLimit =
                    static_cast<std::size_t>(parsedQuery.limit.value_or(effectiveLimit)),
                .total = parsedQuery.total,
            };
        }

        [[nodiscard]] QCoro::Task<std::variant<IncrementalCollapsedMailboxRefresh, OperationError>>
        refreshCollapsedMailboxThreadsIncrementally(
            javelin::jmap::cache::DatabaseConnection& databaseConnection,
            javelin::jmap::api::MethodCaller& methodCaller,
            javelin::jmap::api::ApiRequestContext apiRequestContext, std::string accountId,
            std::string remoteAccountId, std::string mailboxId, std::string sinceQueryState,
            std::string sinceEmailState, std::optional<std::string> upToId,
            std::function<void(const QString&)> reportProgress, const bool refreshAccountEmailState)
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
                .accountId = std::string{remoteAccountId},
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
                    .accountId = std::string{remoteAccountId},
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
            if (const auto accountError = validateResponseAccountId(
                    remoteAccountId, queryChanges.accountId, u"Email/queryChanges"))
                co_return *accountError;

            javelin::jmap::api::EmailChangesResponse emailChanges{
                .accountId = remoteAccountId,
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
                if (const auto accountError = validateResponseAccountId(
                        remoteAccountId, emailChanges.accountId, u"Email/changes"))
                    co_return *accountError;
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
            std::vector<std::string> queryAddedIds;
            queryAddedIds.reserve(queryChanges.added.size());
            for (const auto& added : queryChanges.added)
                queryAddedIds.push_back(added.id);
            const auto cachedAddedIdsResult = emailRepository.existingIds(accountId, queryAddedIds);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&cachedAddedIdsResult))
            {
                co_return javelin::jmap::operationError(*error);
            }
            const auto& cachedAddedIds = std::get<std::vector<std::string>>(cachedAddedIdsResult);
            for (const auto& addedId : queryAddedIds)
            {
                if (std::ranges::find(cachedAddedIds, addedId) == cachedAddedIds.end())
                    existingIds.push_back(addedId);
            }
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
                .accountId = std::string{remoteAccountId},
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
            const auto& updatedEmails =
                std::get<javelin::jmap::api::EmailGetResponse>(updatedEmailsResult);
            if (const auto accountError = validateResponseAccountId(
                    remoteAccountId, updatedEmails.accountId, u"Email/get"))
                co_return *accountError;

            co_return IncrementalCollapsedMailboxRefresh{
                .requiresFullFetch = requiresFullFetch,
                .queryState = queryChanges.newQueryState,
                .emailState = emailChanges.newState,
                .updatedEmails = updatedEmails.list,
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
        auto transactionResult = MutationProjectionTransaction::begin(
            databaseConnection, QStringLiteral("Rebase Email mutations"));
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
            return operationError(*error);
        auto transaction = std::get<MutationProjectionTransaction>(std::move(transactionResult));
        if (const auto error = reapplyPendingEmailPatches(
                transaction, databaseConnection, accountId, std::move(emailIds), serverState))
            return error;
        if (const auto error = transaction.commit())
            return operationError(*error);
        return std::nullopt;
    }

    std::optional<OperationError>
    rebaseActiveEmailProjections(MutationProjectionTransaction& transaction,
                                 javelin::jmap::cache::DatabaseConnection& databaseConnection,
                                 const std::string_view accountId,
                                 std::vector<std::string> emailIds,
                                 const std::string_view serverState)
    {
        return reapplyPendingEmailPatches(transaction, databaseConnection, accountId,
                                          std::move(emailIds), serverState);
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
        const bool refreshAccountEmailState, std::string remoteAccountId) const
    {
        if (remoteAccountId.empty())
            remoteAccountId = accountId;

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
            javelin::jmap::cache::isPaginationAuthoritative((*canonicalWindow)->coverage,
                                                            (*canonicalWindow)->materialization);
        const auto previousRepresentativeIds =
            canonicalWindow->has_value()
                ? std::optional<std::vector<std::string>>{(*canonicalWindow)->emailIds}
                : std::nullopt;
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
                "AND w.coverage='server' AND w.materialization='complete' ORDER BY "
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

        if (!requireFullMaterialization &&
            queryPlan.kind == javelin::jmap::sync::SyncPlanKind::IncrementalChanges &&
            queryPlan.sinceState.has_value() &&
            emailPlan.kind == javelin::jmap::sync::SyncPlanKind::IncrementalChanges &&
            emailPlan.sinceState.has_value())
        {
            emitProgress(QStringLiteral("Checking for mailbox deltas..."));
            const auto incrementalResult = co_await refreshCollapsedMailboxThreadsIncrementally(
                m_databaseConnection, m_methodCaller, m_apiRequestContext, accountId,
                remoteAccountId, mailboxId, *queryPlan.sinceState, *emailPlan.sinceState,
                cachedPrefixTail, reportProgress, refreshAccountEmailState);
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
                co_return supersededMailboxRefresh();
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
                auto transactionResult = javelin::jmap::sync::MutationProjectionTransaction::begin(
                    m_databaseConnection, QStringLiteral("Reconcile mailbox query membership"));
                if (const auto* error =
                        std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
                    co_return javelin::jmap::operationError(*error);
                auto transaction = std::get<javelin::jmap::sync::MutationProjectionTransaction>(
                    std::move(transactionResult));
                auto& cacheTransaction = transaction.cacheTransaction();
                const auto fenceCurrent = consistencyRepository.isCurrent(refreshFence);
                if (const auto* error =
                        std::get_if<javelin::jmap::cache::DatabaseError>(&fenceCurrent))
                    co_return javelin::jmap::operationError(*error);
                if (!std::get<bool>(fenceCurrent))
                    co_return supersededMailboxRefresh();
                if (!incremental.requiresFullFetch)
                {
                    const auto emailAdvanced = syncStateRepository.advanceIfCurrent(
                        cacheTransaction, emailKey, *emailPlan.sinceState, incremental.emailState);
                    if (const auto* error =
                            std::get_if<javelin::jmap::cache::DatabaseError>(&emailAdvanced))
                        co_return javelin::jmap::operationError(*error);
                    if (!std::get<bool>(emailAdvanced))
                        co_return supersededMailboxRefresh();
                    const auto advanced = syncStateRepository.advanceIfCurrent(
                        cacheTransaction, queryKey, *queryPlan.sinceState, incremental.queryState);
                    if (const auto* error =
                            std::get_if<javelin::jmap::cache::DatabaseError>(&advanced))
                        co_return javelin::jmap::operationError(*error);
                    if (!std::get<bool>(advanced))
                        co_return supersededMailboxRefresh();
                }
                if (!destroyedEmailIds.empty())
                {
                    if (const auto error = emailRepository.removeMany(cacheTransaction, accountId,
                                                                      destroyedEmailIds))
                        co_return javelin::jmap::operationError(*error);
                }
                if (!incremental.updatedEmails.empty())
                {
                    if (const auto error = emailRepository.upsertMany(cacheTransaction, accountId,
                                                                      incremental.updatedEmails))
                        co_return javelin::jmap::operationError(*error);
                    if (const auto error =
                            invalidateOtherMailboxWindows(cacheTransaction, m_databaseConnection,
                                                          accountId, mailboxId, affectedMailboxes))
                        co_return *error;
                    std::vector<std::string> updatedEmailIds;
                    updatedEmailIds.reserve(incremental.updatedEmails.size());
                    for (const auto& email : incremental.updatedEmails)
                        updatedEmailIds.push_back(email.id);
                    if (const auto error = reapplyPendingEmailPatches(
                            transaction, m_databaseConnection, accountId,
                            std::move(updatedEmailIds), incremental.emailState))
                        co_return *error;
                }
                if (!incremental.requiresFullFetch)
                {
                    javelin::jmap::cache::MailboxWindowRepository windowRepository{
                        m_databaseConnection};
                    if (const auto error = windowRepository.rebaseContiguousPrefix(
                            cacheTransaction, accountId, mailboxId, canonicalQueryKey,
                            *queryPlan.sinceState, incremental.queryState,
                            incremental.windowAdditions, incremental.removedEmailIds,
                            incremental.representativeCount))
                        co_return javelin::jmap::operationError(*error);
                }
                if (const auto error = transaction.commit())
                    co_return javelin::jmap::operationError(*error);
            }

            if (!incremental.requiresFullFetch)
            {
                representativeCount = incremental.representativeCount;
                usedIncrementalRefresh = true;
                emitProgress(
                    QStringLiteral("Applied mailbox delta without rebuilding the mailbox cache."));
            }
        }

        if (!usedIncrementalRefresh)
        {
            const auto expectedQueryResult = syncStateRepository.find(queryKey);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&expectedQueryResult))
                co_return javelin::jmap::operationError(*error);
            const auto& expectedQueryRecord =
                std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(expectedQueryResult);
            const auto expectedEmailResult = syncStateRepository.find(emailKey);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&expectedEmailResult))
                co_return javelin::jmap::operationError(*error);
            const auto& expectedEmailRecord =
                std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(expectedEmailResult);
            const auto expectedQueryState =
                expectedQueryRecord.transform([](const auto& record) { return record.stateToken; });
            const auto expectedEmailState =
                expectedEmailRecord.transform([](const auto& record) { return record.stateToken; });

            emitProgress(QStringLiteral("Refreshing mailbox window from the server..."));
            const auto fetchResult = co_await fetchCollapsedMailboxThreads(
                m_methodCaller, m_apiRequestContext, remoteAccountId, mailboxId, reportProgress);
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
                co_return supersededMailboxRefresh();
            if (previousRepresentativeIds.has_value())
            {
                const std::unordered_set<std::string> previousIds(
                    previousRepresentativeIds->begin(), previousRepresentativeIds->end());
                std::vector<std::string> insertedRepresentatives;
                for (const auto& representativeId : fetch.representativeIds)
                {
                    if (!previousIds.contains(representativeId))
                        insertedRepresentatives.push_back(representativeId);
                }
                if (!insertedRepresentatives.empty())
                {
                    insertedRepresentatives.insert(insertedRepresentatives.end(),
                                                   insertedEmailIds.begin(),
                                                   insertedEmailIds.end());
                    insertedEmailIds = deduplicatedIds(std::move(insertedRepresentatives));
                    requiresNotificationScan = true;
                }
            }
            const auto affectedMailboxIdsResult =
                changedMailboxIds(m_databaseConnection, accountId, fetch.emails);
            if (const auto* error = std::get_if<OperationError>(&affectedMailboxIdsResult))
                co_return *error;

            auto transactionResult = javelin::jmap::sync::MutationProjectionTransaction::begin(
                m_databaseConnection, QStringLiteral("Materialize mailbox refresh"));
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
                co_return javelin::jmap::operationError(*error);
            auto transaction = std::get<javelin::jmap::sync::MutationProjectionTransaction>(
                std::move(transactionResult));
            auto& cacheTransaction = transaction.cacheTransaction();

            const auto fenceCurrent = consistencyRepository.isCurrent(refreshFence);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&fenceCurrent))
                co_return javelin::jmap::operationError(*error);
            if (!std::get<bool>(fenceCurrent))
                co_return supersededMailboxRefresh();

            const auto expectedQuery = expectedQueryState.has_value()
                                           ? std::optional<std::string_view>{*expectedQueryState}
                                           : std::nullopt;
            const auto queryAdvanced = syncStateRepository.replaceIfCurrent(
                cacheTransaction, queryKey, expectedQuery, fetch.queryState);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&queryAdvanced))
                co_return javelin::jmap::operationError(*error);
            if (!std::get<bool>(queryAdvanced))
                co_return supersededMailboxRefresh();
            if (!fetch.emailState.empty())
            {
                const auto expectedEmail =
                    expectedEmailState.has_value()
                        ? std::optional<std::string_view>{*expectedEmailState}
                        : std::nullopt;
                const auto emailAdvanced = syncStateRepository.replaceIfCurrent(
                    cacheTransaction, emailKey, expectedEmail, fetch.emailState);
                if (const auto* error =
                        std::get_if<javelin::jmap::cache::DatabaseError>(&emailAdvanced))
                    co_return javelin::jmap::operationError(*error);
                if (!std::get<bool>(emailAdvanced))
                    co_return supersededMailboxRefresh();
            }

            if (const auto error =
                    emailRepository.upsertMany(cacheTransaction, accountId, fetch.emails))
                co_return javelin::jmap::operationError(*error);
            if (const auto error = invalidateOtherMailboxWindows(
                    cacheTransaction, m_databaseConnection, accountId, mailboxId,
                    std::get<std::vector<std::string>>(affectedMailboxIdsResult)))
                co_return *error;

            std::vector<std::string> fetchedEmailIds;
            fetchedEmailIds.reserve(fetch.emails.size());
            for (const auto& email : fetch.emails)
                fetchedEmailIds.push_back(email.id);
            if (const auto error =
                    reapplyPendingEmailPatches(transaction, m_databaseConnection, accountId,
                                               std::move(fetchedEmailIds), fetch.emailState))
                co_return *error;
            if (const auto error = canonicalWindows.replace(
                    cacheTransaction,
                    {
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
                co_return javelin::jmap::operationError(*error);
            if (const auto error = transaction.commit())
                co_return javelin::jmap::operationError(*error);

            representativeCount = fetch.representativeCount;
            emitProgress(
                QStringLiteral("Cached %1 threaded conversations for the selected mailbox.")
                    .arg(representativeCount));
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
