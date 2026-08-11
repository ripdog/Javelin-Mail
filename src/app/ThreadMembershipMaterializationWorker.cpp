#include "app/ThreadMembershipMaterializationWorker.h"

#include "app/AccountConnectionProvider.h"
#include "jmap/OperationError.h"
#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/MailMethods.h"
#include "jmap/api/MethodCaller.h"
#include "jmap/api/RequestBuilder.h"
#include "jmap/api/ResponseReader.h"
#include "jmap/api/Session.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/cache/ThreadRepository.h"

#include <algorithm>
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
        std::ranges::sort(target.threadIds);
        target.threadIds.erase(std::unique(target.threadIds.begin(), target.threadIds.end()),
                               target.threadIds.end());
        if (target.accountId.empty() || target.threadIds.empty())
        {
            co_return ThreadMaterializationSummary{
                .threadIds = {},
                .missingEmailIds = {},
                .completedThreadCount = 0,
            };
        }

        const auto settings = m_connectionProvider.connectionSettingsFor(target.accountId);
        if (!settings.has_value())
        {
            co_return error(javelin::jmap::OperationErrorCode::InvalidRequest,
                            QStringLiteral("No connection settings are available for Thread "
                                           "materialization."));
        }

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
        ThreadMaterializationSummary summary;
        summary.threadIds.reserve(target.threadIds.size());

        for (std::size_t offset = 0; offset < target.threadIds.size(); offset += batchSize)
        {
            const auto count = std::min(batchSize, target.threadIds.size() - offset);
            std::vector<std::string> batch{
                target.threadIds.begin() + static_cast<std::ptrdiff_t>(offset),
                target.threadIds.begin() + static_cast<std::ptrdiff_t>(offset + count)};
            const auto request = javelin::jmap::api::threadGet({
                .accountId = target.accountId,
                .ids = batch,
                .idsReference = std::nullopt,
                .properties = std::nullopt,
            });
            if (!request.has_value())
                co_return error(javelin::jmap::OperationErrorCode::InvalidRequest,
                                QStringLiteral("Unable to serialize the Thread/get request."));

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
            auto response = std::get<javelin::jmap::api::ThreadGetResponse>(read);
            if (const auto accountingError = validateAccounting(target.accountId, batch, response))
                co_return error(javelin::jmap::OperationErrorCode::ProtocolViolation,
                                *accountingError);

            auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
                m_databaseConnection, QStringLiteral("Begin Thread materialization commit"));
            if (const auto* databaseError =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
                co_return javelin::jmap::operationError(*databaseError);
            auto transaction =
                std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(transactionResult));
            if (const auto databaseError = threads.upsertMany(transaction, target.accountId,
                                                              response.list, response.state))
                co_return javelin::jmap::operationError(*databaseError);
            if (const auto databaseError =
                    threads.markStale(transaction, target.accountId, response.notFound))
                co_return javelin::jmap::operationError(*databaseError);
            if (const auto databaseError = transaction.commit())
                co_return javelin::jmap::operationError(*databaseError);

            std::vector<std::string> committedIds;
            committedIds.reserve(response.list.size());
            for (const auto& thread : response.list)
            {
                committedIds.push_back(thread.id);
                summary.threadIds.push_back(thread.id);
                const auto missing = threads.missingEmailIds(target.accountId, thread.id);
                if (const auto* databaseError =
                        std::get_if<javelin::jmap::cache::DatabaseError>(&missing))
                    co_return javelin::jmap::operationError(*databaseError);
                const auto& ids = std::get<std::vector<std::string>>(missing);
                summary.missingEmailIds.insert(summary.missingEmailIds.end(), ids.begin(),
                                               ids.end());
            }
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

        std::ranges::sort(summary.missingEmailIds);
        summary.missingEmailIds.erase(
            std::unique(summary.missingEmailIds.begin(), summary.missingEmailIds.end()),
            summary.missingEmailIds.end());
        co_return summary;
    }
} // namespace javelin::app
