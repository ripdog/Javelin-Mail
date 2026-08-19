#include "app/ThreadMembershipMaterializationWorker.h"

#include "app/AccountConnectionProvider.h"
#include "jmap/OperationError.h"
#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/MailMethods.h"
#include "jmap/api/MethodCaller.h"
#include "jmap/api/RequestBuilder.h"
#include "jmap/api/ResponseReader.h"
#include "jmap/api/Session.h"
#include "jmap/cache/AccountRepository.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/cache/ThreadRepository.h"
#include "jmap/sync/MailboxRefreshExecutor.h"
#include "jmap/sync/MutationJournal.h"

#include <algorithm>
#include <array>
#include <ranges>
#include <unordered_set>
#include <utility>

namespace javelin::app
{
    namespace
    {
        [[nodiscard]] javelin::jmap::OperationError
        error(const javelin::jmap::OperationErrorCode code, QString message)
        {
            return {.code = code, .message = std::move(message)};
        }

        [[nodiscard]] javelin::jmap::OperationError
        callError(const javelin::jmap::api::MethodCallerResult& result)
        {
            if (const auto* value = std::get_if<javelin::jmap::api::TransportError>(&result))
                return javelin::jmap::operationError(*value);
            if (const auto* value = std::get_if<javelin::jmap::api::AuthError>(&result))
                return javelin::jmap::operationError(*value);
            if (const auto* value = std::get_if<javelin::jmap::api::ProtocolError>(&result))
                return javelin::jmap::operationError(*value);
            return error(javelin::jmap::OperationErrorCode::ProtocolViolation,
                         QStringLiteral("The Thread/get request failed."));
        }

        [[nodiscard]] QStringList qStringIds(const std::vector<std::string>& ids)
        {
            QStringList result;
            result.reserve(static_cast<qsizetype>(ids.size()));
            for (const auto& id : ids)
                result.push_back(QString::fromStdString(id));
            return result;
        }

