#include "app/LongPollService.h"

#include "jmap/api/MethodCaller.h"
#include "jmap/api/Session.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/sync/MailboxRefreshExecutor.h"

#include <QDebug>
#include <QMetaObject>
#include <QNetworkAccessManager>

#include <ranges>

namespace javelin::app
{

    namespace
    {

        [[nodiscard]] LongPollService::Status
        toServiceStatus(const javelin::jmap::sync::LongPollConnectionStatus status)
        {
            switch (status)
            {
            case javelin::jmap::sync::LongPollConnectionStatus::Disconnected:
                return LongPollService::Status::Disconnected;
            case javelin::jmap::sync::LongPollConnectionStatus::Connecting:
                return LongPollService::Status::Connecting;
            case javelin::jmap::sync::LongPollConnectionStatus::Connected:
                return LongPollService::Status::Connected;
            }

            return LongPollService::Status::Disconnected;
        }

    } // namespace

    LongPollService::LongPollService(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                                     javelin::jmap::api::AbstractTransport& transport,
                                     QNetworkAccessManager& networkAccessManager,
                                     javelin::jmap::cache::AccountRepository& accountRepository,
                                     javelin::jmap::cache::QueryService& queryService,
                                     QObject* parent)
        : QObject(parent), m_databaseConnection(databaseConnection), m_transport(transport),
          m_networkAccessManager(networkAccessManager), m_accountRepository(accountRepository),
          m_queryService(queryService)
    {
    }

    LongPollService::~LongPollService()
    {
        stop();
    }

    void LongPollService::applySettings(javelin::jmap::LiveConnectionSettings settings)
    {
        m_settings = std::move(settings);
        restart();
    }

    void LongPollService::stop()
    {
        if (m_runContext != nullptr)
        {
            m_runContext->cancellation.cancel();
            m_runContext.reset();
        }

        setStatus(Status::Disconnected);
    }

    LongPollService::Status LongPollService::status() const
    {
        return m_status;
    }

    QCoro::Task<void> LongPollService::onUpdate(
        const javelin::jmap::sync::LongPollResponse& response)
    {
        m_lastEventId = response.newState;
        co_await refreshWatchedMailbox();
    }

    bool LongPollService::hasValidSettings() const
    {
        return m_settings.has_value() && !m_settings->sessionUrl.empty() &&
               !m_settings->loginEmail.empty() && !m_settings->apiKey.empty();
    }

    std::optional<LongPollService::RunConfiguration> LongPollService::resolveConfiguration() const
    {
        if (!hasValidSettings())
        {
            return std::nullopt;
        }

        const auto accountsResult = m_accountRepository.listAll();
        const auto* accounts =
            std::get_if<std::vector<javelin::jmap::cache::CachedAccount>>(&accountsResult);
        if (accounts == nullptr || accounts->empty())
        {
            return std::nullopt;
        }

        const auto primaryIt = std::ranges::find_if(
            *accounts, [](const auto& account) { return account.isPrimary; });
        const auto& account = primaryIt != accounts->end() ? *primaryIt : accounts->front();

        javelin::jmap::cache::SessionRepository sessionRepository{m_databaseConnection};
        const auto sessionResult = sessionRepository.load(account.accountId);
        const auto* session =
            std::get_if<std::optional<javelin::jmap::api::Session>>(&sessionResult);
        if (session == nullptr || !session->has_value() || !(*session)->eventSourceUrl.has_value())
        {
            qWarning() << "Long poll configuration unavailable because the cached session has no eventSourceUrl";
            return std::nullopt;
        }

        const auto mailboxTreeResult = m_queryService.listMailboxTree(account.accountId);
        const auto* mailboxTree =
            std::get_if<std::vector<javelin::jmap::cache::MailboxTreeItem>>(&mailboxTreeResult);
        if (mailboxTree == nullptr || mailboxTree->empty())
        {
            qWarning() << "Long poll configuration unavailable because the mailbox tree is empty";
            return std::nullopt;
        }

        const auto inboxIt = std::ranges::find_if(
            *mailboxTree,
            [](const auto& mailbox)
            {
                return mailbox.role == std::optional<std::string>{std::string{"inbox"}};
            });
        const auto& mailbox = inboxIt != mailboxTree->end() ? *inboxIt : mailboxTree->front();

        return RunConfiguration{
            .settings = *m_settings,
            .accountId = account.accountId,
            .mailboxId = mailbox.id,
            .mailboxName = mailbox.name,
            .apiUrl = session->value().apiUrl,
            .eventSourceUrl = *session->value().eventSourceUrl,
        };
    }

    QCoro::Task<void> LongPollService::runLoop(std::shared_ptr<RunContext> runContext)
    {
        javelin::jmap::sync::LongPollRequest request{
            .accountId = runContext->configuration.accountId,
            .eventSourceUrl = runContext->configuration.eventSourceUrl,
            .lastState = m_lastEventId,
            .types = {"Email", "Mailbox"},
        };

        co_await runContext->worker->run(request, runContext->cancellation);
    }