        [[nodiscard]] std::optional<QString>
        validateAccounting(const std::string_view accountId,
                           const std::vector<std::string>& requested,
                           const javelin::jmap::api::ThreadGetResponse& response)
        {
            if (response.accountId != accountId)
                return QStringLiteral("Thread/get returned the wrong account id.");

            const std::unordered_set<std::string> expected(requested.begin(), requested.end());
            std::unordered_set<std::string> accounted;
            accounted.reserve(requested.size());
            const auto account = [&](const std::string& id) -> bool
            { return expected.contains(id) && accounted.insert(id).second; };
            if (!std::ranges::all_of(response.list,
                                     [&](const auto& thread) { return account(thread.id); }) ||
                !std::ranges::all_of(response.notFound, account) ||
                accounted.size() != expected.size())
            {
                return QStringLiteral(
                    "Thread/get list and notFound did not exactly account for the requested ids.");
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<QString>
        validateAccounting(const std::string_view accountId,
                           const std::vector<std::string>& requested,
                           const javelin::jmap::api::EmailGetResponse& response)
        {
            if (response.accountId != accountId)
                return QStringLiteral("Email/get returned the wrong account id.");

            const std::unordered_set<std::string> expected(requested.begin(), requested.end());
            std::unordered_set<std::string> accounted;
            accounted.reserve(requested.size());
            const auto account = [&](const std::string& id) -> bool
            { return expected.contains(id) && accounted.insert(id).second; };
            if (!std::ranges::all_of(response.list,
                                     [&](const auto& email) { return account(email.id); }) ||
                !std::ranges::all_of(response.notFound, account) ||
                accounted.size() != expected.size())
            {
                return QStringLiteral(
                    "Email/get list and notFound did not exactly account for the requested ids.");
            }
            return std::nullopt;
        }

        [[nodiscard]] const std::vector<std::string>& emailSummaryProperties()
        {
            static const std::vector<std::string> properties{
                "id",         "blobId",        "threadId", "mailboxIds", "keywords",
                "size",       "receivedAt",    "sentAt",   "messageId",  "inReplyTo",
                "references", "hasAttachment", "subject",  "from",       "to",
                "cc",         "bcc",           "replyTo",  "preview",
            };
            return properties;
        }
    } // namespace

    ThreadMembershipMaterializationWorker::ThreadMembershipMaterializationWorker(
        javelin::jmap::cache::DatabaseConnection& databaseConnection,
        javelin::jmap::api::JmapMethodTransport& methodTransport,
        const AccountConnectionProvider& connectionProvider, QObject* parent)
        : QObject(parent), m_databaseConnection(databaseConnection),
          m_methodTransport(methodTransport), m_connectionProvider(connectionProvider)
    {
    }

    QCoro::Task<ThreadMaterializationResult>
    ThreadMembershipMaterializationWorker::materialize(ThreadMaterializationTarget target)
    {
        std::unordered_set<std::string> seenThreadIds;
        std::erase_if(target.threadIds, [&seenThreadIds](const std::string& threadId)
                      { return threadId.empty() || !seenThreadIds.insert(threadId).second; });
        if (target.accountId.empty() || target.threadIds.empty())
        {
            co_return ThreadMaterializationSummary{
                .threadIds = {},
                .missingEmailIds = {},
                .completedThreadCount = 0,
                .completedEmailCount = 0,
            };
        }

        const auto settings = m_connectionProvider.connectionSettingsFor(target.accountId);
        if (!settings.has_value())
        {
            co_return error(javelin::jmap::OperationErrorCode::InvalidRequest,
                            QStringLiteral("No connection settings are available for Thread "
                                           "materialization."));
        }

        javelin::jmap::cache::AccountRepository accounts{m_databaseConnection};
        const auto accountResult = accounts.findById(target.accountId);
        if (const auto* databaseError =
                std::get_if<javelin::jmap::cache::DatabaseError>(&accountResult))
            co_return javelin::jmap::operationError(*databaseError);
        const auto& cachedAccount =
            std::get<std::optional<javelin::jmap::cache::CachedAccount>>(accountResult);
        if (!cachedAccount.has_value() || cachedAccount->remoteAccountId.empty())
        {
            co_return error(javelin::jmap::OperationErrorCode::NotFound,
                            QStringLiteral("The mail account has no remote JMAP identity."));
        }
        const std::string remoteAccountId = cachedAccount->remoteAccountId;

        javelin::jmap::cache::SessionRepository sessions{m_databaseConnection};
        const auto loaded = sessions.load(target.accountId);
        if (const auto* databaseError = std::get_if<javelin::jmap::cache::DatabaseError>(&loaded))
            co_return javelin::jmap::operationError(*databaseError);
        const auto& session = std::get<std::optional<javelin::jmap::api::Session>>(loaded);
        if (!session.has_value())
        {
            co_return error(javelin::jmap::OperationErrorCode::UnsupportedCapability,
                            QStringLiteral("No cached JMAP session is available for Thread "
                                           "materialization."));
        }
        const auto capability =
            javelin::jmap::api::validateSessionCapabilities(*session, {.mail = true});
        const auto limits = javelin::jmap::api::coreRequestLimits(*session);
        if (!capability.ok() || !limits.has_value())
        {
            co_return error(javelin::jmap::OperationErrorCode::UnsupportedCapability,
                            QStringLiteral("The cached JMAP session cannot materialize Threads."));
        }

        const auto requestContext = javelin::jmap::api::ApiRequestContext{
            .credentials =
                {
                    .accountId = target.accountId,
                    .emailAddress = settings->loginEmail,
                    .sessionUrl = settings->sessionUrl,
                    .token = {.accessToken = settings->apiKey,
                              .refreshToken = std::nullopt,
                              .expiry = std::nullopt},
                },
            .apiUrl = session->apiUrl,
            .requestLimits = limits,
        };
        const auto batchSize = static_cast<std::size_t>(limits->maxObjectsInGet);
        javelin::jmap::api::MethodCaller caller{m_methodTransport};
        javelin::jmap::cache::ThreadRepository threads{m_databaseConnection};
        javelin::jmap::cache::EmailRepository emails{m_databaseConnection};
        ThreadMaterializationSummary summary;
        summary.threadIds.reserve(target.threadIds.size());

        const auto fetchThreads = [&caller, &requestContext,
                                   &remoteAccountId](std::vector<std::string> ids)
            -> QCoro::Task<
                std::variant<javelin::jmap::api::ThreadGetResponse, javelin::jmap::OperationError>>
        {
            const auto request = javelin::jmap::api::threadGet({
                .accountId = remoteAccountId,
                .ids = std::move(ids),
                .idsReference = std::nullopt,
                .properties = std::nullopt,
            });
            if (!request.has_value())
            {
                co_return error(javelin::jmap::OperationErrorCode::InvalidRequest,
                                QStringLiteral("Unable to serialize the Thread/get request."));
            }
            javelin::jmap::api::RequestBuilder builder;
            builder.useCore().useMail();
            const auto handle = builder.call(*request, "thread-membership");
            const auto called = co_await caller.call(requestContext, builder);
            const auto* envelope = std::get_if<javelin::jmap::api::ResponseEnvelope>(&called);
            if (envelope == nullptr)
                co_return callError(called);
            const auto read = javelin::jmap::api::ResponseReader{*envelope}.require(handle);
            if (const auto* readError = std::get_if<javelin::jmap::api::ResponseReaderError>(&read))
                co_return javelin::jmap::operationError(*readError);
            co_return std::get<javelin::jmap::api::ThreadGetResponse>(read);
        };
        const auto threadBatchFitsRequestLimit = [&remoteAccountId,
                                                  &limits](const std::vector<std::string>& ids)
            -> std::variant<bool, javelin::jmap::OperationError>
        {
            const auto request = javelin::jmap::api::threadGet({
                .accountId = remoteAccountId,
                .ids = ids,
                .idsReference = std::nullopt,
                .properties = std::nullopt,
            });
            if (!request.has_value())
            {
                return error(javelin::jmap::OperationErrorCode::InvalidRequest,
                             QStringLiteral("Unable to serialize the Thread/get request."));
            }
            javelin::jmap::api::RequestBuilder builder;
            builder.useCore().useMail();
            static_cast<void>(builder.call(*request, "thread-membership"));
            const auto encoded = javelin::jmap::api::serializeRequestEnvelope(builder.build());
            if (!encoded.has_value())
            {
                return error(
                    javelin::jmap::OperationErrorCode::InvalidRequest,
                    QStringLiteral("Unable to serialize the Thread/get request envelope."));
            }
            return encoded->size() <= limits->maxSizeRequest;
        };
        const auto commitThreads = [this, &threads,
                                    &target](const javelin::jmap::api::ThreadGetResponse& response)
            -> std::optional<javelin::jmap::OperationError>
        {
            auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
                m_databaseConnection, QStringLiteral("Begin Thread materialization commit"));
            if (const auto* databaseError =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
                return javelin::jmap::operationError(*databaseError);
            auto transaction =
                std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(transactionResult));
            if (const auto databaseError = threads.upsertMany(transaction, target.accountId,
                                                              response.list, response.state))
                return javelin::jmap::operationError(*databaseError);
            if (const auto databaseError =
                    threads.markStale(transaction, target.accountId, response.notFound))
                return javelin::jmap::operationError(*databaseError);
            if (const auto databaseError = transaction.commit())
                return javelin::jmap::operationError(*databaseError);
            return std::nullopt;
        };

        std::vector<std::string> membershipTargets;
        membershipTargets.reserve(target.threadIds.size());
        for (const auto& threadId : target.threadIds)
        {
            const auto membership = threads.findMembership(target.accountId, threadId);
            if (const auto* databaseError =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&membership))
                co_return javelin::jmap::operationError(*databaseError);
            const auto& record =
                std::get<std::optional<javelin::jmap::cache::ThreadMembershipRecord>>(membership);
            const auto coverage = threads.coverage(target.accountId, threadId);
            if (const auto* databaseError =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&coverage))
                co_return javelin::jmap::operationError(*databaseError);
            const auto& currentCoverage =
                std::get<std::optional<javelin::jmap::cache::ThreadCoverage>>(coverage);
            if (!record.has_value() ||
                record->freshness != javelin::jmap::cache::ThreadMembershipFreshness::Current ||
                record->thread.emailIds.size() != record->globalMemberCount ||
                !currentCoverage.has_value() || currentCoverage->untrackedCachedEmailCount != 0)
                membershipTargets.push_back(threadId);
        }

        for (std::size_t offset = 0; offset < membershipTargets.size();)
        {
            const auto count = std::min(batchSize, membershipTargets.size() - offset);
            std::vector<std::string> batch{
                membershipTargets.begin() + static_cast<std::ptrdiff_t>(offset),
                membershipTargets.begin() + static_cast<std::ptrdiff_t>(offset + count)};
            while (true)
            {
                const auto fits = threadBatchFitsRequestLimit(batch);
                if (const auto* fitError = std::get_if<javelin::jmap::OperationError>(&fits))
                    co_return *fitError;
                if (std::get<bool>(fits))
                    break;
                if (batch.size() == 1)
                {
                    co_return error(
                        javelin::jmap::OperationErrorCode::InvalidRequest,
                        QStringLiteral("One Thread id exceeds the negotiated JMAP request size "
                                       "limit."));
                }
                batch.pop_back();
            }
            offset += batch.size();
            auto fetched = co_await fetchThreads(batch);
            if (const auto* fetchError = std::get_if<javelin::jmap::OperationError>(&fetched))
                co_return *fetchError;
            auto response = std::get<javelin::jmap::api::ThreadGetResponse>(std::move(fetched));
            if (const auto accountingError = validateAccounting(remoteAccountId, batch, response))
                co_return error(javelin::jmap::OperationErrorCode::ProtocolViolation,
                                *accountingError);
            if (const auto commitError = commitThreads(response))
                co_return *commitError;

            std::vector<std::string> committedIds;
            committedIds.reserve(response.list.size() + response.notFound.size());
            for (const auto& thread : response.list)
            {
                committedIds.push_back(thread.id);
                summary.threadIds.push_back(thread.id);
            }
            committedIds.insert(committedIds.end(), response.notFound.begin(),
                                response.notFound.end());
            summary.completedThreadCount += response.list.size() + response.notFound.size();
            if (!committedIds.empty())
                Q_EMIT membershipCommitted(QString::fromStdString(target.accountId),
                                           qStringIds(committedIds));
            Q_EMIT progressChanged(QString::fromStdString(target.accountId),
                                   static_cast<quint64>(summary.completedThreadCount),
                                   static_cast<quint64>(target.threadIds.size()));

            if (!response.notFound.empty())
            {
                co_return error(
                    javelin::jmap::OperationErrorCode::NotFound,
                    QStringLiteral("Thread/get could not reconcile %1 represented Thread(s).")
                        .arg(response.notFound.size()));
            }
        }