    QCoro::Task<void> LongPollService::refreshWatchedMailbox()
    {
        if (m_runContext == nullptr || !hasValidSettings())
        {
            co_return;
        }

        javelin::jmap::api::MethodCaller methodCaller{m_transport};
        const javelin::jmap::api::ApiRequestContext apiRequestContext{
            .credentials =
                {
                    .accountId = m_runContext->configuration.accountId,
                    .emailAddress = m_runContext->configuration.settings.loginEmail,
                    .sessionUrl = m_runContext->configuration.settings.sessionUrl,
                    .token =
                        {
                            .accessToken = m_runContext->configuration.settings.apiKey,
                            .refreshToken = std::nullopt,
                            .expiry = std::nullopt,
                        },
                },
            .apiUrl = m_runContext->configuration.apiUrl,
        };

        javelin::jmap::sync::MailboxRefreshExecutor mailboxRefreshExecutor{
            m_databaseConnection, methodCaller, apiRequestContext};
        const auto refreshResult =
            co_await mailboxRefreshExecutor.refreshCollapsedMailbox(
                m_runContext->configuration.accountId, m_runContext->configuration.mailboxId, {});
        if (const auto* summary =
                std::get_if<javelin::jmap::sync::MailboxRefreshSummary>(&refreshResult))
        {
            Q_EMIT mailboxRefreshed(QString::fromStdString(m_runContext->configuration.accountId),
                                    QString::fromStdString(m_runContext->configuration.mailboxId),
                                    !summary->insertedEmailIds.empty());
            publishNotifications(m_runContext->configuration.mailboxName,
                                 summary->notificationCandidates);
        }
        else if (const auto* error =
                     std::get_if<javelin::jmap::sync::MailboxRefreshError>(&refreshResult))
        {
            qWarning().noquote() << "Long poll mailbox refresh failed" << error->message;
        }
    }

    void LongPollService::restart()
    {
        const auto nextConfiguration = resolveConfiguration();
        if (!nextConfiguration.has_value())
        {
            qWarning() << "Long poll restart aborted because no configuration could be resolved";
            stop();
            return;
        }

        const bool sameStream =
            m_runContext != nullptr &&
            m_runContext->configuration.accountId == nextConfiguration->accountId &&
            m_runContext->configuration.mailboxId == nextConfiguration->mailboxId &&
            m_runContext->configuration.eventSourceUrl == nextConfiguration->eventSourceUrl;
        if (!sameStream)
        {
            m_lastEventId.clear();
        }

        stop();

        auto runContext = std::make_shared<RunContext>();
        runContext->generation = ++m_generation;
        runContext->configuration = *nextConfiguration;
        runContext->channel = std::make_unique<javelin::jmap::sync::EventSourceLongPollChannel>(
            m_networkAccessManager, nextConfiguration->settings.apiKey,
            [this, generation = runContext->generation](
                const javelin::jmap::sync::LongPollConnectionStatus status)
            {
                if (m_runContext == nullptr || m_runContext->generation != generation)
                {
                    return;
                }

                setStatus(toServiceStatus(status));
            });
        runContext->worker = std::make_unique<javelin::jmap::sync::LongPollWorker>(
            *runContext->channel, *this, runContext->sleeper,
            javelin::jmap::sync::BackoffPolicy{},
            [this, generation = runContext->generation](
                const javelin::jmap::sync::LongPollConnectionStatus status)
            {
                if (m_runContext == nullptr || m_runContext->generation != generation)
                {
                    return;
                }

                if (status == javelin::jmap::sync::LongPollConnectionStatus::Connected)
                {
                    return;
                }

                setStatus(toServiceStatus(status));
            });

        m_runContext = runContext;
        setStatus(Status::Connecting);
        auto task = runLoop(runContext);
        QCoro::connect(
            std::move(task), this,
            [this, generation = runContext->generation]()
            {
                if (m_runContext == nullptr || m_runContext->generation != generation)
                {
                    return;
                }

                setStatus(Status::Disconnected);
            });
    }

    void LongPollService::setStatus(const Status status)
    {
        if (m_status == status)
        {
            return;
        }

        m_status = status;
        Q_EMIT statusChanged(m_status);
    }

    void LongPollService::publishNotifications(
        const std::string_view mailboxName,
        const std::vector<javelin::jmap::sync::RefreshNotificationCandidate>& candidates)
    {
        if (candidates.empty())
        {
            return;
        }

        const auto& target = candidates.front();

        QString title;
        QString message;
        if (candidates.size() == 1)
        {
            title = QStringLiteral("New mail in %1").arg(QString::fromStdString(std::string{mailboxName}));
            message = QString::fromStdString(
                candidates.front().subject.value_or(std::string{"(no subject)"}));
        }
        else
        {
            title = QStringLiteral("%1 new messages in %2")
                        .arg(candidates.size())
                        .arg(QString::fromStdString(std::string{mailboxName}));
            message = QString::fromStdString(
                candidates.front().subject.value_or(std::string{"(no subject)"}));
        }

        Q_EMIT notificationRaised(
            QString::fromStdString(m_runContext->configuration.accountId),
            QString::fromStdString(m_runContext->configuration.mailboxId),
            QString::fromStdString(target.threadId), QString::fromStdString(target.emailId),
            QString::fromStdString(std::string{mailboxName}), title, message);
    }

} // namespace javelin::app