        constexpr std::size_t maximumMembershipReconciliations = 2;
        for (const auto& threadId : target.threadIds)
        {
            std::size_t reconciliationCount = 0;
            while (true)
            {
                auto missingResult = threads.missingEmailIds(target.accountId, threadId, batchSize);
                if (const auto* databaseError =
                        std::get_if<javelin::jmap::cache::DatabaseError>(&missingResult))
                    co_return javelin::jmap::operationError(*databaseError);
                auto missing = std::get<std::vector<std::string>>(std::move(missingResult));
                if (missing.empty())
                    break;

                javelin::jmap::api::RequestBuilder builder;
                javelin::jmap::api::CallHandle<javelin::jmap::api::EmailGetResponse> handle;
                while (true)
                {
                    const auto request = javelin::jmap::api::emailGet({
                        .accountId = remoteAccountId,
                        .ids = missing,
                        .idsReference = std::nullopt,
                        .properties = emailSummaryProperties(),
                    });
                    if (!request.has_value())
                        co_return error(
                            javelin::jmap::OperationErrorCode::InvalidRequest,
                            QStringLiteral("Unable to serialize the Email/get request."));
                    builder = {};
                    builder.useCore().useMail();
                    handle = builder.call(*request, "thread-child-emails");
                    const auto encoded =
                        javelin::jmap::api::serializeRequestEnvelope(builder.build());
                    if (encoded.has_value() && encoded->size() <= limits->maxSizeRequest)
                        break;
                    if (missing.size() == 1)
                    {
                        co_return error(
                            javelin::jmap::OperationErrorCode::InvalidRequest,
                            QStringLiteral("One child Email id exceeds the negotiated JMAP request "
                                           "size limit."));
                    }
                    missing.pop_back();
                }

                const auto called = co_await caller.call(requestContext, builder);
                const auto* envelope = std::get_if<javelin::jmap::api::ResponseEnvelope>(&called);
                if (envelope == nullptr)
                    co_return callError(called);
                const auto read = javelin::jmap::api::ResponseReader{*envelope}.require(handle);
                if (const auto* readError =
                        std::get_if<javelin::jmap::api::ResponseReaderError>(&read))
                    co_return javelin::jmap::operationError(*readError);
                auto response = std::get<javelin::jmap::api::EmailGetResponse>(read);
                if (const auto accountingError =
                        validateAccounting(remoteAccountId, missing, response))
                    co_return error(javelin::jmap::OperationErrorCode::ProtocolViolation,
                                    *accountingError);

                const bool membershipRace =
                    !response.notFound.empty() ||
                    std::ranges::any_of(response.list, [&threadId](const auto& email)
                                        { return email.threadId != threadId; });
                auto transactionResult = javelin::jmap::sync::MutationProjectionTransaction::begin(
                    m_databaseConnection,
                    QStringLiteral("Commit Thread child Email materialization"));
                if (const auto* databaseError =
                        std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
                    co_return javelin::jmap::operationError(*databaseError);
                auto transaction = std::get<javelin::jmap::sync::MutationProjectionTransaction>(
                    std::move(transactionResult));
                if (const auto databaseError = emails.upsertMany(transaction.cacheTransaction(),
                                                                 target.accountId, response.list))
                    co_return javelin::jmap::operationError(*databaseError);
                std::vector<std::string> committedEmailIds;
                committedEmailIds.reserve(response.list.size());
                std::vector<std::string> affectedThreadIds{threadId};
                for (const auto& email : response.list)
                {
                    committedEmailIds.push_back(email.id);
                    if (std::ranges::find(affectedThreadIds, email.threadId) ==
                        affectedThreadIds.end())
                        affectedThreadIds.push_back(email.threadId);
                }
                if (const auto rebaseError = javelin::jmap::sync::rebaseActiveEmailProjections(
                        transaction, m_databaseConnection, target.accountId, committedEmailIds,
                        response.state))
                    co_return *rebaseError;
                if (membershipRace)
                {
                    const std::array staleThreadIds{threadId};
                    if (const auto databaseError = threads.markStale(
                            transaction.cacheTransaction(), target.accountId, staleThreadIds))
                        co_return javelin::jmap::operationError(*databaseError);
                }
                if (const auto databaseError = transaction.commit())
                    co_return javelin::jmap::operationError(*databaseError);

                summary.completedEmailCount += response.list.size();
                if (!committedEmailIds.empty())
                    Q_EMIT childEmailsCommitted(QString::fromStdString(target.accountId),
                                                qStringIds(affectedThreadIds),
                                                qStringIds(committedEmailIds));
                if (!membershipRace)
                    continue;
                if (reconciliationCount >= maximumMembershipReconciliations)
                {
                    co_return error(
                        javelin::jmap::OperationErrorCode::Conflict,
                        QStringLiteral("Thread membership kept changing while child Emails were "
                                       "materialized."));
                }
                ++reconciliationCount;
                const std::vector<std::string> reconciliationBatch{threadId};
                auto fetched = co_await fetchThreads(reconciliationBatch);
                if (const auto* fetchError = std::get_if<javelin::jmap::OperationError>(&fetched))
                    co_return *fetchError;
                auto threadResponse =
                    std::get<javelin::jmap::api::ThreadGetResponse>(std::move(fetched));
                if (const auto accountingError =
                        validateAccounting(remoteAccountId, reconciliationBatch, threadResponse))
                    co_return error(javelin::jmap::OperationErrorCode::ProtocolViolation,
                                    *accountingError);
                if (const auto commitError = commitThreads(threadResponse))
                    co_return *commitError;
                if (!threadResponse.list.empty())
                    Q_EMIT membershipCommitted(QString::fromStdString(target.accountId),
                                               {QString::fromStdString(threadId)});
                if (!threadResponse.notFound.empty())
                {
                    co_return error(javelin::jmap::OperationErrorCode::NotFound,
                                    QStringLiteral("The represented Thread disappeared during "
                                                   "child Email reconciliation."));
                }
            }
        }

        summary.threadIds = target.threadIds;
        summary.completedThreadCount = target.threadIds.size();
        summary.missingEmailIds.clear();
        co_return summary;
    }
} // namespace javelin::app
