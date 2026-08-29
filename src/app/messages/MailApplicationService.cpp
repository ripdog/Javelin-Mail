#include "app/AccountRuntimeManager.h"
#include "app/CalendarApplicationService.h"
#include "app/ContactApplicationService.h"
#include "app/MailMutationApplicationService.h"
#include "app/MailNotificationService.h"
#include "app/MailQueryApplicationService.h"
#include "app/MessageContentApplicationService.h"
#include "app/SieveApplicationService.h"

#include "app/ApplicationErrorCoordinator.h"
#include "app/EmailMutationBatchSubmitter.h"
#include "app/MailSaveNaming.h"
#include "app/MailboxMaintenanceRegistry.h"
#include "app/MessageSubject.h"
#include "app/RawMailMaterializer.h"
#include "app/StateChangePolicy.h"
#include "app/ThreadMaterializationCoordinator.h"
#include "app/WorkScheduler.h"
#include "app/undo/HistoryTypes.h"
#include "app/undo/UndoManager.h"

#include "jmap/AccountBootstrapClient.h"
#include "jmap/MessageContentClient.h"
#include "jmap/OperationError.h"
#include "jmap/api/CalendarMethods.h"
#include "jmap/api/SessionRefreshClient.h"
#include "jmap/cache/CalendarRepository.h"
#include "jmap/cache/ContactRepository.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailTagReader.h"
#include "jmap/cache/MailboxFilterReader.h"
#include "jmap/cache/MailboxMessageReader.h"
#include "jmap/cache/MailboxReadRepository.h"
#include "jmap/cache/MailboxRepository.h"
#include "jmap/cache/MailboxStatisticsReader.h"
#include "jmap/cache/MailboxWindowRepository.h"
#include "jmap/cache/NotificationRepository.h"
#include "jmap/cache/QueryWindowReadRepository.h"
#include "jmap/cache/SearchWindowRepository.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/cache/SyncStateRepository.h"
#include "jmap/cache/ThreadReadRepository.h"
#include "jmap/calendar/CalendarMutationEngine.h"
#include "jmap/calendar/CalendarProtocolClient.h"
#include "jmap/calendar/CalendarSyncEngine.h"
#include "jmap/contacts/ContactSyncEngine.h"
#include "jmap/domain/MailKeywords.h"
#include "jmap/query/MailQueryClient.h"
#include "jmap/query/MailQueryMaterializer.h"
#include "jmap/sieve/SieveMutationEngine.h"
#include "jmap/sieve/SieveProtocolClient.h"
#include "jmap/sync/EmailMutationEngine.h"
#include "jmap/sync/EmailMutationJournal.h"
#include "jmap/sync/MailboxMutationEngine.h"
#include "jmap/sync/MailboxQueryDescriptor.h"
#include "jmap/sync/MutationJournal.h"

#include <QCoroFuture>
#include <QCoroTask>

#include <KLocalizedString>

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimer>
#include <QUuid>
#include <QtConcurrentRun>

#include <algorithm>
#include <chrono>
#include <limits>
#include <map>
#include <ranges>
#include <unordered_set>
#include <utility>

namespace javelin::app
{
    namespace
    {
        constexpr std::size_t pendingEmailMutationBatchSize = 25;

        [[nodiscard]] QString accountSynchronizationNotConfigured()
        {
            return i18n("Account synchronization is not configured.");
        }

        struct SavedMessageFileResult
        {
            QString path;
            QString error;
        };

        [[nodiscard]] SavedMessageFileResult copySavedMessageFile(const QString& sourcePath,
                                                                  const QString& targetPath)
        {
            QFile source{sourcePath};
            if (!source.open(QIODevice::ReadOnly))
                return {.path = targetPath, .error = source.errorString()};

            QSaveFile target{targetPath};
            if (!target.open(QIODevice::WriteOnly))
                return {.path = targetPath, .error = target.errorString()};

            QByteArray buffer;
            buffer.resize(1024 * 1024);
            while (true)
            {
                const auto read = source.read(buffer.data(), buffer.size());
                if (read < 0)
                    return {.path = targetPath, .error = source.errorString()};
                if (read == 0)
                    break;
                if (target.write(buffer.constData(), read) != read)
                    return {.path = targetPath, .error = target.errorString()};
            }
            if (!target.commit())
                return {.path = targetPath, .error = target.errorString()};
            return {.path = targetPath, .error = {}};
        }

        [[nodiscard]] SavedMessageFileResult
        copySavedMessageFileExclusively(const QString& sourcePath, const QString& directory,
                                        const QString& fileName)
        {
            QFile source{sourcePath};
            if (!source.open(QIODevice::ReadOnly))
                return {.path = directory, .error = source.errorString()};

            const QDir targetDirectory{directory};
            constexpr quint64 maximumCollisionAttempts = 10000;
            for (quint64 attempt = 1; attempt <= maximumCollisionAttempts; ++attempt)
            {
                const QString candidateName = attempt == 1
                                                  ? truncateGeneratedFileName(fileName)
                                                  : collisionMailSaveFileName(fileName, attempt);
                const QString candidatePath = targetDirectory.filePath(candidateName);
                QFile target{candidatePath};
                if (!target.open(QIODevice::WriteOnly | QIODevice::NewOnly))
                {
                    if (QFileInfo::exists(candidatePath))
                        continue;
                    return {.path = candidatePath, .error = target.errorString()};
                }

                QByteArray buffer;
                buffer.resize(1024 * 1024);
                bool failed = false;
                QString error;
                source.seek(0);
                while (true)
                {
                    const auto read = source.read(buffer.data(), buffer.size());
                    if (read < 0)
                    {
                        failed = true;
                        error = source.errorString();
                        break;
                    }
                    if (read == 0)
                        break;
                    if (target.write(buffer.constData(), read) != read)
                    {
                        failed = true;
                        error = target.errorString();
                        break;
                    }
                }
                if (!failed && !target.flush())
                {
                    failed = true;
                    error = target.errorString();
                }
                target.close();
                if (failed)
                {
                    QFile::remove(candidatePath);
                    return {.path = candidatePath, .error = std::move(error)};
                }
                return {.path = candidatePath, .error = {}};
            }

            return {.path = directory,
                    .error = QStringLiteral("Unable to allocate a unique destination filename")};
        }

        [[nodiscard]] std::variant<bool, javelin::jmap::cache::DatabaseError>
        emailMaintenanceActive(javelin::jmap::cache::DatabaseConnection& connection,
                               const MailboxMaintenanceRegistry& registry,
                               const std::string_view accountId, const std::string_view emailId)
        {
            javelin::jmap::cache::EmailRepository emails{connection};
            const auto found = emails.find(accountId, emailId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&found))
                return *error;
            const auto& email = std::get<std::optional<javelin::jmap::domain::Email>>(found);
            if (!email.has_value())
                return false;
            QStringList mailboxIds;
            mailboxIds.reserve(static_cast<qsizetype>(email->mailboxIds.size()));
            for (const auto& mailboxId : email->mailboxIds)
                mailboxIds.push_back(QString::fromStdString(mailboxId));
            return registry.isActiveForEmail(QString::fromStdString(std::string{accountId}),
                                             mailboxIds);
        }

        [[nodiscard]] QStringList affectedMailboxIdsForPendingMutations(
            javelin::jmap::cache::DatabaseConnection& connection, const std::string_view accountId,
            const std::optional<std::string>& operationGroupId, const std::size_t limit)
        {
            javelin::jmap::sync::EmailMutationJournal journal{connection};
            auto recordsResult =
                operationGroupId.has_value()
                    ? journal.listPendingForOperationGroup(accountId, *operationGroupId, limit)
                    : journal.listByStatus(accountId, javelin::jmap::sync::MutationStatus::Pending,
                                           limit);
            const auto* records =
                std::get_if<std::vector<javelin::jmap::sync::EmailMutationRecord>>(&recordsResult);
            if (records == nullptr)
                return {};

            QStringList mailboxIds;
            const auto append = [&mailboxIds](const std::vector<std::string>& values)
            {
                for (const auto& value : values)
                {
                    const auto mailboxId = QString::fromStdString(value);
                    if (!mailboxIds.contains(mailboxId))
                        mailboxIds.push_back(mailboxId);
                }
            };
            for (const auto& record : *records)
            {
                append(record.patch.addMailboxIds);
                append(record.patch.removeMailboxIds);
                if (record.baseMailboxIds.has_value())
                    append(*record.baseMailboxIds);
            }
            return mailboxIds;
        }

        class ForegroundWorkScope final
        {
          public:
            explicit ForegroundWorkScope(WorkScheduler& scheduler) : m_scheduler(scheduler)
            {
                m_scheduler.beginForegroundWork();
            }
            ~ForegroundWorkScope()
            {
                m_scheduler.endForegroundWork();
            }

          private:
            WorkScheduler& m_scheduler;
        };
    } // namespace

    namespace
    {
        [[nodiscard]] javelin::jmap::LiveConnectionSettings
        toLiveConnectionSettings(const AccountConnectionSettings& settings)
        {
            return javelin::jmap::LiveConnectionSettings{
                .sessionUrl = settings.sessionUrl,
                .loginEmail = settings.loginEmail,
                .apiKey = settings.apiKey,
            };
        }

        constexpr auto contactRefreshRetryDelay = std::chrono::seconds{30};
        constexpr unsigned int notificationBaselineRetryMaximumExponent = 5;
        constexpr unsigned int mailNotificationLocalRetryMaximumExponent = 5;

        [[nodiscard]] javelin::app::undo::ExactMailPatch
        historyPatch(const javelin::jmap::EmailMailboxMutation& mutation)
        {
            return {
                .addMailboxIds = mutation.addMailboxIds,
                .removeMailboxIds = mutation.removeMailboxIds,
                .addKeywords = mutation.addKeywords,
                .removeKeywords = mutation.removeKeywords,
            };
        }

        [[nodiscard]] javelin::app::undo::ExactMailPatch
        inverseHistoryPatch(const javelin::jmap::EmailMailboxMutation& mutation)
        {
            return {
                .addMailboxIds = mutation.removeMailboxIds,
                .removeMailboxIds = mutation.addMailboxIds,
                .addKeywords = mutation.removeKeywords,
                .removeKeywords = mutation.addKeywords,
            };
        }

        [[nodiscard]] javelin::app::undo::MailPatchItemHistory
        historyItem(const std::string_view accountId, const javelin::jmap::domain::Email& email,
                    const javelin::jmap::EmailMailboxMutation& mutation)
        {
            return {
                .accountId = std::string{accountId},
                .emailId = email.id,
                .subject = email.subject,
                .forward = historyPatch(mutation),
                .inverse = inverseHistoryPatch(mutation),
                .expectedBefore = historyPatch(mutation),
                .expectedAfter = inverseHistoryPatch(mutation),
                .mutationId = std::nullopt,
            };
        }

        [[nodiscard]] QString messageCountLabel(const QString& verb, const std::size_t count)
        {
            return count == 1 ? verb + QStringLiteral(" Message")
                              : verb + QStringLiteral(" %1 Messages").arg(count);
        }

        [[nodiscard]] std::string contactRefreshJobId(const std::string_view ownerAccountId)
        {
            return "contact-refresh:" + std::string{ownerAccountId};
        }

        [[nodiscard]] std::string tagDeletionJobId(const std::string_view accountId,
                                                   const std::string_view keyword)
        {
            QByteArray identity{accountId.data(), static_cast<qsizetype>(accountId.size())};
            identity.push_back('\0');
            identity.append(keyword.data(), static_cast<qsizetype>(keyword.size()));
            const auto digest =
                QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex().left(20);
            return "tag-delete:" + digest.toStdString();
        }

        [[nodiscard]] QString tagDeletionCheckpoint(const std::string_view keyword)
        {
            QJsonObject object;
            object.insert(QStringLiteral("keyword"), QString::fromStdString(std::string{keyword}));
            return QString::fromUtf8(QJsonDocument{object}.toJson(QJsonDocument::Compact));
        }

        [[nodiscard]] std::optional<std::string> tagDeletionKeyword(const QStringView checkpoint)
        {
            const auto document = QJsonDocument::fromJson(checkpoint.toUtf8());
            if (!document.isObject())
                return std::nullopt;
            const auto value = document.object().value(QStringLiteral("keyword"));
            if (!value.isString() || value.toString().isEmpty())
                return std::nullopt;
            return value.toString().toStdString();
        }

        [[nodiscard]] std::string generatedTagKeyword(const QStringView displayName)
        {
            QString result;
            bool separatorPending = false;
            for (const auto character : displayName.toString().toLower())
            {
                const auto value = character.unicode();
                if ((value >= 'a' && value <= 'z') || (value >= '0' && value <= '9'))
                {
                    if (separatorPending && !result.isEmpty())
                        result.push_back(QLatin1Char('-'));
                    result.push_back(character);
                    separatorPending = false;
                }
                else
                {
                    separatorPending = !result.isEmpty();
                }
            }
            if (result.isEmpty())
                result = QStringLiteral("tag");
            return result.left(240).toStdString();
        }

        [[nodiscard]] std::optional<QString>
        keywordRightsError(const javelin::jmap::domain::Email& email,
                           const std::vector<javelin::jmap::cache::MailboxTreeItem>& mailboxes)
        {
            for (const auto& mailboxId : email.mailboxIds)
            {
                const auto mailbox = std::ranges::find(mailboxes, mailboxId,
                                                       &javelin::jmap::cache::MailboxTreeItem::id);
                if (mailbox == mailboxes.end())
                {
                    return i18n("A mailbox containing this message is not cached locally.");
                }
                if (!mailbox->myRights.maySetKeywords)
                {
                    return i18n("The server does not allow changing tags in %1.",
                                QString::fromStdString(mailbox->name));
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] std::string searchWindowLeaseKey(const std::string_view accountId,
                                                       const std::string_view queryKey)
        {
            std::string key;
            key.reserve(accountId.size() + queryKey.size() + 1);
            key.append(accountId);
            key.push_back('\0');
            key.append(queryKey);
            return key;
        }

        template <typename Result>
        [[nodiscard]] Result observeResult(ApplicationErrorCoordinator& coordinator,
                                           const AccountConnectionSettings& settings,
                                           const std::string_view accountId, QString operation,
                                           Result result)
        {
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                coordinator.reportFailure(settings, accountId, std::move(operation), *error);
            else
                coordinator.reportSuccess(settings.connectionId);
            return result;
        }

        [[nodiscard]] std::optional<javelin::jmap::OperationError> settleReconciledHistory(
            javelin::app::undo::UndoManager& manager,
            const std::vector<javelin::jmap::sync::ReconciledMutation>& reconciled)
        {
            std::unordered_set<std::string> operationGroups;
            for (const auto& mutation : reconciled)
            {
                if (mutation.operationGroupId.has_value())
                    operationGroups.insert(*mutation.operationGroupId);
            }
            for (const auto& operationGroupId : operationGroups)
            {
                if (const auto error =
                        manager.settleAmbiguousOperation(QString::fromStdString(operationGroupId)))
                    return javelin::jmap::operationError(*error);
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<javelin::jmap::OperationError>
        retainUnknownOrDiscard(javelin::app::undo::UndoManager& manager,
                               std::optional<javelin::app::undo::HistoryEntry>& prepared,
                               const javelin::jmap::OperationError& operationError)
        {
            if (!prepared.has_value())
                return std::nullopt;
            if (!javelin::jmap::isTransientError(operationError) ||
                javelin::jmap::isAuthenticationError(operationError))
            {
                if (const auto error = manager.discardNormal(prepared->entryId))
                    return javelin::jmap::operationError(*error);
                return std::nullopt;
            }
            auto committed =
                manager.commitNormalBlockedUnknown(std::move(*prepared), operationError.message);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
                return javelin::jmap::operationError(*error);
            return std::nullopt;
        }
    } // namespace

    MailboxObservation::MailboxObservation(
        MailQueryApplicationService& service,
        const javelin::jmap::sync::MailboxInterestRegistry::ObservationId observationId)
        : m_service(&service), m_observationId(observationId)
    {
    }

    MailboxObservation::~MailboxObservation()
    {
        reset();
    }

    MailboxObservation::MailboxObservation(MailboxObservation&& other) noexcept
        : m_service(std::exchange(other.m_service, nullptr)),
          m_observationId(std::exchange(other.m_observationId, 0))
    {
    }

    MailboxObservation& MailboxObservation::operator=(MailboxObservation&& other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }

        reset();
        m_service = std::exchange(other.m_service, nullptr);
        m_observationId = std::exchange(other.m_observationId, 0);
        return *this;
    }

    void MailboxObservation::reset()
    {
        if (m_service != nullptr && m_observationId != 0)
        {
            m_service->releaseMailboxObservation(m_observationId);
        }
        m_service.clear();
        m_observationId = 0;
    }

    MailboxObservation::operator bool() const
    {
        return m_service != nullptr && m_observationId != 0;
    }

    AccountRuntimeManager::AccountRuntimeManager(
        javelin::jmap::cache::DatabaseConnection& databaseConnection,
        javelin::jmap::SessionRefreshClient& sessionRefreshClient,
        javelin::jmap::AccountBootstrapClient& accountBootstrapClient,
        javelin::jmap::api::JmapMethodTransport& methodTransport,
        QNetworkAccessManager& networkAccessManager,
        javelin::jmap::api::WebSocketFailureCooldowns& cooldowns,
        javelin::jmap::cache::AccountRepository& accountRepository,
        javelin::jmap::cache::MailboxReader& mailboxReader,
        ApplicationErrorCoordinator& errorCoordinator, WorkScheduler& workScheduler,
        QObject* parent)
        : QObject(parent), m_databaseConnection(databaseConnection),
          m_sessionRefreshClient(sessionRefreshClient),
          m_accountBootstrapClient(accountBootstrapClient), m_methodTransport(methodTransport),
          m_networkAccessManager(networkAccessManager), m_transportCooldowns(cooldowns),
          m_accountRepository(accountRepository), m_mailboxReader(mailboxReader),
          m_errorCoordinator(errorCoordinator), m_workScheduler(workScheduler)
    {
        connect(&m_errorCoordinator, &ApplicationErrorCoordinator::authenticationPauseChanged, this,
                [this](const QString& connectionId, const bool paused)
                {
                    for (const auto& [accountId, configuration] : m_configurations)
                    {
                        if (configuration.settings.connectionId != connectionId.toStdString())
                            continue;
                        if (paused)
                        {
                            const auto coordinator = m_coordinators.find(accountId);
                            if (coordinator != m_coordinators.end())
                                coordinator->second->pauseForAuthentication();
                        }
                        else
                        {
                            applyAccountConfiguration(accountId);
                        }
                    }
                });
    }

    MailQueryApplicationService::MailQueryApplicationService(
        javelin::jmap::cache::DatabaseConnection& databaseConnection,
        javelin::jmap::MailQueryMaterializer& queryMaterializer,
        javelin::jmap::cache::ContactReader& contactReader,
        javelin::jmap::cache::MailTagReader& mailTagReader,
        javelin::jmap::cache::MailboxStatisticsReader& mailboxStatisticsReader,
        javelin::jmap::cache::MailboxMessageReader& mailboxMessageReader,
        javelin::jmap::cache::MailboxFilterReader& mailboxFilterReader,
        AccountRuntimeManager& accountRuntime, ApplicationErrorCoordinator& errorCoordinator,
        WorkScheduler& workScheduler, MailboxMaintenanceRegistry& mailboxMaintenanceRegistry,
        QObject* parent)
        : QObject(parent), m_databaseConnection(databaseConnection),
          m_queryMaterializer(queryMaterializer), m_contactReader(contactReader),
          m_mailTagReader(mailTagReader), m_mailboxStatisticsReader(mailboxStatisticsReader),
          m_mailboxMessageReader(mailboxMessageReader), m_mailboxFilterReader(mailboxFilterReader),
          m_accountRuntime(accountRuntime), m_errorCoordinator(errorCoordinator),
          m_workScheduler(workScheduler), m_mailboxMaintenanceRegistry(mailboxMaintenanceRegistry)
    {
        connect(&m_accountRuntime, &AccountRuntimeManager::configurationSetChanged, this,
                [this]
                {
                    const auto configured = m_accountRuntime.configuredAccountIds();
                    m_mailboxInterests.eraseAccountsNotIn(
                        std::unordered_set<std::string>{configured.begin(), configured.end()});
                });
        connect(
            &m_accountRuntime, &AccountRuntimeManager::cacheCommitted, this,
            [this](const MailCacheChange& change)
            {
                if (m_threadMaterializationCoordinator == nullptr)
                    return;
                for (const auto& window : change.queryWindows)
                {
                    const auto queryKey = javelin::jmap::sync::mailboxQueryKey({
                        .mailboxId = window.mailboxId.toStdString(),
                        .sortProperty = "receivedAt",
                        .isAscending = false,
                        .collapseThreads = true,
                    });
                    if (const auto error = m_threadMaterializationCoordinator->enqueueMailboxWindow(
                            change.accountId.toStdString(), queryKey, window.offset, window.limit))
                        qWarning().noquote()
                            << "Could not enqueue refreshed mailbox Thread materialization"
                            << error->message;
                }
            });
    }

    MailMutationApplicationService::MailMutationApplicationService(
        javelin::jmap::cache::DatabaseConnection& databaseConnection,
        javelin::jmap::EmailMutationEngine& emailMutationEngine,
        javelin::jmap::MailboxMutationEngine& mailboxMutationEngine,
        javelin::jmap::MailQueryClient& queryClient,
        javelin::jmap::cache::MailboxReader& mailboxReader,
        javelin::jmap::cache::MailTagReader& mailTagReader,
        javelin::jmap::cache::MailboxMessageReader& mailboxMessageReader,
        AccountRuntimeManager& accountRuntime, ApplicationErrorCoordinator& errorCoordinator,
        WorkScheduler& workScheduler, MailboxMaintenanceRegistry& mailboxMaintenanceRegistry,
        javelin::app::undo::UndoManager& undoManager, QObject* parent)
        : QObject(parent), m_databaseConnection(databaseConnection),
          m_emailMutationEngine(emailMutationEngine),
          m_mailboxMutationEngine(mailboxMutationEngine), m_queryClient(queryClient),
          m_mailboxReader(mailboxReader), m_mailTagReader(mailTagReader),
          m_mailboxMessageReader(mailboxMessageReader), m_accountRuntime(accountRuntime),
          m_errorCoordinator(errorCoordinator), m_workScheduler(workScheduler),
          m_mailboxMaintenanceRegistry(mailboxMaintenanceRegistry), m_undoManager(undoManager)
    {
        connect(&m_workScheduler, &WorkScheduler::jobsChanged, this,
                [this] { scheduleTagDeletionPump(); });
        connect(&m_workScheduler, &WorkScheduler::foregroundAvailabilityChanged, this,
                [this] { scheduleTagDeletionPump(); });
        connect(&m_accountRuntime, &AccountRuntimeManager::accountConfigured, this,
                [this](const QString& accountId) { accountConfigured(accountId.toStdString()); });
        connect(&m_accountRuntime, &AccountRuntimeManager::sessionRefreshed, this,
                [this](const QString& accountId) { accountConfigured(accountId.toStdString()); });
        connect(&m_accountRuntime, &AccountRuntimeManager::networkReachable, this,
                &MailMutationApplicationService::networkBecameReachable);
        connect(
            &m_errorCoordinator, &ApplicationErrorCoordinator::authenticationPauseChanged, this,
            [this](const QString& connectionId, const bool paused)
            {
                if (paused)
                    return;
                const auto listed = m_workScheduler.list();
                if (const auto* jobs = std::get_if<std::vector<WorkRecord>>(&listed))
                {
                    for (const auto& job : *jobs)
                    {
                        if (job.kind != WorkKind::TagDeletion ||
                            job.status != WorkStatus::WaitingForAuth || !job.accountId.has_value())
                            continue;
                        const auto settings =
                            m_accountRuntime.connectionSettingsFor(*job.accountId);
                        if (settings.has_value() &&
                            settings->connectionId == connectionId.toStdString())
                        {
                            static_cast<void>(m_workScheduler.update(
                                job.jobId, WorkStatus::Queued, job.progress, job.checkpointJson));
                        }
                    }
                }
                scheduleTagDeletionPump();
            });
    }

    void MailMutationApplicationService::setThreadMaterializationCoordinator(
        ThreadMaterializationCoordinator* coordinator)
    {
        m_threadMaterializationCoordinator = coordinator;
    }

    void MailMutationApplicationService::accountConfigured(std::string accountId)
    {
        schedulePendingEmailMutationReplay(accountId);
        scheduleMailboxMutationReconciliation(std::move(accountId));
        scheduleTagDeletionPump();
    }

    void MailMutationApplicationService::networkBecameReachable()
    {
        for (const auto& accountId : m_accountRuntime.configuredAccountIds())
        {
            schedulePendingEmailMutationReplay(accountId);
            scheduleMailboxMutationReconciliation(accountId);
        }
        const auto listed = m_workScheduler.list();
        if (const auto* jobs = std::get_if<std::vector<WorkRecord>>(&listed))
        {
            for (const auto& job : *jobs)
            {
                if (job.kind == WorkKind::TagDeletion &&
                    job.status == WorkStatus::WaitingForNetwork && job.accountId.has_value() &&
                    m_accountRuntime.configurationFor(*job.accountId).has_value())
                {
                    static_cast<void>(m_workScheduler.update(job.jobId, WorkStatus::Queued,
                                                             job.progress, job.checkpointJson));
                }
            }
        }
        scheduleTagDeletionPump();
    }

    MessageContentApplicationService::MessageContentApplicationService(
        javelin::jmap::cache::DatabaseConnection& databaseConnection,
        javelin::jmap::MessageContentClient& messageContentClient,
        AccountRuntimeManager& accountRuntime, ApplicationErrorCoordinator& errorCoordinator,
        WorkScheduler& workScheduler, MailboxMaintenanceRegistry& mailboxMaintenanceRegistry,
        QObject* parent)
        : QObject(parent), m_databaseConnection(databaseConnection),
          m_messageContentClient(messageContentClient), m_accountRuntime(accountRuntime),
          m_errorCoordinator(errorCoordinator), m_workScheduler(workScheduler),
          m_mailboxMaintenanceRegistry(mailboxMaintenanceRegistry)
    {
    }

    void MessageContentApplicationService::setThreadMaterializationCoordinator(
        ThreadMaterializationCoordinator* threadMaterializationCoordinator)
    {
        m_threadMaterializationCoordinator = threadMaterializationCoordinator;
    }

    MailNotificationService::MailNotificationService(
        javelin::jmap::cache::DatabaseConnection& databaseConnection, QObject* parent)
        : QObject(parent), m_databaseConnection(databaseConnection)
    {
        m_localRetryTimer.setSingleShot(true);
        connect(&m_localRetryTimer, &QTimer::timeout, this,
                &MailNotificationService::retryLocalFailures);
    }

    ContactApplicationService::ContactApplicationService(
        javelin::jmap::cache::ContactRepository& contactRepository,
        javelin::jmap::contacts::ContactSyncEngine& syncEngine,
        AccountRuntimeManager& accountRuntime, ApplicationErrorCoordinator& errorCoordinator,
        WorkScheduler& workScheduler, javelin::app::undo::UndoManager& undoManager, QObject* parent)
        : QObject(parent), m_contactSyncEngine(syncEngine), m_accountRuntime(accountRuntime),
          m_errorCoordinator(errorCoordinator), m_workScheduler(workScheduler),
          m_undoManager(undoManager)
    {
        connect(&contactRepository, &javelin::jmap::cache::ContactRepository::contactsChanged, this,
                [this](const QString& accountId)
                {
                    Q_EMIT cacheCommitted(MailCacheChange{
                        .accountId = accountId,
                        .mailboxIds = {},
                        .queryWindows = {},
                        .searchWindows = {},
                        .mailboxTreeChanged = false,
                        .emailObjectsChanged = false,
                        .optimisticProjection = false,
                        .contactsChanged = true,
                    });
                });
        connect(&m_workScheduler, &WorkScheduler::jobsChanged, this,
                [this] { scheduleRefreshPump(); });
        connect(&m_workScheduler, &WorkScheduler::foregroundAvailabilityChanged, this,
                [this] { scheduleRefreshPump(); });
        connect(&m_accountRuntime, &AccountRuntimeManager::contactStateChanged, this,
                [this](const QString& ownerAccountId)
                { scheduleRefresh(ownerAccountId.toStdString()); });
        connect(&m_accountRuntime, &AccountRuntimeManager::accountConfigured, this,
                [this](const QString&) { restoreRefreshJobs(); });
        connect(&m_accountRuntime, &AccountRuntimeManager::configurationSetChanged, this,
                [this]
                {
                    const auto configured = m_accountRuntime.configuredAccountIds();
                    const std::unordered_set<std::string> configuredSet{configured.begin(),
                                                                        configured.end()};
                    std::erase_if(m_pendingContactRefreshes,
                                  [&configuredSet](const std::string& accountId)
                                  { return !configuredSet.contains(accountId); });
                });
        connect(&m_errorCoordinator, &ApplicationErrorCoordinator::authenticationPauseChanged, this,
                [this](const QString& connectionId, const bool paused)
                {
                    if (paused)
                        return;
                    const std::vector<std::string> pending{m_pendingContactRefreshes.begin(),
                                                           m_pendingContactRefreshes.end()};
                    for (const auto& accountId : pending)
                    {
                        const auto settings = m_accountRuntime.connectionSettingsFor(accountId);
                        if (settings.has_value() &&
                            settings->connectionId == connectionId.toStdString())
                            scheduleRefresh(accountId);
                    }
                });
    }

    CalendarApplicationService::CalendarApplicationService(
        javelin::jmap::cache::DatabaseConnection& databaseConnection,
        javelin::jmap::calendar::CalendarReader& calendarReader,
        javelin::jmap::calendar::CalendarProtocolClient& protocolClient,
        javelin::jmap::calendar::CalendarSyncEngine& syncEngine,
        javelin::jmap::calendar::CalendarMutationEngine& mutationEngine,
        AccountRuntimeManager& accountRuntime, ApplicationErrorCoordinator& errorCoordinator,
        WorkScheduler& workScheduler, javelin::app::undo::UndoManager& undoManager, QObject* parent)
        : QObject(parent), m_databaseConnection(databaseConnection),
          m_calendarReader(calendarReader), m_calendarProtocolClient(protocolClient),
          m_calendarSyncEngine(syncEngine), m_calendarMutationEngine(mutationEngine),
          m_accountRuntime(accountRuntime), m_errorCoordinator(errorCoordinator),
          m_workScheduler(workScheduler), m_undoManager(undoManager)
    {
        connect(&m_accountRuntime, &AccountRuntimeManager::calendarStateChanged, this,
                [this](const QString& ownerAccountId,
                       const javelin::jmap::sync::AccountTypeStateMap& changedStates)
                {
                    const bool calendarMetadataChanged =
                        std::ranges::any_of(changedStates, [](const auto& account)
                                            { return account.second.contains("Calendar"); });
                    const bool eventStateChanged =
                        std::ranges::any_of(changedStates, [](const auto& account)
                                            { return account.second.contains("CalendarEvent"); });
                    auto owner = ownerAccountId.toStdString();
                    if (!calendarMetadataChanged && !eventStateChanged)
                        return;
                    if (m_visibleCalendarRanges.contains(owner))
                    {
                        scheduleRefresh(std::move(owner));
                        return;
                    }
                    if (eventStateChanged)
                        m_calendarCatchUpRequiredOwners.insert(owner);
                    if (calendarMetadataChanged)
                        scheduleMetadataRefresh(std::move(owner));
                });
        connect(&m_accountRuntime, &AccountRuntimeManager::sessionRefreshed, this,
                [this](const QString& ownerAccountId)
                { requireCatchUp(ownerAccountId.toStdString()); });
        connect(&m_accountRuntime, &AccountRuntimeManager::stateChangeCatchUpRequired, this,
                [this](const QString& ownerAccountId)
                { requireCatchUp(ownerAccountId.toStdString()); });
        connect(&m_accountRuntime, &AccountRuntimeManager::accountRemoved, this,
                [this](const QString& ownerAccountId)
                {
                    const auto key = ownerAccountId.toStdString();
                    m_calendarSyncEngine.invalidateRefresh(key);
                    m_visibleCalendarRanges.erase(key);
                    m_calendarMetadataReadyOwners.erase(key);
                    m_calendarMetadataUsableOwners.erase(key);
                    m_calendarMetadataRefreshesInFlight.erase(key);
                    m_calendarRangeRefreshesInFlight.erase(key);
                    m_calendarMetadataRefreshPending.erase(key);
                    m_calendarStateRefreshesInFlight.erase(key);
                    m_calendarStateRefreshPending.erase(key);
                    m_calendarCatchUpRequiredOwners.erase(key);
                });
    }

    std::vector<std::string> CalendarApplicationService::calendarMetadataReadyOwners() const
    {
        return {m_calendarMetadataUsableOwners.begin(), m_calendarMetadataUsableOwners.end()};
    }

    void CalendarApplicationService::requireCatchUp(std::string ownerAccountId)
    {
        if (ownerAccountId.empty())
            return;
        m_calendarCatchUpRequiredOwners.insert(ownerAccountId);
        if (m_visibleCalendarRanges.contains(ownerAccountId))
            scheduleRefresh(std::move(ownerAccountId));
        else
            scheduleMetadataRefresh(std::move(ownerAccountId));
    }

    SieveApplicationService::SieveApplicationService(
        javelin::jmap::sieve::SieveProtocolClient& protocolClient,
        javelin::jmap::sieve::SieveMutationEngine& mutationEngine,
        AccountRuntimeManager& accountRuntime, ApplicationErrorCoordinator& errorCoordinator,
        WorkScheduler& workScheduler, javelin::app::undo::UndoManager& undoManager, QObject* parent)
        : QObject(parent), m_sieveProtocolClient(protocolClient),
          m_sieveMutationEngine(mutationEngine), m_accountRuntime(accountRuntime),
          m_errorCoordinator(errorCoordinator), m_workScheduler(workScheduler),
          m_undoManager(undoManager)
    {
    }

    void AccountRuntimeManager::applySettings(std::vector<AccountSyncConfiguration> configurations)
    {
        const bool accountConfigurationsChanged =
            m_configurations.size() != configurations.size() ||
            std::ranges::any_of(configurations,
                                [this](const AccountSyncConfiguration& configuration)
                                {
                                    const auto previous =
                                        m_configurations.find(configuration.accountId);
                                    return previous == m_configurations.end() ||
                                           !(previous->second == configuration);
                                });
        std::unordered_set<std::string> previousConnectionIds;
        for (const auto& [accountId, configuration] : m_configurations)
        {
            static_cast<void>(accountId);
            previousConnectionIds.insert(configuration.settings.connectionId);
        }
        std::unordered_set<std::string> configuredAccountIds;
        std::unordered_set<std::string> configuredConnectionIds;
        for (auto& configuration : configurations)
        {
            configuredConnectionIds.insert(configuration.settings.connectionId);
            const auto connectionId = configuration.settings.connectionId;
            const auto revision = configuration.settings.revision;
            configuredAccountIds.insert(configuration.accountId);
            const auto accountId = configuration.accountId;
            const auto previous = m_configurations.find(accountId);
            const bool configurationChanged =
                previous == m_configurations.end() || previous->second != configuration;
            m_configurations.insert_or_assign(accountId, std::move(configuration));
            m_errorCoordinator.settingsApplied(connectionId, revision);
            if (configurationChanged)
                applyAccountConfiguration(accountId);
            Q_EMIT accountConfigured(QString::fromStdString(accountId));
        }

        for (auto coordinatorIt = m_coordinators.begin(); coordinatorIt != m_coordinators.end();)
        {
            if (configuredAccountIds.contains(coordinatorIt->first))
            {
                ++coordinatorIt;
                continue;
            }

            const auto removedAccountId = coordinatorIt->first;
            Q_EMIT accountStatusChanged(QString::fromStdString(removedAccountId),
                                        AccountSyncCoordinator::Status::Disconnected);
            disconnect(coordinatorIt->second.get(), nullptr, this, nullptr);
            coordinatorIt = m_coordinators.erase(coordinatorIt);
            m_observedMailboxIds.erase(removedAccountId);
            m_notificationBaselineRetryAttempts.erase(removedAccountId);
            m_notificationBaselineRetriesPending.erase(removedAccountId);
            Q_EMIT accountRemoved(QString::fromStdString(removedAccountId));
        }
        std::erase_if(m_configurations, [&configuredAccountIds](const auto& entry)
                      { return !configuredAccountIds.contains(entry.first); });
        for (const auto& connectionId : previousConnectionIds)
        {
            if (!configuredConnectionIds.contains(connectionId))
                m_errorCoordinator.forgetConnection(connectionId);
        }
        if (accountConfigurationsChanged)
            refreshConfiguredSessions();
        Q_EMIT configurationSetChanged();
    }

    void MailQueryApplicationService::setThreadMaterializationCoordinator(
        ThreadMaterializationCoordinator* coordinator)
    {
        if (m_threadMaterializationCoordinator != nullptr)
            disconnect(m_threadMaterializationCoordinator, nullptr, this, nullptr);
        m_threadMaterializationCoordinator = coordinator;
        if (coordinator == nullptr)
            return;
        connect(coordinator, &ThreadMaterializationCoordinator::materializationStarted, this,
                [this](QString accountId, QStringList threadIds)
                {
                    Q_EMIT threadMaterializationProgress({
                        .accountId = std::move(accountId),
                        .threadIds = std::move(threadIds),
                        .inFlight = true,
                        .success = true,
                        .error = {},
                    });
                });
        connect(coordinator, &ThreadMaterializationCoordinator::materializationFinished, this,
                [this](QString accountId, QStringList threadIds, const bool success, QString error)
                {
                    Q_EMIT threadMaterializationProgress({
                        .accountId = std::move(accountId),
                        .threadIds = std::move(threadIds),
                        .inFlight = false,
                        .success = success,
                        .error = std::move(error),
                    });
                });
        const auto restoreTargets = [this]
        {
            if (m_threadMaterializationCoordinator == nullptr)
                return;
            for (const auto& accountId : m_accountRuntime.configuredAccountIds())
            {
                if (const auto error =
                        m_threadMaterializationCoordinator->restoreAccount(accountId))
                    qWarning().noquote() << "Could not restore Thread materialization targets"
                                         << QString::fromStdString(accountId) << error->message;
            }
        };
        connect(&m_accountRuntime, &AccountRuntimeManager::configurationSetChanged, this,
                restoreTargets);
        connect(&m_accountRuntime, &AccountRuntimeManager::networkReachable, this, restoreTargets);
        restoreTargets();
    }

    void AccountRuntimeManager::setAuthenticationRefreshHandler(
        javelin::jmap::auth::AccessTokenRefreshHandler handler)
    {
        m_authenticationRefreshHandler = std::move(handler);
    }

    void AccountRuntimeManager::networkBecameReachable()
    {
        m_networkAccessManager.clearConnectionCache();
        for (const auto& [accountId, coordinator] : m_coordinators)
        {
            static_cast<void>(accountId);
            coordinator->networkBecameReachable();
        }
        Q_EMIT networkReachable();
    }

    std::unordered_map<std::string, AccountSyncCoordinator::Status>
    AccountRuntimeManager::accountStatuses() const
    {
        std::unordered_map<std::string, AccountSyncCoordinator::Status> statuses;
        statuses.reserve(m_coordinators.size());
        for (const auto& [accountId, coordinator] : m_coordinators)
            statuses.emplace(accountId, coordinator->status());
        return statuses;
    }

    std::optional<AccountConnectionSettings>
    AccountRuntimeManager::connectionSettingsFor(const std::string_view ownerAccountId) const
    {
        const auto configuration = m_configurations.find(std::string{ownerAccountId});
        return configuration != m_configurations.end()
                   ? std::optional{configuration->second.settings}
                   : std::nullopt;
    }

    std::optional<AccountSyncConfigurationView>
    AccountRuntimeManager::configurationFor(const std::string_view accountId) const
    {
        const auto configuration = m_configurations.find(std::string{accountId});
        if (configuration == m_configurations.end())
            return std::nullopt;
        return AccountSyncConfigurationView{configuration->second};
    }

    std::vector<std::string> AccountRuntimeManager::configuredAccountIds() const
    {
        std::vector<std::string> accountIds;
        accountIds.reserve(m_configurations.size());
        for (const auto& [accountId, configuration] : m_configurations)
        {
            static_cast<void>(configuration);
            accountIds.push_back(accountId);
        }
        return accountIds;
    }

    void AccountRuntimeManager::refreshAccountConfiguration(const std::string_view accountId)
    {
        applyAccountConfiguration(std::string{accountId});
    }

    void AccountRuntimeManager::setObservedMailboxIds(std::string accountId,
                                                      std::vector<std::string> mailboxIds)
    {
        if (mailboxIds.empty())
            m_observedMailboxIds.erase(accountId);
        else
            m_observedMailboxIds.insert_or_assign(accountId, std::move(mailboxIds));
        applyAccountConfiguration(accountId);
    }

    void AccountRuntimeManager::refreshConfiguredSessions()
    {
        javelin::jmap::cache::SessionRepository sessions{m_databaseConnection};
        for (const auto& [accountId, configuration] : m_configurations)
        {
            if (m_errorCoordinator.authenticationPaused(configuration.settings.connectionId,
                                                        configuration.settings.revision))
                continue;
            const auto loaded = sessions.load(accountId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&loaded))
            {
                qWarning().noquote() << "JMAP startup session lookup failed"
                                     << QString::fromStdString(accountId) << error->message;
                continue;
            }

            const auto& session = std::get<std::optional<javelin::jmap::api::Session>>(loaded);
            if (session.has_value())
            {
                startSessionRefresh(accountId, configuration.settings);
            }
        }
    }

    void AccountRuntimeManager::startSessionRefresh(const std::string& ownerAccountId,
                                                    const AccountConnectionSettings& settings)
    {
        if (!m_sessionRefreshesInFlight.insert(ownerAccountId).second)
        {
            return;
        }

        const auto accountResult = m_accountRepository.findById(ownerAccountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&accountResult))
        {
            m_sessionRefreshesInFlight.erase(ownerAccountId);
            qWarning().noquote() << "JMAP startup account identity lookup failed"
                                 << QString::fromStdString(ownerAccountId) << error->message;
            return;
        }
        const auto& account =
            std::get<std::optional<javelin::jmap::cache::CachedAccount>>(accountResult);
        if (!account.has_value() || account->remoteAccountId.empty())
        {
            m_sessionRefreshesInFlight.erase(ownerAccountId);
            qWarning().noquote() << "JMAP startup account has no remote identity"
                                 << QString::fromStdString(ownerAccountId);
            return;
        }

        m_workScheduler.beginForegroundWork();
        const auto appliedSettings = settings;
        auto task = m_sessionRefreshClient.refresh(toLiveConnectionSettings(settings),
                                                   settings.connectionId, ownerAccountId,
                                                   account->remoteAccountId);
        QCoro::connect(
            std::move(task), this,
            [this, ownerAccountId, appliedSettings](javelin::jmap::SessionRefreshResult result)
            {
                m_sessionRefreshesInFlight.erase(ownerAccountId);
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    qWarning().noquote()
                        << "JMAP startup session discovery failed"
                        << QString::fromStdString(ownerAccountId) << error->message;
                    m_errorCoordinator.reportFailure(appliedSettings, ownerAccountId,
                                                     QStringLiteral("Discover server session"),
                                                     *error);
                    m_workScheduler.endForegroundWork();
                    return;
                }

                m_errorCoordinator.reportSuccess(appliedSettings.connectionId);

                const auto& summary = std::get<javelin::jmap::SessionRefreshSummary>(result);
                qInfo() << "JMAP startup session discovered"
                        << (summary.websocketAdvertised ? "WebSocket" : "HTTP only");
                for (const auto& [accountId, configuration] : m_configurations)
                {
                    if (configuration.settings.connectionId == appliedSettings.connectionId)
                    {
                        applyAccountConfiguration(accountId);
                    }
                }
                Q_EMIT sessionCapabilitiesChanged(QString::fromStdString(ownerAccountId));
                Q_EMIT sessionRefreshed(QString::fromStdString(ownerAccountId));
                m_workScheduler.endForegroundWork();
            });
    }

    void
    MailMutationApplicationService::scheduleMailboxMutationReconciliation(std::string accountId)
    {
        if (!m_accountRuntime.connectionSettingsFor(accountId).has_value() ||
            !m_mailboxMutationReconciliationsInFlight.insert(accountId).second)
            return;
        auto task = reconcileMailboxMutations(accountId);
        QCoro::connect(std::move(task), this, [] {});
    }

    QCoro::Task<void>
    MailMutationApplicationService::reconcileMailboxMutations(std::string accountId)
    {
        const auto configuration = m_accountRuntime.connectionSettingsFor(accountId);
        if (!configuration.has_value())
        {
            m_mailboxMutationReconciliationsInFlight.erase(accountId);
            co_return;
        }
        const auto settings = toLiveConnectionSettings(*configuration);
        bool changed = false;
        bool reconciliationFailed = false;

        auto subscription =
            co_await m_mailboxMutationEngine.reconcileSubscription(settings, accountId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&subscription))
        {
            if (error->code != javelin::jmap::OperationErrorCode::Conflict)
            {
                qWarning().noquote() << "Mailbox visibility reconciliation failed"
                                     << QString::fromStdString(accountId) << error->message;
                reconciliationFailed = true;
            }
        }
        else if (!std::get<javelin::jmap::MailboxSubscriptionChange>(subscription)
                      .mailboxId.empty())
        {
            changed = true;
        }

        auto creation = co_await m_mailboxMutationEngine.reconcileCreate(settings, accountId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&creation))
        {
            if (error->code != javelin::jmap::OperationErrorCode::Conflict)
            {
                qWarning().noquote() << "Mailbox creation reconciliation failed"
                                     << QString::fromStdString(accountId) << error->message;
                reconciliationFailed = true;
            }
        }
        else if (!std::get<javelin::jmap::MailboxCreateChange>(creation).mailboxId.empty())
        {
            changed = true;
        }

        auto destruction = co_await m_mailboxMutationEngine.reconcileDestroy(settings, accountId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&destruction))
        {
            if (error->code != javelin::jmap::OperationErrorCode::Conflict)
            {
                qWarning().noquote() << "Mailbox deletion reconciliation failed"
                                     << QString::fromStdString(accountId) << error->message;
                reconciliationFailed = true;
            }
        }
        else if (!std::get<javelin::jmap::MailboxDestroyChange>(destruction).mailboxId.empty())
        {
            changed = true;
        }

        javelin::jmap::sync::MutationJournalRepository mutations{m_databaseConnection};
        const auto active = mutations.listActive({.accountId = accountId, .dataType = "Mailbox"});
        const bool unresolved =
            std::holds_alternative<javelin::jmap::cache::DatabaseError>(active) ||
            !std::get<std::vector<javelin::jmap::sync::MutationRecord>>(active).empty();
        if (reconciliationFailed && !unresolved)
            changed = true;

        m_mailboxMutationReconciliationsInFlight.erase(accountId);
        if (!changed)
            co_return;
        Q_EMIT cacheCommitted(MailCacheChange{
            .accountId = QString::fromStdString(accountId),
            .mailboxIds = {},
            .queryWindows = {},
            .searchWindows = {},
            .mailboxTreeChanged = true,
            .emailObjectsChanged = false,
            .optimisticProjection = unresolved,
        });
        m_accountRuntime.refreshAccountConfiguration(accountId);
    }

    void MailMutationApplicationService::schedulePendingEmailMutationReplay(std::string accountId)
    {
        if (!m_accountRuntime.connectionSettingsFor(accountId).has_value() ||
            !m_pendingMutationReplaysInFlight.insert(accountId).second)
        {
            return;
        }

        QTimer::singleShot(
            0, this,
            [this, accountId = std::move(accountId)]
            {
                auto task = submitPendingEmailMutations(accountId);
                QCoro::connect(
                    std::move(task), this,
                    [this, accountId](javelin::jmap::SubmittedEmailMutationsResult result)
                    {
                        m_pendingMutationReplaysInFlight.erase(accountId);
                        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                        {
                            qWarning().noquote()
                                << "Queued mail replay failed" << QString::fromStdString(accountId)
                                << error->message;
                            return;
                        }

                        const auto& summary =
                            std::get<javelin::jmap::SubmittedEmailMutations>(result);
                        if (summary.attemptedEmailCount == 0)
                            return;
                        qInfo() << "Queued mail replay submitted" << summary.updatedEmailCount
                                << "updated" << summary.failedEmailCount << "failed";
                        if (m_accountRuntime.connectionSettingsFor(accountId).has_value())
                            schedulePendingEmailMutationReplay(accountId);
                    });
            });
    }

    void AccountRuntimeManager::applyAccountConfiguration(const std::string& accountId)
    {
        const auto stored = m_configurations.find(accountId);
        if (stored == m_configurations.end())
            return;

        auto configuration = stored->second;
        const auto observed = m_observedMailboxIds.find(accountId);
        if (observed != m_observedMailboxIds.end())
            configuration.mailboxIds.insert(configuration.mailboxIds.end(),
                                            observed->second.begin(), observed->second.end());
        std::ranges::sort(configuration.mailboxIds);
        configuration.mailboxIds.erase(std::ranges::unique(configuration.mailboxIds).begin(),
                                       configuration.mailboxIds.end());

        bool notificationBaselineCheckFailed = false;
        bool notificationBaselineRequired = false;
        javelin::jmap::cache::NotificationRepository notifications{m_databaseConnection};
        if (const auto activeMailboxError = notifications.retainActiveMailboxes(
                accountId, configuration.notificationMailboxIds))
        {
            qWarning().noquote() << "Could not prune disabled notification mailboxes"
                                 << QString::fromStdString(accountId)
                                 << activeMailboxError->message;
            notificationBaselineCheckFailed = true;
            notificationBaselineRequired = true;
        }
        else
        {
            javelin::jmap::cache::SyncStateRepository syncStates{m_databaseConnection};
            const auto emailState =
                syncStates.find({.accountId = accountId, .objectType = "Email", .queryKey = {}});
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&emailState))
            {
                qWarning().noquote() << "Could not read Email state for notification baseline"
                                     << QString::fromStdString(accountId) << error->message;
                notificationBaselineCheckFailed = true;
                notificationBaselineRequired = true;
            }
            else if (const auto& state =
                         std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(emailState);
                     !state.has_value())
            {
                notificationBaselineRequired = !configuration.notificationMailboxIds.empty();
            }
            else
            {
                const auto activeResult = notifications.activeMailboxIds(accountId);
                if (const auto* activeReadError =
                        std::get_if<javelin::jmap::cache::DatabaseError>(&activeResult))
                {
                    qWarning().noquote()
                        << "Could not read active notification mailboxes"
                        << QString::fromStdString(accountId) << activeReadError->message;
                    notificationBaselineCheckFailed = true;
                    notificationBaselineRequired = true;
                }
                else
                {
                    auto activeMailboxIds = std::get<std::vector<std::string>>(activeResult);
                    auto desiredMailboxIds = configuration.notificationMailboxIds;
                    std::ranges::sort(activeMailboxIds);
                    std::ranges::sort(desiredMailboxIds);
                    notificationBaselineRequired = activeMailboxIds != desiredMailboxIds;
                }
            }
        }
        if (notificationBaselineCheckFailed)
            scheduleNotificationBaselineRetry(accountId);
        else
            m_notificationBaselineRetryAttempts.erase(accountId);

        auto [coordinatorIt, inserted] = m_coordinators.try_emplace(accountId);
        if (inserted)
        {
            coordinatorIt->second = std::make_unique<AccountSyncCoordinator>(
                m_databaseConnection, m_methodTransport, m_networkAccessManager,
                m_transportCooldowns, m_accountRepository, m_mailboxReader, m_workScheduler,
                m_endpointRetryGate, m_authenticationRefreshHandler, this);
            connectCoordinator(coordinatorIt->first, *coordinatorIt->second);
        }
        if (m_errorCoordinator.authenticationPaused(configuration.settings.connectionId,
                                                    configuration.settings.revision))
        {
            coordinatorIt->second->pauseForAuthentication();
            return;
        }
        const auto desiredNotificationMailboxIds = configuration.notificationMailboxIds;
        coordinatorIt->second->applySettings(std::move(configuration.settings), accountId,
                                             std::move(configuration.mailboxIds),
                                             std::move(configuration.notificationMailboxIds));
        const auto baselineConfigurationError =
            notificationBaselineRequired
                ? coordinatorIt->second->requestNotificationBaseline(desiredNotificationMailboxIds)
                : coordinatorIt->second->cancelNotificationBaseline();
        if (baselineConfigurationError.has_value())
        {
            qWarning().noquote() << "Could not serialize notification baseline configuration"
                                 << QString::fromStdString(accountId)
                                 << baselineConfigurationError->message;
            scheduleNotificationBaselineRetry(accountId);
        }
    }

    void AccountRuntimeManager::scheduleNotificationBaselineRetry(const std::string& accountId)
    {
        // This retry owns durable configuration and baseline setup only. The coordinator retains
        // execution failures and retries the synchronized Email transition; active notification
        // mailboxes are replaced only by that transition's commit.
        if (!m_configurations.contains(accountId) ||
            !m_notificationBaselineRetriesPending.insert(accountId).second)
            return;

        auto& attempts = m_notificationBaselineRetryAttempts[accountId];
        const auto exponent = std::min(attempts, notificationBaselineRetryMaximumExponent);
        ++attempts;
        const auto delay = std::chrono::seconds{1U << exponent};
        QTimer::singleShot(delay, this,
                           [this, accountId]
                           {
                               m_notificationBaselineRetriesPending.erase(accountId);
                               if (!m_configurations.contains(accountId) ||
                                   !m_notificationBaselineRetryAttempts.contains(accountId))
                                   return;
                               applyAccountConfiguration(accountId);
                           });
    }

    MailboxObservation MailQueryApplicationService::observeMailbox(std::string accountId,
                                                                   std::string mailboxId)
    {
        const auto configuredAccountId = accountId;
        const auto observedMailboxId = mailboxId;
        const auto observationId =
            m_mailboxInterests.observe(std::move(accountId), std::move(mailboxId));
        publishObservedMailboxIds(configuredAccountId);
        if (m_threadMaterializationCoordinator != nullptr)
        {
            if (const auto error = m_threadMaterializationCoordinator->enqueueRetainedMailbox(
                    configuredAccountId, observedMailboxId, WorkPriority::VisibleMaterialization))
            {
                qWarning().noquote() << "Could not enqueue retained mailbox Thread materialization"
                                     << error->message;
            }
        }
        return MailboxObservation{*this, observationId};
    }

    MailboxObservationLease
    MailQueryApplicationService::beginMailboxObservation(std::string accountId,
                                                         std::string mailboxId)
    {
        auto observation = std::make_shared<MailboxObservation>(
            observeMailbox(std::move(accountId), std::move(mailboxId)));
        return MailboxObservationLease{[observation = std::move(observation)]() mutable
                                       { observation.reset(); }};
    }

    void MailQueryApplicationService::releaseMailboxObservation(
        const javelin::jmap::sync::MailboxInterestRegistry::ObservationId observationId)
    {
        const auto interest = m_mailboxInterests.unobserve(observationId);
        if (!interest.has_value())
        {
            return;
        }
        publishObservedMailboxIds(interest->accountId);
    }

    void MailQueryApplicationService::publishObservedMailboxIds(const std::string& accountId)
    {
        m_accountRuntime.setObservedMailboxIds(accountId, m_mailboxInterests.mailboxIds(accountId));
    }

    bool MailQueryApplicationService::beginSearchWindowRequest(const std::string& leaseKey)
    {
        auto& state = m_searchWindowRequests[leaseKey];
        if (state.retired)
            return false;
        ++state.activeRequests;
        return true;
    }

    void MailQueryApplicationService::finishSearchWindowRequest(const std::string& leaseKey)
    {
        const auto found = m_searchWindowRequests.find(leaseKey);
        if (found == m_searchWindowRequests.end())
            return;
        if (found->second.activeRequests > 0)
            --found->second.activeRequests;
        if (found->second.retired && found->second.activeRequests == 0)
            m_searchWindowRequests.erase(found);
    }

    bool MailQueryApplicationService::searchWindowRetired(const std::string& leaseKey) const
    {
        const auto found = m_searchWindowRequests.find(leaseKey);
        return found != m_searchWindowRequests.end() && found->second.retired;
    }

    bool AccountRuntimeManager::requestAccountSynchronization(const std::string_view accountId)
    {
        const auto coordinator = m_coordinators.find(std::string{accountId});
        if (coordinator == m_coordinators.end())
            return false;
        return coordinator->second->requestSynchronization();
    }

    bool AccountRuntimeManager::requestMailboxSynchronization(const std::string_view accountId,
                                                              const std::string_view mailboxId)
    {
        const auto coordinator = m_coordinators.find(std::string{accountId});
        if (coordinator == m_coordinators.end())
            return false;
        return coordinator->second->requestMailboxSynchronization(mailboxId);
    }

    void MailNotificationService::accountChanged(const QString& accountId)
    {
        javelin::jmap::cache::NotificationRepository notifications{m_databaseConnection};
        const auto claimed = notifications.claimPendingEvents(accountId.toStdString());
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&claimed))
        {
            qWarning().noquote() << "Claim mail notification delivery failed" << error->message;
            Q_EMIT deliveryRetryRequired(accountId);
            return;
        }

        const auto& pending =
            std::get<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(claimed);
        if (pending.empty())
            return;

        std::map<std::string,
                 std::vector<const javelin::jmap::cache::MailNotificationPendingEvent*>>
            byMailbox;
        for (const auto& event : pending)
            byMailbox[event.mailboxId].push_back(&event);

        javelin::jmap::cache::MailboxRepository mailboxes{m_databaseConnection};
        for (const auto& [mailboxId, events] : byMailbox)
        {
            QString mailboxName = QString::fromStdString(mailboxId);
            const auto mailboxResult = mailboxes.find(accountId.toStdString(), mailboxId);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&mailboxResult))
            {
                qWarning().noquote() << "Read notification mailbox name failed" << error->message;
            }
            else if (const auto& mailbox =
                         std::get<std::optional<javelin::jmap::domain::Mailbox>>(mailboxResult);
                     mailbox.has_value())
            {
                mailboxName = QString::fromStdString(mailbox->name);
            }

            const auto& target = *events.front();
            const auto title =
                events.size() == 1
                    ? QStringLiteral("New mail in %1").arg(mailboxName)
                    : QStringLiteral("%1 new messages in %2").arg(events.size()).arg(mailboxName);
            const auto message = subjectForDisplay(target.subject);
            QStringList deliveredEmailIds;
            deliveredEmailIds.reserve(static_cast<qsizetype>(events.size()));
            for (const auto* event : events)
                deliveredEmailIds.push_back(QString::fromStdString(event->emailId));

            Q_EMIT notificationRaised(accountId, QString::fromStdString(mailboxId),
                                      QString::fromStdString(target.threadId),
                                      QString::fromStdString(target.emailId), mailboxName, title,
                                      message, deliveredEmailIds);
        }
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MailNotificationService::markDelivered(const std::string_view accountId,
                                           const QStringList& emailIds)
    {
        std::vector<std::string> ids;
        ids.reserve(static_cast<std::size_t>(emailIds.size()));
        for (const auto& emailId : emailIds)
            ids.push_back(emailId.toStdString());
        javelin::jmap::cache::NotificationRepository notifications{m_databaseConnection};
        const auto error = notifications.markDelivered(accountId, ids);
        if (error.has_value())
            rememberLocalRetry(m_markDeliveredRetries, QString::fromUtf8(accountId), emailIds);
        return error;
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MailNotificationService::releaseDispatches(const std::string_view accountId,
                                               const QStringList& emailIds)
    {
        std::vector<std::string> ids;
        ids.reserve(static_cast<std::size_t>(emailIds.size()));
        for (const auto& emailId : emailIds)
            ids.push_back(emailId.toStdString());
        javelin::jmap::cache::NotificationRepository notifications{m_databaseConnection};
        const auto error = notifications.releaseDispatches(accountId, ids);
        if (error.has_value())
            rememberLocalRetry(m_releaseDispatchRetries, QString::fromUtf8(accountId), emailIds);
        return error;
    }

    void MailNotificationService::rememberLocalRetry(RetryMap& retries, QString accountId,
                                                     const QStringList& emailIds)
    {
        if (emailIds.isEmpty())
            return;
        auto& pending = retries[std::move(accountId)];
        for (const auto& emailId : emailIds)
            pending.insert(emailId);
        scheduleLocalRetry();
    }

    void MailNotificationService::scheduleLocalRetry()
    {
        if (m_localRetryTimer.isActive() ||
            (m_markDeliveredRetries.isEmpty() && m_releaseDispatchRetries.isEmpty()))
            return;

        if (m_localRetryAttempts == 0)
        {
            ++m_localRetryAttempts;
            m_localRetryTimer.start(0);
            return;
        }

        const auto exponent =
            std::min(m_localRetryAttempts - 1, mailNotificationLocalRetryMaximumExponent);
        ++m_localRetryAttempts;
        m_localRetryTimer.start(
            static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::seconds{1U << exponent})
                                 .count()));
    }

    void MailNotificationService::retryLocalFailures()
    {
        auto deliveredRetries = std::exchange(m_markDeliveredRetries, {});
        auto releaseRetries = std::exchange(m_releaseDispatchRetries, {});

        for (auto it = deliveredRetries.cbegin(); it != deliveredRetries.cend(); ++it)
        {
            QStringList emailIds;
            emailIds.reserve(it.value().size());
            for (const auto& emailId : it.value())
                emailIds.push_back(emailId);
            if (const auto error = markDelivered(it.key().toStdString(), emailIds))
                qWarning().noquote()
                    << "Retry mail notification delivery acknowledgement failed" << error->message;
        }

        for (auto it = releaseRetries.cbegin(); it != releaseRetries.cend(); ++it)
        {
            QStringList emailIds;
            emailIds.reserve(it.value().size());
            for (const auto& emailId : it.value())
                emailIds.push_back(emailId);
            if (const auto error = releaseDispatches(it.key().toStdString(), emailIds))
            {
                qWarning().noquote()
                    << "Retry mail notification dispatch release failed" << error->message;
                continue;
            }
            Q_EMIT deliveryRetryRequired(it.key());
        }

        if (m_markDeliveredRetries.isEmpty() && m_releaseDispatchRetries.isEmpty())
            m_localRetryAttempts = 0;
        else
            scheduleLocalRetry();
    }

    std::optional<javelin::jmap::cache::DatabaseError> MailNotificationService::recoverDispatches()
    {
        javelin::jmap::cache::NotificationRepository notifications{m_databaseConnection};
        return notifications.recoverDispatches();
    }

    void MailQueryApplicationService::publishCacheChange(MailCacheChange change)
    {
        Q_EMIT cacheCommitted(std::move(change));
    }

    void MailQueryApplicationService::publishMailboxWindowCommitted(QString accountId,
                                                                    QString mailboxId,
                                                                    const std::size_t offset,
                                                                    const std::size_t limit)
    {
        Q_EMIT cacheCommitted(MailCacheChange{
            .accountId = std::move(accountId),
            .mailboxIds = {},
            .queryWindows = {MailboxQueryWindowChange{
                .mailboxId = std::move(mailboxId),
                .offset = offset,
                .limit = limit,
                .total = std::nullopt,
            }},
            .searchWindows = {},
            .mailboxTreeChanged = false,
            .emailObjectsChanged = false,
        });
    }

    void MessageContentApplicationService::publishMessageContentCommitted(QString accountId,
                                                                          QString emailId)
    {
        Q_EMIT cacheCommitted(MailCacheChange{
            .accountId = std::move(accountId),
            .mailboxIds = {},
            .queryWindows = {},
            .searchWindows = {},
            .messageContentEmailIds = {std::move(emailId)},
        });
    }

    void
    MailQueryApplicationService::publishThreadMaterializationCommitted(QString accountId,
                                                                       const QStringList& threadIds)
    {
        if (threadIds.empty())
            return;

        QJsonArray requestedThreadIds;
        for (const auto& threadId : threadIds)
            requestedThreadIds.push_back(threadId);
        const auto requestedThreadIdsJson =
            QString::fromUtf8(QJsonDocument{requestedThreadIds}.toJson(QJsonDocument::Compact));

        MailCacheChange change{
            .accountId = accountId,
            .mailboxIds = {},
            .queryWindows = {},
            .searchWindows = {},
        };
        QSqlQuery mailboxWindows{m_databaseConnection.database()};
        mailboxWindows.prepare(QStringLiteral(
            "WITH requested(thread_id) AS MATERIALIZED (SELECT value FROM "
            "json_each(:thread_ids_json)) "
            "SELECT DISTINCT w.mailbox_id,w.requested_offset,w.requested_limit,w.total FROM "
            "requested r CROSS JOIN emails e INDEXED BY idx_emails_thread ON "
            "e.account_id=:account_id AND e.thread_id=r.thread_id CROSS JOIN "
            "mailbox_query_window_items i INDEXED BY idx_mailbox_query_window_items_email ON "
            "i.account_id=e.account_id AND i.email_id=e.email_id INNER JOIN "
            "mailbox_query_windows w ON w.account_id=i.account_id AND "
            "w.query_key=i.query_key AND w.requested_offset=i.requested_offset AND "
            "w.requested_limit=i.requested_limit"));
        QSqlQuery searchWindows{m_databaseConnection.database()};
        searchWindows.prepare(QStringLiteral(
            "WITH requested(thread_id) AS MATERIALIZED (SELECT value FROM "
            "json_each(:thread_ids_json)) "
            "SELECT DISTINCT w.query_key,w.window_offset,w.window_limit,w.total FROM requested r "
            "CROSS JOIN emails e INDEXED BY idx_emails_thread ON e.account_id=:account_id AND "
            "e.thread_id=r.thread_id CROSS JOIN search_window_items i INDEXED BY "
            "idx_search_window_items_email ON i.account_id=e.account_id AND "
            "i.email_id=e.email_id INNER JOIN search_windows w ON w.account_id=i.account_id AND "
            "w.query_key=i.query_key AND w.window_offset=i.window_offset AND "
            "w.window_limit=i.window_limit"));

        mailboxWindows.bindValue(QStringLiteral(":thread_ids_json"), requestedThreadIdsJson);
        mailboxWindows.bindValue(QStringLiteral(":account_id"), accountId);
        if (!mailboxWindows.exec())
        {
            qWarning().noquote() << "Could not resolve Thread mailbox-window invalidation"
                                 << mailboxWindows.lastError().text();
            return;
        }
        while (mailboxWindows.next())
        {
            change.queryWindows.push_back(MailboxQueryWindowChange{
                .mailboxId = mailboxWindows.value(0).toString(),
                .offset = mailboxWindows.value(1).toULongLong(),
                .limit = mailboxWindows.value(2).toULongLong(),
                .total = mailboxWindows.value(3).isNull()
                             ? std::nullopt
                             : std::optional<std::size_t>{mailboxWindows.value(3).toULongLong()},
            });
        }

        searchWindows.bindValue(QStringLiteral(":thread_ids_json"), requestedThreadIdsJson);
        searchWindows.bindValue(QStringLiteral(":account_id"), accountId);
        if (!searchWindows.exec())
        {
            qWarning().noquote() << "Could not resolve Thread search-window invalidation"
                                 << searchWindows.lastError().text();
            return;
        }
        while (searchWindows.next())
        {
            change.searchWindows.push_back(SearchQueryWindowChange{
                .queryKey = searchWindows.value(0).toString(),
                .offset = searchWindows.value(1).toULongLong(),
                .limit = searchWindows.value(2).toULongLong(),
                .total = searchWindows.value(3).isNull()
                             ? std::nullopt
                             : std::optional<std::size_t>{searchWindows.value(3).toULongLong()},
            });
        }

        if (!change.queryWindows.empty() || !change.searchWindows.empty())
            Q_EMIT cacheCommitted(std::move(change));
    }

    QCoro::Task<MailboxWindowResult>
    MailQueryApplicationService::requestMailboxWindow(MailboxWindowIntent intent)
    {
        if (m_mailboxMaintenanceRegistry.isActive(QString::fromStdString(intent.accountId),
                                                  QString::fromStdString(intent.mailboxId)))
        {
            co_return javelin::jmap::OperationError{
                .message = i18n("The mailbox cache is being cleared."),
            };
        }
        const auto configuration = m_accountRuntime.connectionSettingsFor(intent.accountId);
        if (!configuration.has_value())
        {
            co_return javelin::jmap::OperationError{
                .message = accountSynchronizationNotConfigured(),
            };
        }

        const auto queryKey = javelin::jmap::sync::mailboxQueryKey({
            .mailboxId = intent.mailboxId,
            .sortProperty = javelin::jmap::query::propertyName(intent.sort.property),
            .isAscending = javelin::jmap::query::isAscending(intent.sort),
            .collapseThreads = true,
        });
        const auto canonicalQueryKey = javelin::jmap::sync::mailboxQueryKey({
            .mailboxId = intent.mailboxId,
            .sortProperty = "receivedAt",
            .isAscending = false,
            .collapseThreads = true,
        });
        const auto offlineStateResult = m_mailboxMessageReader.completeOfflineMailboxQueryState(
            intent.accountId, intent.mailboxId, canonicalQueryKey);
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&offlineStateResult))
        {
            co_return javelin::jmap::operationError(*error);
        }
        const auto& offlineState = std::get<std::optional<std::string>>(offlineStateResult);
        if (!intent.forceRefresh)
        {
            javelin::jmap::cache::QueryWindowReadRepository queryWindows{m_databaseConnection,
                                                                         m_mailboxMessageReader};
            const auto cachedResult = queryWindows.loadMailboxWindow(
                intent.accountId, queryKey, intent.offset, intent.limit, intent.sort);
            if (const auto* cached =
                    std::get_if<std::optional<javelin::jmap::cache::MailboxWindowPage>>(
                        &cachedResult);
                cached != nullptr && cached->has_value())
            {
                const bool projectedDisplayCurrent =
                    (*cached)->coverage ==
                        javelin::jmap::cache::QueryWindowCoverage::LocallyProjected &&
                    javelin::jmap::cache::isDisplayCurrent((*cached)->coverage,
                                                           (*cached)->materialization);
                const bool authoritativeCurrent =
                    javelin::jmap::cache::isPaginationAuthoritative((*cached)->coverage,
                                                                    (*cached)->materialization) &&
                    (!offlineState.has_value() || (*cached)->queryState == *offlineState);
                if (projectedDisplayCurrent || authoritativeCurrent)
                {
                    if (m_threadMaterializationCoordinator != nullptr)
                    {
                        if (const auto error =
                                m_threadMaterializationCoordinator->enqueueMailboxWindow(
                                    intent.accountId, queryKey, intent.offset, intent.limit,
                                    WorkPriority::VisibleMaterialization))
                            qWarning().noquote()
                                << "Could not enqueue cached mailbox Thread materialization"
                                << error->message;
                    }
                    co_return MailboxWindowSummary{
                        .accountId = std::move(intent.accountId),
                        .mailboxId = std::move(intent.mailboxId),
                        .offset = intent.offset,
                        .limit = intent.limit,
                        .position = (*cached)->position,
                        .returnedLimit = (*cached)->returnedLimit,
                        .representativeCount = (*cached)->items.size(),
                        .total = (*cached)->total,
                        .queryState = (*cached)->queryState,
                    };
                }
            }
        }
        if (offlineState.has_value())
        {
            const std::string& state = *offlineState;
            javelin::jmap::cache::MailboxWindowRepository windows{m_databaseConnection};
            if (!intent.forceRefresh)
            {
                const auto cachedResult =
                    windows.find(intent.accountId, queryKey, intent.offset, intent.limit);
                if (const auto* error =
                        std::get_if<javelin::jmap::cache::DatabaseError>(&cachedResult))
                    co_return javelin::jmap::operationError(*error);
                const auto& cached =
                    std::get<std::optional<javelin::jmap::cache::MailboxWindowRecord>>(
                        cachedResult);
                if (cached.has_value() &&
                    javelin::jmap::cache::isPaginationAuthoritative(cached->coverage,
                                                                    cached->materialization) &&
                    cached->queryState == state)
                {
                    co_return MailboxWindowSummary{
                        .accountId = std::move(intent.accountId),
                        .mailboxId = std::move(intent.mailboxId),
                        .offset = intent.offset,
                        .limit = intent.limit,
                        .position = cached->position,
                        .returnedLimit = cached->returnedLimit,
                        .representativeCount = cached->emailIds.size(),
                        .total = cached->total,
                        .queryState = cached->queryState,
                    };
                }
            }
            const auto itemsResult = m_mailboxMessageReader.listMailboxMessages(
                intent.accountId, intent.mailboxId, intent.limit, intent.offset, intent.sort);
            const auto totalResult =
                m_mailboxStatisticsReader.countMailboxMessages(intent.accountId, intent.mailboxId);
            const auto* items =
                std::get_if<std::vector<javelin::jmap::cache::MessageListItem>>(&itemsResult);
            const auto* total = std::get_if<std::size_t>(&totalResult);
            if (items != nullptr && total != nullptr)
            {
                std::vector<std::string> emailIds;
                emailIds.reserve(items->size());
                for (const auto& item : *items)
                    emailIds.push_back(item.emailId);
                if (const auto error = windows.replace({
                        .accountId = intent.accountId,
                        .mailboxId = intent.mailboxId,
                        .queryKey = queryKey,
                        .requestedOffset = intent.offset,
                        .requestedLimit = intent.limit,
                        .position = intent.offset,
                        .returnedLimit = intent.limit,
                        .total = *total,
                        .queryState = state,
                        .coverage = javelin::jmap::cache::QueryWindowCoverage::Server,
                        .emailIds = std::move(emailIds),
                    }))
                {
                    co_return javelin::jmap::operationError(*error);
                }
                co_return MailboxWindowSummary{
                    .accountId = std::move(intent.accountId),
                    .mailboxId = std::move(intent.mailboxId),
                    .offset = intent.offset,
                    .limit = intent.limit,
                    .position = intent.offset,
                    .returnedLimit = intent.limit,
                    .representativeCount = items->size(),
                    .total = *total,
                    .queryState = state,
                };
            }
        }

        const ForegroundWorkScope foreground{m_workScheduler};
        auto result = co_await m_queryMaterializer.queryMailboxPage(
            toLiveConnectionSettings(*configuration), intent.accountId, intent.mailboxId,
            intent.offset, intent.limit, intent.sort, std::move(intent.anchor),
            intent.anchorOffset);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            m_errorCoordinator.reportFailure(*configuration, intent.accountId,
                                             QStringLiteral("Load mailbox messages"), *error);
            co_return *error;
        }
        m_errorCoordinator.reportSuccess(configuration->connectionId);

        auto page = std::get<javelin::jmap::MailboxPageSummary>(std::move(result));
        if (m_threadMaterializationCoordinator != nullptr)
        {
            if (const auto error = m_threadMaterializationCoordinator->enqueueMailboxWindow(
                    page.accountId, queryKey, page.offset, page.limit,
                    WorkPriority::VisibleMaterialization))
                qWarning().noquote()
                    << "Could not enqueue mailbox Thread materialization" << error->message;
        }
        MailboxWindowSummary summary{
            .accountId = page.accountId,
            .mailboxId = page.mailboxId,
            .offset = page.offset,
            .limit = page.limit,
            .position = page.position,
            .returnedLimit = page.returnedLimit,
            .representativeCount = page.representativeCount,
            .total = page.total,
            .queryState = page.queryState,
        };
        Q_EMIT cacheCommitted(MailCacheChange{
            .accountId = QString::fromStdString(page.accountId),
            .mailboxIds = {QString::fromStdString(page.mailboxId)},
            .queryWindows = {MailboxQueryWindowChange{
                .mailboxId = QString::fromStdString(page.mailboxId),
                .offset = page.offset,
                .limit = page.limit,
                .total = page.total,
            }},
            .searchWindows = {},
            .emailObjectsChanged = false,
        });
        co_return summary;
    }

    void MailQueryApplicationService::ensureThread(ThreadMaterializationIntent intent)
    {
        if (m_threadMaterializationCoordinator == nullptr || intent.accountId.empty() ||
            intent.threadId.empty())
        {
            return;
        }
        if (const auto error = m_threadMaterializationCoordinator->ensureThreads(
                std::move(intent.accountId), {std::move(intent.threadId)},
                WorkPriority::Interactive))
        {
            qWarning().noquote() << "Could not ensure Thread materialization" << error->message;
        }
    }

    QCoro::Task<SearchWindowResult>
    MailQueryApplicationService::requestSearchWindow(SearchWindowIntent intent)
    {
        const auto configuration = m_accountRuntime.connectionSettingsFor(intent.accountId);
        if (!configuration.has_value())
        {
            co_return javelin::jmap::OperationError{
                .message = accountSynchronizationNotConfigured(),
            };
        }

        const auto queryKey = intent.windowKey.empty()
                                  ? javelin::jmap::search::cacheKey(intent.criteria, intent.sort)
                                  : intent.windowKey;
        const auto leaseKey = searchWindowLeaseKey(intent.accountId, queryKey);
        if (!beginSearchWindowRequest(leaseKey))
        {
            co_return javelin::jmap::OperationError{
                .message = i18n("The search tab has been closed."),
            };
        }
        const auto requestLease =
            qScopeGuard([this, leaseKey]() { finishSearchWindowRequest(leaseKey); });
        if (intent.criteria.inMailbox.has_value())
        {
            const auto& mailboxId = *intent.criteria.inMailbox;
            const auto canonicalQueryKey = javelin::jmap::sync::mailboxQueryKey({
                .mailboxId = mailboxId,
                .sortProperty = "receivedAt",
                .isAscending = false,
                .collapseThreads = true,
            });
            const auto offlineStateResult = m_mailboxMessageReader.completeOfflineMailboxQueryState(
                intent.accountId, mailboxId, canonicalQueryKey);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&offlineStateResult))
            {
                co_return javelin::jmap::operationError(*error);
            }
            const auto& offlineState = std::get<std::optional<std::string>>(offlineStateResult);
            if (offlineState.has_value())
            {
                const auto itemsResult = m_mailboxFilterReader.listFilteredMailboxMessages(
                    intent.accountId, mailboxId, intent.criteria, intent.limit, intent.offset,
                    intent.sort);
                const auto totalResult = m_mailboxFilterReader.countFilteredMailboxMessages(
                    intent.accountId, mailboxId, intent.criteria);
                const auto* items =
                    std::get_if<std::vector<javelin::jmap::cache::MessageListItem>>(&itemsResult);
                const auto* total = std::get_if<std::size_t>(&totalResult);
                if (items == nullptr)
                    co_return javelin::jmap::operationError(
                        std::get<javelin::jmap::cache::DatabaseError>(itemsResult));
                if (total == nullptr)
                    co_return javelin::jmap::operationError(
                        std::get<javelin::jmap::cache::DatabaseError>(totalResult));

                std::vector<std::string> emailIds;
                emailIds.reserve(items->size());
                for (const auto& item : *items)
                    emailIds.push_back(item.emailId);
                javelin::jmap::cache::SearchWindowRepository searchWindows{m_databaseConnection};
                if (const auto error = searchWindows.replace({
                        .accountId = intent.accountId,
                        .queryKey = queryKey,
                        .offset = intent.offset,
                        .limit = intent.limit,
                        .position = intent.offset,
                        .returnedLimit = intent.limit,
                        .total = *total,
                        .queryState = *offlineState,
                        .emailIds = emailIds,
                    }))
                {
                    co_return javelin::jmap::operationError(*error);
                }

                Q_EMIT cacheCommitted(MailCacheChange{
                    .accountId = QString::fromStdString(intent.accountId),
                    .mailboxIds = {},
                    .queryWindows = {},
                    .searchWindows = {SearchQueryWindowChange{
                        .queryKey = QString::fromStdString(queryKey),
                        .offset = intent.offset,
                        .limit = intent.limit,
                        .total = *total,
                    }},
                    .emailObjectsChanged = false,
                });
                co_return SearchWindowSummary{
                    .accountId = std::move(intent.accountId),
                    .queryKey = queryKey,
                    .offset = intent.offset,
                    .limit = intent.limit,
                    .position = intent.offset,
                    .returnedLimit = intent.limit,
                    .representativeCount = items->size(),
                    .total = *total,
                    .queryState = *offlineState,
                };
            }
        }

        javelin::jmap::search::EmailSearchResolution resolution;
        if (intent.criteria.fromContactsOnly)
        {
            const auto contacts = m_contactReader.listEmailAddresses();
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&contacts))
                co_return javelin::jmap::operationError(*error);
            resolution.contactAddresses = std::get<std::vector<std::string>>(contacts);
        }
        if (intent.criteria.taggedOnly && intent.criteria.tags.empty())
        {
            const auto keywords = m_mailTagReader.listTagKeywords(intent.accountId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&keywords))
                co_return javelin::jmap::operationError(*error);
            resolution.userKeywords = std::get<std::vector<std::string>>(keywords);
        }

        const auto settings = *configuration;
        const ForegroundWorkScope foreground{m_workScheduler};
        auto result = co_await m_queryMaterializer.searchMessages(
            toLiveConnectionSettings(settings), intent.accountId, intent.criteria, intent.offset,
            intent.limit, intent.sort, std::move(intent.anchor), queryKey, {},
            std::move(resolution));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            m_errorCoordinator.reportFailure(settings, intent.accountId,
                                             QStringLiteral("Search messages"), *error);
            co_return *error;
        }
        m_errorCoordinator.reportSuccess(settings.connectionId);

        const auto& page = std::get<javelin::jmap::MessageSearchSummary>(result);
        if (searchWindowRetired(leaseKey))
        {
            static_cast<void>(
                javelin::jmap::cache::SearchWindowRepository{m_databaseConnection}.eraseQuery(
                    intent.accountId, queryKey));
            co_return javelin::jmap::OperationError{
                .message = i18n("The search tab has been closed."),
            };
        }
        if (m_threadMaterializationCoordinator != nullptr)
        {
            if (const auto error = m_threadMaterializationCoordinator->enqueueSearchWindow(
                    page.accountId, queryKey, page.offset, page.limit,
                    WorkPriority::VisibleMaterialization))
                qWarning().noquote()
                    << "Could not enqueue search Thread materialization" << error->message;
        }
        SearchWindowSummary summary{
            .accountId = page.accountId,
            .queryKey = queryKey,
            .offset = page.offset,
            .limit = page.limit,
            .position = page.position,
            .returnedLimit = page.returnedLimit,
            .representativeCount = page.representativeCount,
            .total = page.total,
            .queryState = page.queryState,
        };
        Q_EMIT cacheCommitted(MailCacheChange{
            .accountId = QString::fromStdString(page.accountId),
            .mailboxIds = {},
            .queryWindows = {},
            .searchWindows = {SearchQueryWindowChange{
                .queryKey = QString::fromStdString(queryKey),
                .offset = page.offset,
                .limit = page.limit,
                .total = page.total,
            }},
            .emailObjectsChanged = false,
        });
        co_return summary;
    }

    void MailQueryApplicationService::retireSearchWindow(std::string accountId,
                                                         std::string windowKey)
    {
        const auto leaseKey = searchWindowLeaseKey(accountId, windowKey);
        auto& state = m_searchWindowRequests[leaseKey];
        state.retired = true;
        static_cast<void>(
            javelin::jmap::cache::SearchWindowRepository{m_databaseConnection}.eraseQuery(
                accountId, windowKey));
        if (state.activeRequests == 0)
            m_searchWindowRequests.erase(leaseKey);
    }

    QCoro::Task<QueuedMailboxSelectionMutationResult>
    MailMutationApplicationService::queueMailboxSelectionMutation(
        MailboxSelectionMutationIntent intent)
    {
        if (intent.operation == MailboxSelectionOperation::Archive &&
            !intent.sourceMailboxId.has_value())
        {
            const auto mailboxesResult = m_mailboxReader.listMailboxTree(intent.accountId);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&mailboxesResult))
                co_return javelin::jmap::operationError(*error);
            const auto& mailboxes =
                std::get<std::vector<javelin::jmap::cache::MailboxTreeItem>>(mailboxesResult);
            const auto inbox = std::ranges::find(mailboxes, std::optional<std::string>{"inbox"},
                                                 &javelin::jmap::cache::MailboxTreeItem::role);
            if (inbox == mailboxes.end())
                co_return javelin::jmap::OperationError{.message =
                                                            i18n("No Inbox mailbox is available.")};
            intent.sourceMailboxId = inbox->id;
        }

        if (const auto error = co_await javelin::app::ensureMessageSelectionMaterialized(
                m_databaseConnection, m_threadMaterializationCoordinator, intent.accountId,
                intent.sourceMailboxId, intent.selection))
            co_return *error;
        co_return queueResolvedMailboxSelectionMutation(std::move(intent));
    }

    QueuedMailboxSelectionMutationResult
    MailMutationApplicationService::queueResolvedMailboxSelectionMutation(
        MailboxSelectionMutationIntent intent)
    {
        javelin::jmap::cache::ThreadRepository threads{m_databaseConnection};
        javelin::jmap::cache::ThreadReadRepository threadReader{m_databaseConnection};
        auto emailIdsResult = resolveMessageSelection(threadReader, threads, intent.accountId,
                                                      intent.sourceMailboxId, intent.selection);
        if (const auto* error = std::get_if<QString>(&emailIdsResult))
        {
            return javelin::jmap::OperationError{.message = *error};
        }
        auto emailIds = std::get<std::vector<std::string>>(std::move(emailIdsResult));

        const auto mailboxesResult = m_mailboxReader.listMailboxTree(intent.accountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&mailboxesResult))
        {
            return javelin::jmap::OperationError{.message = error->message};
        }

        javelin::jmap::cache::EmailRepository emailRepository{m_databaseConnection};
        std::vector<javelin::jmap::domain::Email> emails;
        emails.reserve(emailIds.size());
        for (const auto& emailId : emailIds)
        {
            const auto emailResult = emailRepository.find(intent.accountId, emailId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&emailResult))
            {
                return javelin::jmap::OperationError{.message = error->message};
            }
            const auto& email = std::get<std::optional<javelin::jmap::domain::Email>>(emailResult);
            if (!email.has_value())
            {
                return javelin::jmap::OperationError{
                    .message = i18n("Message %1 is not available in the local cache.",
                                    QString::fromStdString(emailId)),
                };
            }
            emails.push_back(*email);
        }

        auto planResult = planMailboxSelectionMutation(
            intent, emailIds, emails,
            std::get<std::vector<javelin::jmap::cache::MailboxTreeItem>>(mailboxesResult));
        if (const auto* error = std::get_if<QString>(&planResult))
        {
            return javelin::jmap::OperationError{.message = *error};
        }

        auto plan = std::get<PlannedMailboxSelectionMutation>(std::move(planResult));
        const auto queuedEmailCount = plan.mutations.size();
        if (queuedEmailCount == 0)
        {
            return QueuedMailboxSelectionMutation{
                .accountId = std::move(intent.accountId),
                .queuedEmailCount = 0,
                .skippedEmailCount = plan.skippedEmailCount,
                .queuedMutations = {},
                .historyEntryId = std::nullopt,
            };
        }

        const auto& mailboxes =
            std::get<std::vector<javelin::jmap::cache::MailboxTreeItem>>(mailboxesResult);
        const auto destination = [&mailboxes, &intent]
        {
            if (intent.destinationMailboxId.has_value())
            {
                return std::ranges::find(mailboxes, *intent.destinationMailboxId,
                                         &javelin::jmap::cache::MailboxTreeItem::id);
            }
            const auto role = intent.operation == MailboxSelectionOperation::Archive
                                  ? std::optional<std::string>{"archive"}
                              : intent.operation == MailboxSelectionOperation::Junk
                                  ? std::optional<std::string>{"junk"}
                              : intent.operation == MailboxSelectionOperation::NotJunk
                                  ? std::optional<std::string>{"inbox"}
                                  : std::optional<std::string>{std::nullopt};
            return std::ranges::find(mailboxes, role, &javelin::jmap::cache::MailboxTreeItem::role);
        }();
        QString label;
        if (intent.operation == MailboxSelectionOperation::Archive)
        {
            label = messageCountLabel(QStringLiteral("Archive"), queuedEmailCount);
        }
        else if (intent.operation == MailboxSelectionOperation::Junk)
        {
            label = messageCountLabel(QStringLiteral("Mark Junk"), queuedEmailCount);
        }
        else if (intent.operation == MailboxSelectionOperation::NotJunk)
        {
            label = messageCountLabel(QStringLiteral("Mark Not Junk"), queuedEmailCount);
        }
        else
        {
            const bool deleting = destination != mailboxes.end() &&
                                  destination->role == std::optional<std::string>{"trash"} &&
                                  intent.operation == MailboxSelectionOperation::Move;
            if (deleting)
            {
                label = messageCountLabel(QStringLiteral("Delete"), queuedEmailCount);
            }
            else
            {
                const auto verb = intent.operation == MailboxSelectionOperation::Move
                                      ? QStringLiteral("Move")
                                      : QStringLiteral("Copy");
                label = messageCountLabel(verb, queuedEmailCount);
                if (destination != mailboxes.end())
                    label += QStringLiteral(" to ") + QString::fromStdString(destination->name);
            }
        }

        const auto operationGroupId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        javelin::app::undo::MailPatchHistory history;
        history.items.reserve(plan.mutations.size());
        for (const auto& mutation : plan.mutations)
        {
            const auto email =
                std::ranges::find(emails, mutation.emailId, &javelin::jmap::domain::Email::id);
            if (email == emails.end())
            {
                return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::LocalStorageFailure,
                    .message = i18n("A planned email mutation lost its cache snapshot."),
                };
            }
            history.items.push_back(historyItem(intent.accountId, *email, mutation));
        }

        auto preparedResult = m_undoManager.prepareNormal(
            label, javelin::app::undo::HistoryDomain::Mail, history, operationGroupId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&preparedResult))
            return javelin::jmap::operationError(*error);
        auto prepared =
            std::get<std::optional<javelin::app::undo::HistoryEntry>>(std::move(preparedResult));
        if (!prepared.has_value())
        {
            return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::LocalStorageFailure,
                .message = i18n("Failed to reserve operation history."),
            };
        }

        auto mutations = plan.mutations;
        for (auto& mutation : mutations)
            mutation.operationGroupId = operationGroupId.toStdString();
        auto queuedResult = queueExactEmailMutations(intent.accountId, std::move(mutations));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&queuedResult))
        {
            if (const auto discardError = m_undoManager.discardNormal(prepared->entryId))
                return javelin::jmap::operationError(*discardError);
            return *error;
        }
        auto queuedMutations =
            std::get<std::vector<javelin::jmap::QueuedEmailMutation>>(std::move(queuedResult));
        for (std::size_t index = 0; index < queuedMutations.size(); ++index)
            std::get<javelin::app::undo::MailPatchHistory>(prepared->payload)
                .items[index]
                .mutationId = queuedMutations[index].mutationId;

        auto committedResult = m_undoManager.commitNormal(std::move(*prepared));
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&committedResult))
            return javelin::jmap::operationError(*error);
        const auto& committed = std::get<javelin::app::undo::HistoryEntry>(committedResult);
        return QueuedMailboxSelectionMutation{
            .accountId = std::move(intent.accountId),
            .queuedEmailCount = queuedEmailCount,
            .skippedEmailCount = plan.skippedEmailCount,
            .queuedMutations = std::move(queuedMutations),
            .historyEntryId = committed.entryId,
        };
    }

    QCoro::Task<QueuedMessageSelectionMutationResult>
    MailMutationApplicationService::queueDestroyMessages(std::string accountId,
                                                         std::optional<std::string> sourceMailboxId,
                                                         MessageSelection selection)
    {
        if (const auto error = co_await javelin::app::ensureMessageSelectionMaterialized(
                m_databaseConnection, m_threadMaterializationCoordinator, accountId,
                sourceMailboxId, selection))
            co_return *error;
        co_return queueSelectedMessageMutation(std::move(accountId), std::move(sourceMailboxId),
                                               std::move(selection),
                                               SelectedMessageMutation::Destroy);
    }

    QCoro::Task<QueuedMessageSelectionMutationResult>
    MailMutationApplicationService::queueMarkMessagesUnread(
        std::string accountId, std::optional<std::string> sourceMailboxId,
        MessageSelection selection)
    {
        if (const auto error = co_await javelin::app::ensureMessageSelectionMaterialized(
                m_databaseConnection, m_threadMaterializationCoordinator, accountId,
                sourceMailboxId, selection))
            co_return *error;
        co_return queueSelectedMessageMutation(std::move(accountId), std::move(sourceMailboxId),
                                               std::move(selection),
                                               SelectedMessageMutation::MarkUnread);
    }

    QueuedMessageSelectionMutationResult
    MailMutationApplicationService::queueSelectedMessageMutation(
        std::string accountId, std::optional<std::string> sourceMailboxId,
        MessageSelection selection, const SelectedMessageMutation mutation)
    {
        javelin::jmap::cache::ThreadRepository threads{m_databaseConnection};
        javelin::jmap::cache::ThreadReadRepository threadReader{m_databaseConnection};
        auto emailIdsResult =
            resolveMessageSelection(threadReader, threads, accountId, sourceMailboxId, selection);
        if (const auto* error = std::get_if<QString>(&emailIdsResult))
        {
            return javelin::jmap::OperationError{.message = *error};
        }

        auto emailIds = std::get<std::vector<std::string>>(std::move(emailIdsResult));
        javelin::jmap::cache::EmailRepository emails{m_databaseConnection};
        std::vector<javelin::jmap::domain::Email> selectedEmails;
        for (const auto& emailId : emailIds)
        {
            const auto found = emails.find(accountId, emailId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&found))
                return javelin::jmap::operationError(*error);
            const auto& email = std::get<std::optional<javelin::jmap::domain::Email>>(found);
            if (!email.has_value())
            {
                return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::NotFound,
                    .message = i18n("A selected message is not cached locally."),
                };
            }
            if (mutation == SelectedMessageMutation::MarkUnread &&
                !std::ranges::contains(email->keywords, std::string{"$seen"}))
            {
                continue;
            }
            selectedEmails.push_back(*email);
        }
        if (selectedEmails.empty())
        {
            return QueuedMessageSelectionMutation{
                .accountId = std::move(accountId),
                .queuedEmailCount = 0,
                .queuedMutations = {},
                .historyEntryId = std::nullopt,
            };
        }

        const auto operationGroupId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        std::optional<javelin::app::undo::HistoryEntry> prepared;
        if (mutation == SelectedMessageMutation::MarkUnread)
        {
            javelin::app::undo::MailPatchHistory history;
            for (const auto& email : selectedEmails)
            {
                history.items.push_back(
                    historyItem(accountId, email,
                                {
                                    .emailId = email.id,
                                    .addMailboxIds = {},
                                    .removeMailboxIds = {},
                                    .addKeywords = {},
                                    .removeKeywords = {"$seen"},
                                    .operationGroupId = operationGroupId.toStdString(),
                                    .ifInState = std::nullopt,
                                }));
            }
            auto preparedResult = m_undoManager.prepareNormal(
                messageCountLabel(QStringLiteral("Mark"), selectedEmails.size()) +
                    QStringLiteral(" Unread"),
                javelin::app::undo::HistoryDomain::Mail, std::move(history), operationGroupId);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&preparedResult))
                return javelin::jmap::operationError(*error);
            prepared = std::get<std::optional<javelin::app::undo::HistoryEntry>>(
                std::move(preparedResult));
        }
        else
        {
            const auto count = selectedEmails.size();
            const auto explanation =
                count == 1
                    ? QStringLiteral("Unable to undo permanently deleting this message.")
                    : QStringLiteral("Unable to undo permanently deleting %1 messages.").arg(count);
            auto preparedResult = m_undoManager.prepareImpossible(
                messageCountLabel(QStringLiteral("Permanently Delete"), count),
                javelin::app::undo::HistoryDomain::Mail, explanation, operationGroupId);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&preparedResult))
                return javelin::jmap::operationError(*error);
            prepared = std::get<std::optional<javelin::app::undo::HistoryEntry>>(
                std::move(preparedResult));
        }

        std::vector<javelin::jmap::EmailMailboxMutation> mutations;
        mutations.reserve(selectedEmails.size());
        for (const auto& email : selectedEmails)
        {
            mutations.push_back({
                .emailId = email.id,
                .addMailboxIds = {},
                .removeMailboxIds = {},
                .addKeywords = {},
                .removeKeywords = mutation == SelectedMessageMutation::MarkUnread
                                      ? std::vector<std::string>{"$seen"}
                                      : std::vector<std::string>{},
                .operationGroupId = operationGroupId.toStdString(),
                .ifInState = std::nullopt,
                .authoritativeMailboxIds = std::nullopt,
                .authoritativeKeywords = std::nullopt,
                .destroy = mutation == SelectedMessageMutation::Destroy,
            });
        }
        auto queuedResult = queueExactEmailMutations(accountId, std::move(mutations));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&queuedResult))
        {
            if (prepared.has_value())
            {
                if (const auto discardError = m_undoManager.discardNormal(prepared->entryId))
                    return javelin::jmap::operationError(*discardError);
            }
            return *error;
        }
        auto queuedMutations =
            std::get<std::vector<javelin::jmap::QueuedEmailMutation>>(std::move(queuedResult));
        if (mutation == SelectedMessageMutation::MarkUnread && prepared.has_value())
        {
            for (std::size_t index = 0; index < queuedMutations.size(); ++index)
                std::get<javelin::app::undo::MailPatchHistory>(prepared->payload)
                    .items[index]
                    .mutationId = queuedMutations[index].mutationId;
        }

        std::optional<QString> historyEntryId;
        if (prepared.has_value())
        {
            auto committed = mutation == SelectedMessageMutation::Destroy
                                 ? m_undoManager.commitImpossible(std::move(*prepared))
                                 : m_undoManager.commitNormal(std::move(*prepared));
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
                return javelin::jmap::operationError(*error);
            historyEntryId = std::get<javelin::app::undo::HistoryEntry>(committed).entryId;
        }

        return QueuedMessageSelectionMutation{
            .accountId = std::move(accountId),
            .queuedEmailCount = selectedEmails.size(),
            .queuedMutations = std::move(queuedMutations),
            .historyEntryId = std::move(historyEntryId),
        };
    }

    QueuedMessageSelectionMutationResult
    MailMutationApplicationService::queueMarkEmailRead(std::string accountId, std::string emailId)
    {
        javelin::jmap::cache::EmailRepository emails{m_databaseConnection};
        const auto found = emails.find(accountId, emailId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&found))
            return javelin::jmap::operationError(*error);
        const auto& email = std::get<std::optional<javelin::jmap::domain::Email>>(found);
        if (!email.has_value())
            return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::NotFound,
                .message = i18n("Message not found."),
            };
        if (std::ranges::contains(email->keywords, std::string{"$seen"}))
        {
            return QueuedMessageSelectionMutation{
                .accountId = std::move(accountId),
                .queuedEmailCount = 0,
                .queuedMutations = {},
                .historyEntryId = std::nullopt,
            };
        }

        const auto operationGroupId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        javelin::jmap::EmailMailboxMutation mutation{
            .emailId = emailId,
            .addMailboxIds = {},
            .removeMailboxIds = {},
            .addKeywords = {"$seen"},
            .removeKeywords = {},
            .operationGroupId = operationGroupId.toStdString(),
            .ifInState = std::nullopt,
        };
        javelin::app::undo::MailPatchHistory history{
            .items = {historyItem(accountId, *email, mutation)}};
        auto preparedResult = m_undoManager.prepareNormal(QStringLiteral("Mark Message Read"),
                                                          javelin::app::undo::HistoryDomain::Mail,
                                                          std::move(history), operationGroupId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&preparedResult))
            return javelin::jmap::operationError(*error);
        auto prepared =
            std::get<std::optional<javelin::app::undo::HistoryEntry>>(std::move(preparedResult));
        auto queuedResult = queueExactEmailMutation(accountId, std::move(mutation));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&queuedResult))
        {
            if (prepared.has_value())
                static_cast<void>(m_undoManager.discardNormal(prepared->entryId));
            return *error;
        }
        auto queued = std::get<javelin::jmap::QueuedEmailMutation>(std::move(queuedResult));
        std::get<javelin::app::undo::MailPatchHistory>(prepared->payload).items.front().mutationId =
            queued.mutationId;
        auto committed = m_undoManager.commitNormal(std::move(*prepared));
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
            return javelin::jmap::operationError(*error);
        return QueuedMessageSelectionMutation{
            .accountId = std::move(accountId),
            .queuedEmailCount = 1,
            .queuedMutations = {std::move(queued)},
            .historyEntryId = std::get<javelin::app::undo::HistoryEntry>(committed).entryId,
        };
    }

    QCoro::Task<QueuedMessageSelectionMutationResult>
    MailMutationApplicationService::queueSetMessagesFlagged(
        std::string accountId, std::optional<std::string> sourceMailboxId,
        MessageSelection selection, const bool flagged)
    {
        if (const auto error = co_await javelin::app::ensureMessageSelectionMaterialized(
                m_databaseConnection, m_threadMaterializationCoordinator, accountId,
                sourceMailboxId, selection))
            co_return *error;
        co_return queueSetMessagesKeyword(
            std::move(accountId), std::move(sourceMailboxId), std::move(selection), "$flagged",
            flagged, flagged ? QStringLiteral("Add Star to") : QStringLiteral("Remove Star from"),
            false);
    }

    QCoro::Task<QueuedMessageSelectionMutationResult>
    MailMutationApplicationService::queueSetMessagesTag(std::string accountId,
                                                        std::optional<std::string> sourceMailboxId,
                                                        MessageSelection selection,
                                                        std::string keyword, const bool enabled)
    {
        keyword = javelin::jmap::domain::canonicalKeyword(std::move(keyword));
        if (!javelin::jmap::domain::isValidKeyword(keyword) ||
            javelin::jmap::domain::hasStandardKeywordSemantics(keyword))
        {
            co_return javelin::jmap::OperationError{
                .message = i18n("This is not a valid user tag keyword."),
            };
        }

        if (const auto error = co_await javelin::app::ensureMessageSelectionMaterialized(
                m_databaseConnection, m_threadMaterializationCoordinator, accountId,
                sourceMailboxId, selection))
            co_return *error;
        co_return queueSetMessagesKeyword(
            std::move(accountId), std::move(sourceMailboxId), std::move(selection),
            std::move(keyword), enabled,
            enabled ? QStringLiteral("Add Tag") : QStringLiteral("Remove Tag"), true);
    }

    QueuedMessageSelectionMutationResult MailMutationApplicationService::queueSetMessagesKeyword(
        std::string accountId, std::optional<std::string> sourceMailboxId,
        MessageSelection selection, std::string keyword, const bool enabled, QString historyVerb,
        const bool appendKeywordToHistoryLabel)
    {
        javelin::jmap::cache::ThreadRepository threads{m_databaseConnection};
        javelin::jmap::cache::ThreadReadRepository threadReader{m_databaseConnection};
        auto emailIdsResult =
            resolveMessageSelection(threadReader, threads, accountId, sourceMailboxId, selection);
        if (const auto* error = std::get_if<QString>(&emailIdsResult))
            return javelin::jmap::OperationError{.message = *error};
        const auto emailIds = std::get<std::vector<std::string>>(std::move(emailIdsResult));

        const auto mailboxesResult = m_mailboxReader.listMailboxTree(accountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&mailboxesResult))
            return javelin::jmap::operationError(*error);
        const auto& mailboxes =
            std::get<std::vector<javelin::jmap::cache::MailboxTreeItem>>(mailboxesResult);

        javelin::jmap::cache::EmailRepository repository{m_databaseConnection};
        std::vector<javelin::jmap::domain::Email> emails;
        emails.reserve(emailIds.size());
        for (const auto& emailId : emailIds)
        {
            const auto result = repository.find(accountId, emailId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
                return javelin::jmap::operationError(*error);
            const auto& email = std::get<std::optional<javelin::jmap::domain::Email>>(result);
            if (!email.has_value())
            {
                return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::NotFound,
                    .message = i18n("A selected message is not cached locally."),
                };
            }
            const bool alreadyEnabled = std::ranges::contains(email->keywords, keyword);
            if (alreadyEnabled == enabled)
                continue;
            if (const auto rightsError = keywordRightsError(*email, mailboxes))
                return javelin::jmap::OperationError{.message = *rightsError};
            emails.push_back(*email);
        }

        if (emails.empty())
        {
            return QueuedMessageSelectionMutation{
                .accountId = std::move(accountId),
                .queuedEmailCount = 0,
                .queuedMutations = {},
                .historyEntryId = std::nullopt,
            };
        }

        const auto operationGroupId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        std::vector<javelin::jmap::EmailMailboxMutation> mutations;
        javelin::app::undo::MailPatchHistory history;
        mutations.reserve(emails.size());
        history.items.reserve(emails.size());
        for (const auto& email : emails)
        {
            javelin::jmap::EmailMailboxMutation mutation{
                .emailId = email.id,
                .addMailboxIds = {},
                .removeMailboxIds = {},
                .addKeywords =
                    enabled ? std::vector<std::string>{keyword} : std::vector<std::string>{},
                .removeKeywords =
                    enabled ? std::vector<std::string>{} : std::vector<std::string>{keyword},
                .operationGroupId = operationGroupId.toStdString(),
                .ifInState = std::nullopt,
            };
            history.items.push_back(historyItem(accountId, email, mutation));
            mutations.push_back(std::move(mutation));
        }

        auto historyLabel = messageCountLabel(historyVerb, emails.size());
        if (appendKeywordToHistoryLabel)
            historyLabel += QStringLiteral(" ") + QString::fromStdString(keyword);
        auto preparedResult = m_undoManager.prepareNormal(std::move(historyLabel),
                                                          javelin::app::undo::HistoryDomain::Mail,
                                                          std::move(history), operationGroupId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&preparedResult))
            return javelin::jmap::operationError(*error);
        auto prepared =
            std::get<std::optional<javelin::app::undo::HistoryEntry>>(std::move(preparedResult));
        if (!prepared.has_value())
        {
            return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::LocalStorageFailure,
                .message = i18n("Failed to reserve operation history."),
            };
        }

        auto queuedResult = queueExactEmailMutations(accountId, std::move(mutations));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&queuedResult))
        {
            if (const auto discardError = m_undoManager.discardNormal(prepared->entryId))
                return javelin::jmap::operationError(*discardError);
            return *error;
        }
        auto queuedMutations =
            std::get<std::vector<javelin::jmap::QueuedEmailMutation>>(std::move(queuedResult));
        for (std::size_t index = 0; index < queuedMutations.size(); ++index)
        {
            std::get<javelin::app::undo::MailPatchHistory>(prepared->payload)
                .items[index]
                .mutationId = queuedMutations[index].mutationId;
        }
        auto committed = m_undoManager.commitNormal(std::move(*prepared));
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
            return javelin::jmap::operationError(*error);

        return QueuedMessageSelectionMutation{
            .accountId = std::move(accountId),
            .queuedEmailCount = emails.size(),
            .queuedMutations = std::move(queuedMutations),
            .historyEntryId = std::get<javelin::app::undo::HistoryEntry>(committed).entryId,
        };
    }

    SaveMailTagDefinitionResult
    MailMutationApplicationService::saveTagDefinition(SaveMailTagDefinition definition)
    {
        const auto displayName = QString::fromStdString(definition.displayName).trimmed();
        if (displayName.isEmpty())
            return javelin::jmap::OperationError{.message = i18n("Tag names cannot be empty.")};

        std::string keyword;
        if (definition.keyword.has_value())
        {
            keyword = javelin::jmap::domain::canonicalKeyword(*definition.keyword);
            if (!javelin::jmap::domain::isValidKeyword(keyword) ||
                javelin::jmap::domain::hasStandardKeywordSemantics(keyword))
            {
                return javelin::jmap::OperationError{
                    .message = i18n("This is not a valid user tag keyword."),
                };
            }
        }
        else
        {
            const auto base = generatedTagKeyword(displayName);
            keyword = base;
            int suffix = 2;
            while (true)
            {
                QSqlQuery exists{m_databaseConnection.database()};
                exists.prepare(QStringLiteral(
                    "SELECT EXISTS(SELECT 1 FROM mail_tag_definitions WHERE account_id=:account "
                    "AND keyword=:keyword COLLATE NOCASE) OR EXISTS(SELECT 1 FROM email_keywords "
                    "WHERE account_id=:account AND keyword=:keyword COLLATE NOCASE)"));
                exists.bindValue(QStringLiteral(":account"),
                                 QString::fromStdString(definition.accountId));
                exists.bindValue(QStringLiteral(":keyword"), QString::fromStdString(keyword));
                if (!exists.exec() || !exists.next())
                {
                    return javelin::jmap::OperationError{
                        .code = javelin::jmap::OperationErrorCode::LocalStorageFailure,
                        .message = i18n("Could not check existing mail tags: %1",
                                        exists.lastError().text()),
                    };
                }
                if (!exists.value(0).toBool() &&
                    !javelin::jmap::domain::hasStandardKeywordSemantics(keyword))
                    break;
                keyword = base + "-" + std::to_string(suffix++);
            }
        }

        const javelin::jmap::cache::DatabaseWriteScope writeScope{m_databaseConnection};
        QSqlQuery order{m_databaseConnection.database()};
        order.prepare(
            QStringLiteral("SELECT COALESCE((SELECT sort_order FROM mail_tag_definitions "
                           "WHERE account_id=:account AND keyword=:keyword COLLATE NOCASE),"
                           "COALESCE(MAX(sort_order),-10)+10) FROM mail_tag_definitions "
                           "WHERE account_id=:account"));
        order.bindValue(QStringLiteral(":account"), QString::fromStdString(definition.accountId));
        order.bindValue(QStringLiteral(":keyword"), QString::fromStdString(keyword));
        if (!order.exec() || !order.next())
        {
            return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::LocalStorageFailure,
                .message = i18n("Could not determine tag order: %1", order.lastError().text()),
            };
        }
        const auto sortOrder = order.value(0).toInt();

        QSqlQuery save{m_databaseConnection.database()};
        save.prepare(QStringLiteral(
            "INSERT INTO mail_tag_definitions(account_id,keyword,display_name,color,sort_order) "
            "VALUES(:account,:keyword,:name,:color,:sort_order) ON CONFLICT(account_id,keyword) "
            "DO UPDATE SET display_name=excluded.display_name,color=excluded.color"));
        save.bindValue(QStringLiteral(":account"), QString::fromStdString(definition.accountId));
        save.bindValue(QStringLiteral(":keyword"), QString::fromStdString(keyword));
        save.bindValue(QStringLiteral(":name"), displayName);
        save.bindValue(QStringLiteral(":color"), QString::fromStdString(definition.color));
        save.bindValue(QStringLiteral(":sort_order"), sortOrder);
        if (!save.exec())
        {
            return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::LocalStorageFailure,
                .message = i18n("Could not save tag: %1", save.lastError().text()),
            };
        }

        Q_EMIT cacheCommitted(MailCacheChange{
            .accountId = QString::fromStdString(definition.accountId),
            .mailboxIds = {},
            .queryWindows = {},
            .searchWindows = {},
            .mailTagsChanged = true,
        });

        return MailTagDefinition{
            .accountId = std::move(definition.accountId),
            .keyword = std::move(keyword),
            .displayName = displayName.toStdString(),
            .color = std::move(definition.color),
            .sortOrder = sortOrder,
        };
    }

    QueuedMailTagDeletionResult MailMutationApplicationService::deleteTag(std::string accountId,
                                                                          std::string keyword)
    {
        keyword = javelin::jmap::domain::canonicalKeyword(std::move(keyword));
        if (!javelin::jmap::domain::isValidKeyword(keyword) ||
            javelin::jmap::domain::hasStandardKeywordSemantics(keyword))
        {
            return javelin::jmap::OperationError{
                .message = i18n("This is not a valid user tag keyword."),
            };
        }

        QString displayName = QString::fromStdString(keyword);
        const auto definitions = m_mailTagReader.listTagDefinitions(accountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&definitions))
            return javelin::jmap::operationError(*error);
        const auto& tags = std::get<std::vector<javelin::jmap::cache::TagDefinition>>(definitions);
        const auto found =
            std::ranges::find(tags, keyword, &javelin::jmap::cache::TagDefinition::keyword);
        if (found != tags.end())
            displayName = found->displayName;

        const auto jobId = tagDeletionJobId(accountId, keyword);
        if (const auto error = m_workScheduler.ensure(WorkSpec{
                .jobId = jobId,
                .parentJobId = std::nullopt,
                .accountId = accountId,
                .kind = WorkKind::TagDeletion,
                .priority = WorkPriority::Bulk,
                .title = i18n("Delete tag %1", displayName),
                .checkpointJson = tagDeletionCheckpoint(keyword),
                .restartCompleted = true,
            }))
        {
            return javelin::jmap::operationError(*error);
        }
        scheduleTagDeletionPump();
        return QueuedMailTagDeletion{
            .accountId = std::move(accountId),
            .keyword = std::move(keyword),
            .jobId = jobId,
        };
    }

    QCoro::Task<javelin::jmap::MailboxSubscriptionChangeResult>
    MailMutationApplicationService::setMailboxSubscribed(std::string accountId,
                                                         std::string mailboxId,
                                                         const bool subscribed)
    {
        const auto configuration = m_accountRuntime.configurationFor(accountId);
        if (!configuration.has_value())
        {
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::NotFound,
                .message = i18n("The account is not configured."),
            };
        }

        const auto publishMailboxTree = [this, accountId](const bool optimisticProjection)
        {
            Q_EMIT cacheCommitted(MailCacheChange{
                .accountId = QString::fromStdString(accountId),
                .mailboxIds = {},
                .queryWindows = {},
                .searchWindows = {},
                .mailboxTreeChanged = true,
                .emailObjectsChanged = false,
                .optimisticProjection = optimisticProjection,
            });
        };
        auto result = co_await m_mailboxMutationEngine.setSubscribed(
            toLiveConnectionSettings(configuration->second.settings), accountId,
            std::move(mailboxId), subscribed, [publishMailboxTree] { publishMailboxTree(true); });
        javelin::jmap::sync::MutationJournalRepository mutations{m_databaseConnection};
        const auto active = mutations.listActive({.accountId = accountId, .dataType = "Mailbox"});
        const bool unresolved =
            std::holds_alternative<javelin::jmap::cache::DatabaseError>(active) ||
            !std::get<std::vector<javelin::jmap::sync::MutationRecord>>(active).empty();
        publishMailboxTree(unresolved);
        m_accountRuntime.refreshAccountConfiguration(accountId);
        if (unresolved)
            scheduleMailboxMutationReconciliation(accountId);
        co_return result;
    }

    QCoro::Task<javelin::jmap::MailboxCreateResult>
    MailMutationApplicationService::createMailbox(std::string accountId, std::string name)
    {
        const auto configuration = m_accountRuntime.configurationFor(accountId);
        if (!configuration.has_value())
        {
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::NotFound,
                .message = i18n("The account is not configured."),
            };
        }

        const auto publishMailboxTree = [this, accountId](const bool optimisticProjection)
        {
            Q_EMIT cacheCommitted(MailCacheChange{
                .accountId = QString::fromStdString(accountId),
                .mailboxIds = {},
                .queryWindows = {},
                .searchWindows = {},
                .mailboxTreeChanged = true,
                .emailObjectsChanged = false,
                .optimisticProjection = optimisticProjection,
            });
        };
        auto result = co_await m_mailboxMutationEngine.create(
            toLiveConnectionSettings(configuration->second.settings), accountId, std::move(name),
            [publishMailboxTree] { publishMailboxTree(true); });
        javelin::jmap::sync::MutationJournalRepository mutations{m_databaseConnection};
        const auto active = mutations.listActive({.accountId = accountId, .dataType = "Mailbox"});
        const bool unresolved =
            std::holds_alternative<javelin::jmap::cache::DatabaseError>(active) ||
            !std::get<std::vector<javelin::jmap::sync::MutationRecord>>(active).empty();
        publishMailboxTree(unresolved);
        m_accountRuntime.refreshAccountConfiguration(accountId);
        if (unresolved)
            scheduleMailboxMutationReconciliation(accountId);
        co_return result;
    }

    QCoro::Task<javelin::jmap::MailboxDestroyResult>
    MailMutationApplicationService::destroyMailbox(std::string accountId, std::string mailboxId)
    {
        const auto configuration = m_accountRuntime.configurationFor(accountId);
        if (!configuration.has_value())
        {
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::NotFound,
                .message = i18n("The account is not configured."),
            };
        }

        const auto publishMailboxTree = [this, accountId](const bool optimisticProjection)
        {
            Q_EMIT cacheCommitted(MailCacheChange{
                .accountId = QString::fromStdString(accountId),
                .mailboxIds = {},
                .queryWindows = {},
                .searchWindows = {},
                .mailboxTreeChanged = true,
                .emailObjectsChanged = false,
                .optimisticProjection = optimisticProjection,
            });
        };
        auto result = co_await m_mailboxMutationEngine.destroy(
            toLiveConnectionSettings(configuration->second.settings), accountId,
            std::move(mailboxId), [publishMailboxTree] { publishMailboxTree(true); });
        javelin::jmap::sync::MutationJournalRepository mutations{m_databaseConnection};
        const auto active = mutations.listActive({.accountId = accountId, .dataType = "Mailbox"});
        const bool unresolved =
            std::holds_alternative<javelin::jmap::cache::DatabaseError>(active) ||
            !std::get<std::vector<javelin::jmap::sync::MutationRecord>>(active).empty();
        publishMailboxTree(unresolved);
        m_accountRuntime.refreshAccountConfiguration(accountId);
        if (unresolved)
            scheduleMailboxMutationReconciliation(accountId);
        co_return result;
    }

    javelin::jmap::QueuedEmailMutationResult
    MailMutationApplicationService::queueExactEmailMutation(
        std::string accountId, javelin::jmap::EmailMailboxMutation mutation)
    {
        auto result = queueExactEmailMutations(std::move(accountId), {std::move(mutation)});
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
            return *error;
        auto queued = std::get<std::vector<javelin::jmap::QueuedEmailMutation>>(std::move(result));
        return std::move(queued.front());
    }

    javelin::jmap::QueuedEmailMutationsResult
    MailMutationApplicationService::queueExactEmailMutations(
        std::string accountId, std::vector<javelin::jmap::EmailMailboxMutation> mutations)
    {
        QStringList affectedMailboxIds;
        const auto appendMailboxIds = [&affectedMailboxIds](const auto& mailboxIds)
        {
            for (const auto& mailboxId : mailboxIds)
            {
                const auto value = QString::fromStdString(mailboxId);
                if (!affectedMailboxIds.contains(value))
                    affectedMailboxIds.push_back(value);
            }
        };

        javelin::jmap::cache::EmailRepository emails{m_databaseConnection};
        for (const auto& mutation : mutations)
        {
            const auto found = emails.find(accountId, mutation.emailId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&found))
                return javelin::jmap::operationError(*error);
            const auto& email = std::get<std::optional<javelin::jmap::domain::Email>>(found);
            if (email.has_value())
                appendMailboxIds(email->mailboxIds);
        }

        auto result = m_emailMutationEngine.queueBatch(accountId, std::move(mutations));
        const auto* queued = std::get_if<std::vector<javelin::jmap::QueuedEmailMutation>>(&result);
        if (queued == nullptr)
            return result;

        for (const auto& mutation : *queued)
        {
            appendMailboxIds(mutation.patch.addMailboxIds);
            appendMailboxIds(mutation.patch.removeMailboxIds);
            if (mutation.patch.authoritativeMailboxIds.has_value())
                appendMailboxIds(*mutation.patch.authoritativeMailboxIds);
        }

        Q_EMIT cacheCommitted(MailCacheChange{
            .accountId = QString::fromStdString(accountId),
            .mailboxIds = std::move(affectedMailboxIds),
            .queryWindows = {},
            .searchWindows = {},
            .mailboxTreeChanged = false,
            .emailObjectsChanged = false,
            .optimisticProjection = true,
        });
        return result;
    }

    QCoro::Task<javelin::jmap::SubmittedEmailMutationsResult>
    MailMutationApplicationService::submitPendingEmailMutations(
        std::string accountId, std::optional<std::string> operationGroupId)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_accountRuntime.configurationFor(accountId);
        if (!configuration.has_value())
            co_return javelin::jmap::OperationError{
                .message = accountSynchronizationNotConfigured(),
            };
        javelin::jmap::cache::SessionRepository sessions{m_databaseConnection};
        const auto sessionResult = sessions.load(accountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&sessionResult))
            co_return javelin::jmap::operationError(*error);
        const auto& session = std::get<std::optional<javelin::jmap::api::Session>>(sessionResult);
        if (!session.has_value())
            co_return javelin::jmap::OperationError{
                .message = QStringLiteral("No cached JMAP session is available for the account."),
            };
        const auto requestLimits = javelin::jmap::api::coreRequestLimits(*session);
        if (!requestLimits.has_value())
            co_return javelin::jmap::OperationError{
                .message = QStringLiteral("The cached JMAP session has invalid request limits."),
            };
        const auto batchLimit = static_cast<std::size_t>(std::min<std::uint64_t>(
            requestLimits->maxObjectsInSet, operationGroupId.has_value()
                                                ? std::numeric_limits<std::size_t>::max()
                                                : pendingEmailMutationBatchSize));
        QStringList affectedMailboxIds;
        EmailMutationBatchSubmission groupedSubmission;
        if (operationGroupId.has_value())
        {
            EmailMutationBatchSubmitter submitter{m_emailMutationEngine};
            groupedSubmission = co_await submitter.submit(
                toLiveConnectionSettings(configuration->second.settings), accountId,
                *operationGroupId, batchLimit,
                [&]
                {
                    const auto batchMailboxIds = affectedMailboxIdsForPendingMutations(
                        m_databaseConnection, accountId, operationGroupId, batchLimit);
                    for (const auto& mailboxId : batchMailboxIds)
                        if (!affectedMailboxIds.contains(mailboxId))
                            affectedMailboxIds.push_back(mailboxId);
                });
        }
        else
        {
            affectedMailboxIds = affectedMailboxIdsForPendingMutations(
                m_databaseConnection, accountId, operationGroupId, batchLimit);
            auto single = co_await m_emailMutationEngine.submitPending(
                toLiveConnectionSettings(configuration->second.settings), accountId,
                operationGroupId, batchLimit);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&single))
                groupedSubmission.error = *error;
            else
                groupedSubmission.submitted =
                    std::get<javelin::jmap::SubmittedEmailMutations>(std::move(single));
        }
        auto& submittedAll = groupedSubmission.submitted;

        javelin::jmap::SubmittedEmailMutationsResult result =
            groupedSubmission.error.has_value()
                ? javelin::jmap::SubmittedEmailMutationsResult{*groupedSubmission.error}
                : javelin::jmap::SubmittedEmailMutationsResult{submittedAll};

        if (operationGroupId.has_value())
        {
            const auto historyEntry = std::ranges::find_if(
                m_undoManager.entries(),
                [&](const javelin::app::undo::HistoryEntry& entry)
                {
                    return entry.operationGroupId.has_value() &&
                           entry.operationGroupId->toStdString() == *operationGroupId;
                });
            if (historyEntry != m_undoManager.entries().end())
            {
                std::unordered_set<std::string> accepted;
                for (const auto& item : submittedAll.items)
                {
                    if (item.accepted)
                        accepted.insert(item.emailId);
                }
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result);
                    error != nullptr && accepted.empty())
                {
                    using enum javelin::jmap::OperationErrorCode;
                    if (error->code == NetworkUnavailable || error->code == Timeout ||
                        error->code == HttpFailure || error->code == ProtocolViolation)
                    {
                        static_cast<void>(m_undoManager.setEntryStatus(
                            historyEntry->entryId,
                            javelin::app::undo::HistoryEntryStatus::BlockedUnknown,
                            error->message));
                    }
                }
                else if (!submittedAll.items.empty())
                {
                    if (auto* history = std::get_if<javelin::app::undo::MailPatchHistory>(
                            &historyEntry->payload))
                    {
                        auto updatedEntry = *historyEntry;
                        auto& updatedHistory =
                            std::get<javelin::app::undo::MailPatchHistory>(updatedEntry.payload);
                        std::erase_if(updatedHistory.items, [&](const auto& item)
                                      { return !accepted.contains(item.emailId); });
                        if (updatedHistory.items.empty())
                            static_cast<void>(m_undoManager.forget(updatedEntry.entryId));
                        else if (updatedHistory.items.size() != history->items.size())
                            static_cast<void>(m_undoManager.replaceEntry(std::move(updatedEntry)));
                    }
                    else if (std::holds_alternative<javelin::app::undo::ImpossibleHistory>(
                                 historyEntry->payload) &&
                             submittedAll.updatedEmailCount == 0)
                    {
                        static_cast<void>(m_undoManager.forget(historyEntry->entryId));
                    }
                }
            }
        }

        auto observed =
            observeResult(m_errorCoordinator, configuration->second.settings, accountId,
                          QStringLiteral("Submit pending mail changes"), std::move(result));
        if (submittedAll.attemptedEmailCount > 0)
        {
            Q_EMIT cacheCommitted(MailCacheChange{
                .accountId = QString::fromStdString(accountId),
                .mailboxIds = std::move(affectedMailboxIds),
                .queryWindows = {},
                .searchWindows = {},
                .mailboxTreeChanged = false,
                .emailObjectsChanged = false,
                .optimisticProjection = true,
            });
        }
        co_return observed;
    }

    QCoro::Task<javelin::jmap::AuthoritativeEmailsResult>
    MailMutationApplicationService::getAuthoritativeEmails(std::string accountId,
                                                           std::vector<std::string> emailIds)
    {
        const auto configuration = m_accountRuntime.configurationFor(accountId);
        if (!configuration.has_value())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::InvalidRequest,
                .message = accountSynchronizationNotConfigured(),
            };
        co_return co_await m_emailMutationEngine.getAuthoritative(
            toLiveConnectionSettings(configuration->second.settings), std::move(accountId),
            std::move(emailIds));
    }

    javelin::jmap::AuthoritativeEmailsResult
    MailMutationApplicationService::getEffectiveEmails(const std::string_view accountId,
                                                       const std::span<const std::string> emailIds)
    {
        javelin::jmap::cache::EmailRepository emails{m_databaseConnection};
        javelin::jmap::cache::SyncStateRepository states{m_databaseConnection};
        const auto stateResult = states.find(
            {.accountId = std::string{accountId}, .objectType = "Email", .queryKey = {}});
        const auto* state =
            std::get_if<std::optional<javelin::jmap::cache::SyncStateRecord>>(&stateResult);
        if (state == nullptr)
            return javelin::jmap::operationError(
                std::get<javelin::jmap::cache::DatabaseError>(stateResult));
        if (!state->has_value())
            return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::InvalidRequest,
                .message = i18n("Email state is not available offline."),
            };
        std::vector<javelin::jmap::domain::Email> result;
        result.reserve(emailIds.size());
        for (const auto& emailId : emailIds)
        {
            const auto found = emails.find(accountId, emailId);
            const auto* email = std::get_if<std::optional<javelin::jmap::domain::Email>>(&found);
            if (email == nullptr)
                return javelin::jmap::operationError(
                    std::get<javelin::jmap::cache::DatabaseError>(found));
            if (email->has_value())
                result.push_back(**email);
        }
        return javelin::jmap::AuthoritativeEmails{
            .accountId = std::string{accountId},
            .state = state->value().stateToken,
            .emails = std::move(result),
            .notFound = {},
        };
    }

    QCoro::Task<javelin::jmap::MessageContentRefreshResult>
    MessageContentApplicationService::requestMessageContent(std::string accountId,
                                                            std::string emailId)
    {
        const auto maintenance = emailMaintenanceActive(
            m_databaseConnection, m_mailboxMaintenanceRegistry, accountId, emailId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&maintenance))
            co_return javelin::jmap::operationError(*error);
        if (std::get<bool>(maintenance))
            co_return javelin::jmap::OperationError{
                .message = i18n("The mailbox cache is being cleared."),
            };
        const auto configuration = m_accountRuntime.configurationFor(accountId);
        if (!configuration.has_value())
            co_return javelin::jmap::OperationError{
                .message = accountSynchronizationNotConfigured(),
            };
        const ForegroundWorkScope foreground{m_workScheduler};
        auto result = observeResult(m_errorCoordinator, configuration->second.settings, accountId,
                                    QStringLiteral("Load message content"),
                                    co_await m_messageContentClient.refresh(
                                        toLiveConnectionSettings(configuration->second.settings),
                                        accountId, std::move(emailId)));
        if (std::holds_alternative<javelin::jmap::MessageContentUnavailable>(result))
            static_cast<void>(m_accountRuntime.requestAccountSynchronization(accountId));
        if (const auto* summary = std::get_if<javelin::jmap::MessageContentRefreshSummary>(&result);
            summary != nullptr && !summary->usedCachedContent)
        {
            publishMessageContentCommitted(QString::fromStdString(summary->accountId),
                                           QString::fromStdString(summary->emailId));
        }
        co_return result;
    }

    QCoro::Task<javelin::jmap::AttachmentDownloadResult>
    MessageContentApplicationService::requestAttachment(std::string accountId, std::string emailId,
                                                        std::string partId)
    {
        const auto maintenance = emailMaintenanceActive(
            m_databaseConnection, m_mailboxMaintenanceRegistry, accountId, emailId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&maintenance))
            co_return javelin::jmap::operationError(*error);
        if (std::get<bool>(maintenance))
            co_return javelin::jmap::OperationError{
                .message = i18n("The mailbox cache is being cleared."),
            };
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_accountRuntime.configurationFor(accountId);
        if (!configuration.has_value())
            co_return javelin::jmap::OperationError{
                .message = accountSynchronizationNotConfigured(),
            };
        auto refreshed = observeResult(
            m_errorCoordinator, configuration->second.settings, accountId,
            QStringLiteral("Materialize attachment source"),
            co_await m_messageContentClient.refresh(
                toLiveConnectionSettings(configuration->second.settings), accountId, emailId));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&refreshed))
            co_return *error;
        if (const auto* unavailable =
                std::get_if<javelin::jmap::MessageContentUnavailable>(&refreshed))
        {
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::NotFound,
                .message = unavailable->message,
            };
        }
        if (const auto* summary =
                std::get_if<javelin::jmap::MessageContentRefreshSummary>(&refreshed);
            summary != nullptr && !summary->usedCachedContent)
        {
            publishMessageContentCommitted(QString::fromStdString(summary->accountId),
                                           QString::fromStdString(summary->emailId));
        }
        co_return observeResult(m_errorCoordinator, configuration->second.settings, accountId,
                                QStringLiteral("Download attachment"),
                                co_await m_messageContentClient.loadAttachment(
                                    accountId, std::move(emailId), std::move(partId)));
    }

    QCoro::Task<javelin::jmap::MessageSourceDownloadResult>
    MessageContentApplicationService::requestMessageSource(std::string accountId,
                                                           std::string emailId)
    {
        const auto maintenance = emailMaintenanceActive(
            m_databaseConnection, m_mailboxMaintenanceRegistry, accountId, emailId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&maintenance))
            co_return javelin::jmap::operationError(*error);
        if (std::get<bool>(maintenance))
            co_return javelin::jmap::OperationError{
                .message = i18n("The mailbox cache is being cleared."),
            };
        const ForegroundWorkScope foreground{m_workScheduler};
        co_return co_await m_messageContentClient.loadCachedSource(std::move(accountId),
                                                                   std::move(emailId));
    }

    QCoro::Task<SaveMessagesResult>
    MessageContentApplicationService::saveMessages(SaveMessagesIntent intent)
    {
        if (intent.accountId.empty() || intent.selection.empty() ||
            intent.destinationPath.isEmpty())
        {
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::PreconditionFailed,
                .message = i18n("The message save request is incomplete."),
            };
        }
        if (!QDir::isAbsolutePath(intent.destinationPath))
        {
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::PreconditionFailed,
                .message = i18n("Choose an absolute destination path for the saved message."),
            };
        }

        if (const auto error = co_await javelin::app::ensureMessageSelectionMaterialized(
                m_databaseConnection, m_threadMaterializationCoordinator, intent.accountId,
                intent.sourceMailboxId, intent.selection))
            co_return *error;

        javelin::jmap::cache::ThreadRepository threads{m_databaseConnection};
        javelin::jmap::cache::ThreadReadRepository threadReader{m_databaseConnection};
        auto resolved = resolveMessageSelection(threadReader, threads, intent.accountId,
                                                intent.sourceMailboxId, intent.selection);
        if (const auto* error = std::get_if<QString>(&resolved))
        {
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::PreconditionFailed,
                .message = *error,
            };
        }
        auto emailIds = std::get<std::vector<std::string>>(std::move(resolved));
        if (emailIds.empty())
        {
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::NotFound,
                .message = i18n("No messages are available to save."),
            };
        }
        if (intent.targetKind == MessageSaveTargetKind::SingleFile && emailIds.size() != 1)
        {
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::PreconditionFailed,
                .message = i18n("This selection contains multiple messages. Choose a directory "
                                "instead of a single file."),
            };
        }

        if (intent.targetKind == MessageSaveTargetKind::Directory)
        {
            const QDir directory{intent.destinationPath};
            if (!directory.exists())
            {
                co_return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::LocalStorageFailure,
                    .message = i18n("The selected save directory no longer exists: %1",
                                    intent.destinationPath),
                };
            }
        }
        else
        {
            const QFileInfo target{intent.destinationPath};
            if (!target.absoluteDir().exists())
            {
                co_return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::LocalStorageFailure,
                    .message = i18n("The selected save directory no longer exists: %1",
                                    target.absolutePath()),
                };
            }
        }

        const auto configuration = m_accountRuntime.configurationFor(intent.accountId);
        if (!configuration.has_value())
        {
            co_return javelin::jmap::OperationError{
                .message = accountSynchronizationNotConfigured(),
            };
        }

        const ForegroundWorkScope foreground{m_workScheduler};
        javelin::jmap::cache::EmailRepository emails{m_databaseConnection};
        RawMailMaterializer rawMailMaterializer{m_databaseConnection, m_messageContentClient};
        std::size_t savedCount = 0;
        const auto partialFailure = [&savedCount](javelin::jmap::OperationError error)
        {
            if (savedCount != 0)
            {
                error.message = i18np("One message was saved before the operation failed: %2",
                                      "%1 messages were saved before the operation failed: %2",
                                      savedCount, error.message);
            }
            return error;
        };

        for (const auto& emailId : emailIds)
        {
            const auto maintenance = emailMaintenanceActive(
                m_databaseConnection, m_mailboxMaintenanceRegistry, intent.accountId, emailId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&maintenance))
                co_return partialFailure(javelin::jmap::operationError(*error));
            if (std::get<bool>(maintenance))
            {
                co_return partialFailure(javelin::jmap::OperationError{
                    .message = i18n("The mailbox cache is being cleared."),
                });
            }

            const auto emailResult = emails.find(intent.accountId, emailId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&emailResult))
                co_return partialFailure(javelin::jmap::operationError(*error));
            const auto& email = std::get<std::optional<javelin::jmap::domain::Email>>(emailResult);
            if (!email.has_value())
            {
                co_return partialFailure(javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::NotFound,
                    .message = i18n("A selected message is no longer available."),
                });
            }

            auto materializedResult =
                observeResult(m_errorCoordinator, configuration->second.settings, intent.accountId,
                              QStringLiteral("Save message source"),
                              co_await rawMailMaterializer.materialize(
                                  toLiveConnectionSettings(configuration->second.settings),
                                  intent.accountId, emailId, email->blobId));
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&materializedResult))
                co_return partialFailure(*error);
            auto materialized = std::get<MaterializedRawMail>(std::move(materializedResult));

            auto writeFuture =
                intent.targetKind == MessageSaveTargetKind::SingleFile
                    ? QtConcurrent::run(copySavedMessageFile, materialized.filePath,
                                        intent.destinationPath)
                    : QtConcurrent::run(copySavedMessageFileExclusively, materialized.filePath,
                                        intent.destinationPath, suggestedMailSaveFileName(*email));
            const auto written = co_await qCoro(writeFuture).takeResult();
            if (!written.error.isEmpty())
            {
                co_return partialFailure(javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::LocalStorageFailure,
                    .message = i18n("Could not save %1: %2", written.path, written.error),
                });
            }
            ++savedCount;
        }

        co_return SaveMessagesSummary{
            .savedMessageCount = savedCount,
            .destinationPath = std::move(intent.destinationPath),
        };
    }

    QCoro::Task<javelin::jmap::LiveRefreshResult>
    AccountRuntimeManager::bootstrapAccount(AccountBootstrapIntent intent)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto liveSettings = toLiveConnectionSettings(intent.settings);
        auto result = co_await m_accountBootstrapClient.bootstrap(
            liveSettings, intent.settings.connectionId, {}, std::move(intent.mailboxIds));
        co_return observeResult(m_errorCoordinator, intent.settings, {},
                                QStringLiteral("Synchronize account"), std::move(result));
    }

    void ContactApplicationService::scheduleRefresh(std::string ownerAccountId)
    {
        const auto configuration = m_accountRuntime.configurationFor(ownerAccountId);
        if (!configuration.has_value())
            return;

        m_pendingContactRefreshes.insert(ownerAccountId);
        const auto jobId = contactRefreshJobId(ownerAccountId);
        if (const auto error = m_workScheduler.ensure({
                .jobId = jobId,
                .parentJobId = std::nullopt,
                .accountId = ownerAccountId,
                .kind = WorkKind::ContactRefresh,
                .priority = WorkPriority::Freshness,
                .title = i18n("Refresh contacts"),
                .checkpointJson = QStringLiteral("{}"),
                .restartCompleted = true,
            }))
        {
            qWarning().noquote() << "Could not queue contact refresh" << error->message;
            return;
        }

        const auto current = m_workScheduler.find(jobId);
        const auto* job = std::get_if<std::optional<WorkRecord>>(&current);
        if (job == nullptr || !job->has_value())
        {
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&current))
                qWarning().noquote() << "Could not inspect contact refresh" << error->message;
            return;
        }

        if (m_errorCoordinator.authenticationPaused(configuration->second.settings.connectionId,
                                                    configuration->second.settings.revision))
        {
            if ((*job)->status != WorkStatus::Paused)
            {
                WorkProgress authenticationProgress;
                authenticationProgress.detail =
                    QStringLiteral("Waiting for account authentication");
                static_cast<void>(m_workScheduler.update(jobId, WorkStatus::WaitingForAuth,
                                                         authenticationProgress,
                                                         QStringLiteral("{}")));
            }
            return;
        }

        if ((*job)->status != WorkStatus::Queued && (*job)->status != WorkStatus::Running &&
            (*job)->status != WorkStatus::Paused)
        {
            static_cast<void>(
                m_workScheduler.update(jobId, WorkStatus::Queued, {}, QStringLiteral("{}")));
        }
        scheduleRefreshPump();
    }

    void ContactApplicationService::restoreRefreshJobs()
    {
        const auto listed = m_workScheduler.list();
        const auto* jobs = std::get_if<std::vector<WorkRecord>>(&listed);
        if (jobs == nullptr)
        {
            qWarning() << "Could not restore queued contact refresh work";
            return;
        }
        for (const auto& job : *jobs)
        {
            if (job.kind != WorkKind::ContactRefresh || !job.accountId.has_value() ||
                !shouldRestoreContactRefresh(job.status) ||
                !m_accountRuntime.configurationFor(*job.accountId).has_value())
                continue;
            scheduleRefresh(*job.accountId);
        }
    }

    void ContactApplicationService::scheduleRefreshPump()
    {
        if (m_pendingContactRefreshes.empty() || m_contactRefreshPumpScheduled)
            return;
        m_contactRefreshPumpScheduled = true;
        QTimer::singleShot(0, this,
                           [this]()
                           {
                               m_contactRefreshPumpScheduled = false;
                               pumpRefreshes();
                           });
    }

    void ContactApplicationService::pumpRefreshes()
    {
        if (!m_workScheduler.mayStartBackgroundNetwork())
            return;

        const std::vector<std::string> pending{m_pendingContactRefreshes.begin(),
                                               m_pendingContactRefreshes.end()};
        for (const auto& ownerAccountId : pending)
        {
            if (m_runningContactRefreshes.contains(ownerAccountId))
                continue;
            if (!m_accountRuntime.configurationFor(ownerAccountId).has_value())
            {
                m_pendingContactRefreshes.erase(ownerAccountId);
                continue;
            }

            const auto jobId = contactRefreshJobId(ownerAccountId);
            const auto current = m_workScheduler.find(jobId);
            const auto* job = std::get_if<std::optional<WorkRecord>>(&current);
            if (job == nullptr || !job->has_value() || (*job)->status != WorkStatus::Queued)
                continue;
            if (!m_workScheduler.admit(jobId).has_value())
                continue;

            m_pendingContactRefreshes.erase(ownerAccountId);
            m_runningContactRefreshes.insert(ownerAccountId);
            auto task = runRefresh(ownerAccountId, jobId);
            QCoro::connect(std::move(task), this,
                           [this, ownerAccountId, jobId]()
                           {
                               m_workScheduler.release(jobId);
                               m_runningContactRefreshes.erase(ownerAccountId);
                               if (m_pendingContactRefreshes.contains(ownerAccountId))
                               {
                                   const auto completedJobResult = m_workScheduler.find(jobId);
                                   const auto* completedJob =
                                       std::get_if<std::optional<WorkRecord>>(&completedJobResult);
                                   if (completedJob != nullptr && completedJob->has_value() &&
                                       ((*completedJob)->status == WorkStatus::Complete ||
                                        (*completedJob)->status == WorkStatus::Failed))
                                   {
                                       static_cast<void>(m_workScheduler.update(
                                           jobId, WorkStatus::Queued, {}, QStringLiteral("{}")));
                                   }
                               }
                               scheduleRefreshPump();
                           });
        }
    }

    QCoro::Task<void> ContactApplicationService::runRefresh(std::string ownerAccountId,
                                                            std::string jobId)
    {
        WorkProgress progress;
        progress.detail = i18n("Checking for contact changes");
        static_cast<void>(
            m_workScheduler.update(jobId, WorkStatus::Running, progress, QStringLiteral("{}")));

        const auto configuration = m_accountRuntime.configurationFor(ownerAccountId);
        if (!configuration.has_value())
        {
            static_cast<void>(m_workScheduler.update(
                jobId, WorkStatus::Failed, progress, QStringLiteral("{}"),
                QStringLiteral("Account synchronization is not configured.")));
            co_return;
        }
        const auto settings = configuration->second.settings;
        auto result = observeResult(m_errorCoordinator, settings, ownerAccountId,
                                    QStringLiteral("Synchronize contacts after state change"),
                                    co_await m_contactSyncEngine.refreshAll(
                                        toLiveConnectionSettings(settings), ownerAccountId));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            WorkStatus status = WorkStatus::Failed;
            if (javelin::jmap::isAuthenticationError(*error))
                status = WorkStatus::WaitingForAuth;
            else if (javelin::jmap::isTransientError(*error))
                status = WorkStatus::WaitingForNetwork;
            progress.detail = error->message;
            static_cast<void>(m_workScheduler.update(jobId, status, progress, QStringLiteral("{}"),
                                                     error->message));

            if (status == WorkStatus::WaitingForAuth || status == WorkStatus::WaitingForNetwork)
                m_pendingContactRefreshes.insert(ownerAccountId);
            if (status == WorkStatus::WaitingForNetwork)
            {
                QTimer::singleShot(contactRefreshRetryDelay, this,
                                   [this, ownerAccountId, jobId]()
                                   {
                                       if (!m_pendingContactRefreshes.contains(ownerAccountId))
                                           return;
                                       const auto current = m_workScheduler.find(jobId);
                                       const auto* job =
                                           std::get_if<std::optional<WorkRecord>>(&current);
                                       if (job == nullptr || !job->has_value() ||
                                           (*job)->status != WorkStatus::WaitingForNetwork)
                                           return;
                                       static_cast<void>(m_workScheduler.update(
                                           jobId, WorkStatus::Queued, {}, QStringLiteral("{}")));
                                       scheduleRefreshPump();
                                   });
            }
            co_return;
        }

        const auto& summary = std::get<javelin::jmap::contacts::ContactRefreshSummary>(result);
        if (const auto historyError =
                settleReconciledHistory(m_undoManager, summary.reconciledMutations))
        {
            progress.detail = historyError->message;
            static_cast<void>(m_workScheduler.update(jobId, WorkStatus::Failed, progress,
                                                     QStringLiteral("{}"), historyError->message));
            co_return;
        }
        progress.completedUnits = summary.contactCount;
        progress.totalUnits = summary.contactCount;
        progress.detail = i18n("%1 contacts across %2 address books", summary.contactCount,
                               summary.addressBookCount);
        static_cast<void>(
            m_workScheduler.update(jobId, WorkStatus::Complete, progress, QStringLiteral("{}")));
    }

    void MailMutationApplicationService::scheduleTagDeletionPump()
    {
        if (m_tagDeletionPumpScheduled)
            return;
        m_tagDeletionPumpScheduled = true;
        QTimer::singleShot(0, this,
                           [this]()
                           {
                               m_tagDeletionPumpScheduled = false;
                               pumpTagDeletions();
                           });
    }

    void MailMutationApplicationService::pumpTagDeletions()
    {
        if (!m_workScheduler.mayStartBackgroundNetwork())
            return;
        const auto listed = m_workScheduler.list();
        const auto* jobs = std::get_if<std::vector<WorkRecord>>(&listed);
        if (jobs == nullptr)
            return;

        for (const auto& job : *jobs)
        {
            if (job.kind != WorkKind::TagDeletion || job.status != WorkStatus::Queued ||
                !job.accountId.has_value() || m_runningTagDeletions.contains(*job.accountId) ||
                !m_accountRuntime.configurationFor(*job.accountId).has_value())
                continue;
            const auto keyword = tagDeletionKeyword(job.checkpointJson);
            if (!keyword.has_value())
            {
                static_cast<void>(
                    m_workScheduler.update(job.jobId, WorkStatus::Failed, {}, job.checkpointJson,
                                           i18n("The tag deletion checkpoint is invalid.")));
                continue;
            }
            if (!m_workScheduler.admit(job.jobId).has_value())
                continue;

            m_runningTagDeletions.insert(*job.accountId);
            auto task = runTagDeletion(job.jobId, *job.accountId, *keyword);
            QCoro::connect(std::move(task), this,
                           [this, jobId = job.jobId, accountId = *job.accountId]()
                           {
                               m_workScheduler.release(jobId);
                               m_runningTagDeletions.erase(accountId);
                               scheduleTagDeletionPump();
                           });
        }
    }

    QCoro::Task<void> MailMutationApplicationService::runTagDeletion(std::string jobId,
                                                                     std::string accountId,
                                                                     std::string keyword)
    {
        constexpr std::size_t batchSize = 25;
        const auto checkpoint = tagDeletionCheckpoint(keyword);
        WorkProgress progress;
        progress.detail = i18n("Finding messages with tag %1", QString::fromStdString(keyword));
        static_cast<void>(m_workScheduler.update(jobId, WorkStatus::Running, progress, checkpoint));

        const auto fail =
            [this, &jobId, &checkpoint, &progress](const javelin::jmap::OperationError& error)
        {
            auto status = WorkStatus::Failed;
            if (javelin::jmap::isAuthenticationError(error))
                status = WorkStatus::WaitingForAuth;
            else if (javelin::jmap::isTransientError(error))
                status = WorkStatus::WaitingForNetwork;
            progress.detail = error.message;
            static_cast<void>(
                m_workScheduler.update(jobId, status, progress, checkpoint, error.message));
            return status;
        };

        const auto configuration = m_accountRuntime.configurationFor(accountId);
        if (!configuration.has_value())
        {
            static_cast<void>(m_workScheduler.update(jobId, WorkStatus::Failed, progress,
                                                     checkpoint,
                                                     accountSynchronizationNotConfigured()));
            co_return;
        }
        const auto settings = configuration->second.settings;

        while (true)
        {
            auto pending = co_await submitPendingEmailMutations(accountId, jobId);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&pending))
            {
                fail(*error);
                co_return;
            }
            const auto& submitted = std::get<javelin::jmap::SubmittedEmailMutations>(pending);
            if (submitted.failedEmailCount != 0)
            {
                static_cast<void>(m_workScheduler.update(
                    jobId, WorkStatus::Failed, progress, checkpoint,
                    i18n("The server rejected removing this tag from %1 message(s).",
                         submitted.failedEmailCount)));
                co_return;
            }

            progress.detail = i18n("Finding messages with tag %1", QString::fromStdString(keyword));
            auto queryResult = co_await m_queryClient.queryEmailIdsByKeyword(
                toLiveConnectionSettings(settings), accountId, keyword, batchSize);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&queryResult))
            {
                fail(*error);
                co_return;
            }
            const auto& page = std::get<javelin::jmap::EmailIdQueryPage>(queryResult);
            if (!progress.totalUnits.has_value() && page.total.has_value())
                progress.totalUnits = *page.total;

            if (page.emailIds.empty())
            {
                javelin::jmap::sync::EmailMutationJournal journal{m_databaseConnection};
                const auto group = journal.listForOperationGroup(accountId, jobId);
                if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&group))
                {
                    static_cast<void>(m_workScheduler.update(jobId, WorkStatus::Failed, progress,
                                                             checkpoint, error->message));
                    co_return;
                }
                const auto records =
                    std::get<std::vector<javelin::jmap::sync::EmailMutationRecord>>(group);
                for (const auto& record : records)
                {
                    if (record.status == javelin::jmap::sync::MutationStatus::Unknown)
                    {
                        if (const auto error = journal.transition(
                                record.mutationId, javelin::jmap::sync::MutationStatus::Accepted))
                        {
                            static_cast<void>(m_workScheduler.update(
                                jobId, WorkStatus::Failed, progress, checkpoint, error->message));
                            co_return;
                        }
                    }
                }
                const auto settled = journal.listForOperationGroup(accountId, jobId);
                if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&settled))
                {
                    static_cast<void>(m_workScheduler.update(jobId, WorkStatus::Failed, progress,
                                                             checkpoint, error->message));
                    co_return;
                }
                for (const auto& record :
                     std::get<std::vector<javelin::jmap::sync::EmailMutationRecord>>(settled))
                {
                    if (record.status == javelin::jmap::sync::MutationStatus::Accepted)
                        static_cast<void>(journal.remove(record.mutationId));
                }

                {
                    const javelin::jmap::cache::DatabaseWriteScope writeScope{m_databaseConnection};
                    QSqlQuery remove{m_databaseConnection.database()};
                    remove.prepare(
                        QStringLiteral("DELETE FROM mail_tag_definitions WHERE account_id=:account "
                                       "AND keyword=:keyword COLLATE NOCASE"));
                    remove.bindValue(QStringLiteral(":account"), QString::fromStdString(accountId));
                    remove.bindValue(QStringLiteral(":keyword"), QString::fromStdString(keyword));
                    if (!remove.exec())
                    {
                        static_cast<void>(
                            m_workScheduler.update(jobId, WorkStatus::Failed, progress, checkpoint,
                                                   i18n("Could not remove the tag definition: %1",
                                                        remove.lastError().text())));
                        co_return;
                    }
                }

                if (progress.totalUnits.has_value())
                    progress.completedUnits = *progress.totalUnits;
                progress.detail = i18n("Tag deleted");
                static_cast<void>(
                    m_workScheduler.update(jobId, WorkStatus::Complete, progress, checkpoint));
                Q_EMIT cacheCommitted(MailCacheChange{
                    .accountId = QString::fromStdString(accountId),
                    .mailboxIds = {},
                    .queryWindows = {},
                    .searchWindows = {},
                    .mailboxTreeChanged = false,
                    .emailObjectsChanged = false,
                    .optimisticProjection = false,
                    .mailTagsChanged = true,
                });
                co_return;
            }

            auto authoritativeResult = co_await getAuthoritativeEmails(accountId, page.emailIds);
            if (const auto* error =
                    std::get_if<javelin::jmap::OperationError>(&authoritativeResult))
            {
                fail(*error);
                co_return;
            }
            const auto& authoritative =
                std::get<javelin::jmap::AuthoritativeEmails>(authoritativeResult);
            const auto mailboxesResult = m_mailboxReader.listMailboxTree(accountId);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&mailboxesResult))
            {
                static_cast<void>(m_workScheduler.update(jobId, WorkStatus::Failed, progress,
                                                         checkpoint, error->message));
                co_return;
            }
            const auto& mailboxes =
                std::get<std::vector<javelin::jmap::cache::MailboxTreeItem>>(mailboxesResult);

            std::vector<javelin::jmap::EmailMailboxMutation> mutations;
            mutations.reserve(authoritative.emails.size());
            for (const auto& email : authoritative.emails)
            {
                if (!std::ranges::contains(email.keywords, keyword))
                    continue;
                if (const auto rightsError = keywordRightsError(email, mailboxes))
                {
                    static_cast<void>(m_workScheduler.update(jobId, WorkStatus::Failed, progress,
                                                             checkpoint, *rightsError));
                    co_return;
                }
                mutations.push_back(javelin::jmap::EmailMailboxMutation{
                    .emailId = email.id,
                    .addMailboxIds = {},
                    .removeMailboxIds = {},
                    .addKeywords = {},
                    .removeKeywords = {keyword},
                    .operationGroupId = jobId,
                    .ifInState = std::nullopt,
                    .authoritativeMailboxIds = email.mailboxIds,
                    .authoritativeKeywords = email.keywords,
                });
            }
            if (mutations.empty())
                continue;

            progress.detail = i18np("Removing tag from %1 message", "Removing tag from %1 messages",
                                    mutations.size());
            auto queued = queueExactEmailMutations(accountId, std::move(mutations));
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&queued))
            {
                fail(*error);
                co_return;
            }
            const auto batchCount =
                std::get<std::vector<javelin::jmap::QueuedEmailMutation>>(queued).size();
            progress.completedUnits += batchCount;
            static_cast<void>(
                m_workScheduler.update(jobId, WorkStatus::Running, progress, checkpoint));
        }
    }

    QCoro::Task<javelin::jmap::contacts::ContactRefreshResult>
    ContactApplicationService::requestContacts(std::string accountId)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_accountRuntime.configurationFor(accountId);
        if (!configuration.has_value())
            co_return javelin::jmap::OperationError{
                .message = accountSynchronizationNotConfigured(),
            };
        auto result = co_await m_contactSyncEngine.refreshAll(
            toLiveConnectionSettings(configuration->second.settings), accountId);
        if (const auto* summary =
                std::get_if<javelin::jmap::contacts::ContactRefreshSummary>(&result))
        {
            if (const auto historyError =
                    settleReconciledHistory(m_undoManager, summary->reconciledMutations))
                result = *historyError;
        }
        co_return observeResult(m_errorCoordinator, configuration->second.settings, accountId,
                                i18n("Synchronize contacts"), std::move(result));
    }

    QCoro::Task<javelin::jmap::calendar::CalendarRefreshResult>
    CalendarApplicationService::requestCalendarRange(
        std::string ownerAccountId, javelin::jmap::calendar::VisibleInterval interval,
        javelin::jmap::calendar::TimeZoneId displayTimeZone, const bool forceRefresh)
    {
        const auto configuration = m_accountRuntime.configurationFor(ownerAccountId);
        if (!configuration.has_value())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = accountSynchronizationNotConfigured(),
            };

        const VisibleCalendarRange requestedRange{.interval = interval,
                                                  .displayTimeZone = displayTimeZone};
        m_visibleCalendarRanges.insert_or_assign(ownerAccountId, requestedRange);
        const auto sameRange =
            [](const VisibleCalendarRange& left, const VisibleCalendarRange& right)
        {
            return left.interval.start.value == right.interval.start.value &&
                   left.interval.end.value == right.interval.end.value &&
                   left.displayTimeZone.value == right.displayTimeZone.value;
        };
        const auto stillDesired = [this, &ownerAccountId, &requestedRange, &sameRange]
        {
            const auto desired = m_visibleCalendarRanges.find(ownerAccountId);
            return desired != m_visibleCalendarRanges.end() &&
                   sameRange(desired->second, requestedRange);
        };
        if (const auto metadata = m_calendarMetadataRefreshesInFlight.find(ownerAccountId);
            metadata != m_calendarMetadataRefreshesInFlight.end())
        {
            auto future = metadata->second;
            static_cast<void>(co_await qCoro(future).result());
            if (!stillDesired())
                co_return javelin::jmap::calendar::RefreshedRange{
                    .interval = requestedRange.interval,
                    .displayTimeZone = requestedRange.displayTimeZone,
                    .accountCount = 0,
                    .eventCount = 0,
                    .calendarMetadataAuthoritative =
                        m_calendarMetadataReadyOwners.contains(ownerAccountId),
                    .reconciledMutations = {}};
            co_return co_await requestCalendarRange(std::move(ownerAccountId), std::move(interval),
                                                    std::move(displayTimeZone), forceRefresh);
        }
        if (const auto state = m_calendarStateRefreshesInFlight.find(ownerAccountId);
            state != m_calendarStateRefreshesInFlight.end())
        {
            auto future = state->second;
            static_cast<void>(co_await qCoro(future).result());
            if (!stillDesired())
                co_return javelin::jmap::calendar::RefreshedRange{
                    .interval = requestedRange.interval,
                    .displayTimeZone = requestedRange.displayTimeZone,
                    .accountCount = 0,
                    .eventCount = 0,
                    .calendarMetadataAuthoritative =
                        m_calendarMetadataReadyOwners.contains(ownerAccountId),
                    .reconciledMutations = {}};
            co_return co_await requestCalendarRange(std::move(ownerAccountId), std::move(interval),
                                                    std::move(displayTimeZone), forceRefresh);
        }
        if (const auto active = m_calendarRangeRefreshesInFlight.find(ownerAccountId);
            active != m_calendarRangeRefreshesInFlight.end())
        {
            const auto activeRange = active->second.range;
            auto future = active->second.future;
            auto activeResult = co_await qCoro(future).result();
            if (sameRange(activeRange, requestedRange))
                co_return activeResult;

            const auto desired = m_visibleCalendarRanges.find(ownerAccountId);
            if (desired == m_visibleCalendarRanges.end() ||
                !sameRange(desired->second, requestedRange))
            {
                co_return activeResult;
            }
            co_return co_await requestCalendarRange(std::move(ownerAccountId), std::move(interval),
                                                    std::move(displayTimeZone), forceRefresh);
        }

        const auto accountsResult = m_calendarReader.accounts();
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&accountsResult))
            co_return *error;
        const auto& calendarAccounts =
            std::get<std::vector<javelin::jmap::cache::CalendarAccount>>(accountsResult);
        bool cachedRangeAvailable = false;
        std::size_t cachedAccountCount = 0;
        std::size_t cachedEventCount = 0;
        for (const auto& account : calendarAccounts)
        {
            if (account.ownerAccountId != ownerAccountId)
                continue;
            ++cachedAccountCount;
            const auto loaded =
                m_calendarReader.loadCached(account.accountId, interval, displayTimeZone);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&loaded))
                co_return *error;
            const auto& window =
                std::get<std::optional<javelin::jmap::cache::CalendarWindow>>(loaded);
            if (!window.has_value())
            {
                cachedAccountCount = 0;
                break;
            }
            cachedEventCount += window->events.size();
        }
        cachedRangeAvailable = cachedAccountCount > 0;
        const bool catchUpRequired = m_calendarCatchUpRequiredOwners.contains(ownerAccountId);
        if (!forceRefresh && !catchUpRequired && cachedRangeAvailable)
        {
            co_return javelin::jmap::calendar::RefreshedRange{
                .interval = requestedRange.interval,
                .displayTimeZone = requestedRange.displayTimeZone,
                .accountCount = cachedAccountCount,
                .eventCount = cachedEventCount,
                .calendarMetadataAuthoritative =
                    m_calendarMetadataReadyOwners.contains(ownerAccountId),
                .reconciledMutations = {},
            };
        }

        const ForegroundWorkScope foreground{m_workScheduler};
        const bool refreshCalendarMetadata =
            !m_calendarMetadataReadyOwners.contains(ownerAccountId);
        const bool useIncrementalRefresh =
            cachedRangeAvailable && (forceRefresh || catchUpRequired);
        auto promise = std::make_shared<QPromise<javelin::jmap::calendar::CalendarRefreshResult>>();
        promise->start();
        auto future = promise->future();
        m_calendarRangeRefreshesInFlight.insert_or_assign(
            ownerAccountId,
            RangeRefreshFlight{.range = requestedRange, .future = future, .promise = promise});
        auto refreshTask =
            useIncrementalRefresh
                ? m_calendarSyncEngine.refreshChanged(
                      toLiveConnectionSettings(configuration->second.settings), ownerAccountId,
                      interval, displayTimeZone)
                : m_calendarSyncEngine.refresh(
                      toLiveConnectionSettings(configuration->second.settings), ownerAccountId,
                      interval, displayTimeZone, refreshCalendarMetadata);
        auto result = co_await std::move(refreshTask);

        const auto* summary = std::get_if<javelin::jmap::calendar::RefreshedRange>(&result);
        std::optional<javelin::jmap::OperationError> historySettlementError;
        if (summary != nullptr)
            historySettlementError =
                settleReconciledHistory(m_undoManager, summary->reconciledMutations);
        const bool succeeded = summary != nullptr;
        const bool metadataAuthoritative =
            summary != nullptr && summary->calendarMetadataAuthoritative;
        if (succeeded)
        {
            if (refreshCalendarMetadata || metadataAuthoritative)
            {
                if (m_calendarMetadataUsableOwners.insert(ownerAccountId).second)
                    Q_EMIT calendarMetadataReady(QString::fromStdString(ownerAccountId));
                if (metadataAuthoritative)
                    m_calendarMetadataReadyOwners.insert(ownerAccountId);
            }
            m_calendarCatchUpRequiredOwners.erase(ownerAccountId);
        }
        const bool pendingMetadataRequest =
            m_calendarMetadataRefreshPending.erase(ownerAccountId) > 0;
        const bool metadataRefreshPending =
            pendingMetadataRequest && (!succeeded || !metadataAuthoritative);

        if (summary != nullptr && summary->accountCount > 0)
        {
            Q_EMIT calendarCacheCommitted({.ownerAccountId = QString::fromStdString(ownerAccountId),
                                           .interval = summary->interval,
                                           .displayTimeZone = summary->displayTimeZone,
                                           .accountCount = summary->accountCount,
                                           .eventCount = summary->eventCount});
        }
        const bool stateRefreshPending = m_calendarStateRefreshPending.erase(ownerAccountId) > 0;
        m_calendarRangeRefreshesInFlight.erase(ownerAccountId);
        if (metadataRefreshPending)
            scheduleMetadataRefresh(ownerAccountId);
        else if (stateRefreshPending)
            scheduleRefresh(ownerAccountId);

        auto observedResult = observeResult(
            m_errorCoordinator, configuration->second.settings, ownerAccountId,
            i18n("Synchronize calendar"),
            historySettlementError.has_value()
                ? javelin::jmap::calendar::CalendarRefreshResult{*historySettlementError}
                : std::move(result));
        promise->addResult(observedResult);
        promise->finish();
        co_return observedResult;
    }

    void CalendarApplicationService::scheduleMetadataRefresh(std::string ownerAccountId)
    {
        if (ownerAccountId.empty())
            return;
        if (m_calendarRangeRefreshesInFlight.contains(ownerAccountId) ||
            m_calendarMetadataRefreshesInFlight.contains(ownerAccountId) ||
            m_calendarStateRefreshesInFlight.contains(ownerAccountId))
        {
            m_calendarMetadataRefreshPending.insert(std::move(ownerAccountId));
            return;
        }

        auto promise = std::make_shared<QPromise<bool>>();
        promise->start();
        auto future = promise->future();
        const auto key = ownerAccountId;
        if (!m_calendarMetadataRefreshesInFlight.try_emplace(key, future).second)
            return;

        auto task = requestCalendarMetadata(std::move(ownerAccountId));
        QCoro::connect(
            std::move(task), this,
            [this, key, promise](const std::variant<bool, javelin::jmap::OperationError>& result)
            {
                m_calendarMetadataRefreshesInFlight.erase(key);
                const bool configured = m_accountRuntime.configurationFor(key).has_value();
                const auto* error = std::get_if<javelin::jmap::OperationError>(&result);
                const bool authoritative = configured && error == nullptr && std::get<bool>(result);
                if (error != nullptr)
                    qWarning().noquote() << "Calendar metadata refresh failed" << error->message;
                if (configured && error == nullptr)
                {
                    m_calendarMetadataUsableOwners.insert(key);
                    Q_EMIT calendarMetadataReady(QString::fromStdString(key));
                }
                if (authoritative)
                {
                    m_calendarMetadataReadyOwners.insert(key);
                    const auto range = m_visibleCalendarRanges.find(key);
                    if (range != m_visibleCalendarRanges.end())
                    {
                        Q_EMIT calendarCacheCommitted({
                            .ownerAccountId = QString::fromStdString(key),
                            .interval = range->second.interval,
                            .displayTimeZone = range->second.displayTimeZone,
                            .accountCount = 0,
                            .eventCount = 0,
                        });
                    }
                }
                if (configured && m_calendarMetadataRefreshPending.erase(key) > 0)
                    scheduleMetadataRefresh(key);
                else if (configured && m_calendarStateRefreshPending.erase(key) > 0)
                    scheduleRefresh(key);
                else if (!configured)
                {
                    m_calendarMetadataRefreshPending.erase(key);
                    m_calendarStateRefreshPending.erase(key);
                }
                promise->addResult(authoritative);
                promise->finish();
            });
    }

    QCoro::Task<std::variant<bool, javelin::jmap::OperationError>>
    CalendarApplicationService::requestCalendarMetadata(std::string ownerAccountId)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_accountRuntime.configurationFor(ownerAccountId);
        if (!configuration.has_value())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = accountSynchronizationNotConfigured(),
            };
        auto result = co_await m_calendarSyncEngine.refreshMetadata(
            toLiveConnectionSettings(configuration->second.settings), ownerAccountId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            m_errorCoordinator.reportFailure(configuration->second.settings, ownerAccountId,
                                             QStringLiteral("Synchronize calendar metadata"),
                                             *error);
            co_return *error;
        }
        m_errorCoordinator.reportSuccess(configuration->second.settings.connectionId);
        co_return std::get<bool>(result);
    }

    void CalendarApplicationService::scheduleRefresh(std::string ownerAccountId)
    {
        if (!m_visibleCalendarRanges.contains(ownerAccountId))
            return;
        if (m_calendarRangeRefreshesInFlight.contains(ownerAccountId) ||
            m_calendarMetadataRefreshesInFlight.contains(ownerAccountId) ||
            m_calendarStateRefreshesInFlight.contains(ownerAccountId))
        {
            m_calendarStateRefreshPending.insert(std::move(ownerAccountId));
            return;
        }

        auto promise = std::make_shared<QPromise<bool>>();
        promise->start();
        auto future = promise->future();
        const auto key = ownerAccountId;
        if (!m_calendarStateRefreshesInFlight.try_emplace(key, future).second)
            return;

        auto task = requestCalendarChanges(std::move(ownerAccountId));
        QCoro::connect(
            std::move(task), this,
            [this, key, promise](const javelin::jmap::calendar::CalendarRefreshResult& result)
            {
                m_calendarStateRefreshesInFlight.erase(key);
                const bool configured = m_accountRuntime.configurationFor(key).has_value();
                const auto* summary = std::get_if<javelin::jmap::calendar::RefreshedRange>(&result);
                const bool succeeded = configured && summary != nullptr;
                const bool metadataAuthoritative =
                    summary != nullptr && summary->calendarMetadataAuthoritative;
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                    qWarning().noquote()
                        << "Calendar state-change refresh failed" << error->message;
                const bool metadataRefreshPending =
                    m_calendarMetadataRefreshPending.erase(key) > 0 &&
                    (!succeeded || !metadataAuthoritative);
                if (configured && metadataRefreshPending)
                    scheduleMetadataRefresh(key);
                else if (configured && m_calendarStateRefreshPending.erase(key) > 0)
                    scheduleRefresh(key);
                else if (!configured)
                {
                    m_calendarMetadataRefreshPending.erase(key);
                    m_calendarStateRefreshPending.erase(key);
                }
                promise->addResult(succeeded);
                promise->finish();
            });
    }

    QCoro::Task<javelin::jmap::calendar::CalendarRefreshResult>
    CalendarApplicationService::requestCalendarChanges(std::string ownerAccountId)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_accountRuntime.configurationFor(ownerAccountId);
        const auto range = m_visibleCalendarRanges.find(ownerAccountId);
        if (!configuration.has_value() || range == m_visibleCalendarRanges.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = i18n("Calendar synchronization is not configured.")};
        auto result = co_await m_calendarSyncEngine.refreshChanged(
            toLiveConnectionSettings(configuration->second.settings), ownerAccountId,
            range->second.interval, range->second.displayTimeZone);
        if (const auto* summary = std::get_if<javelin::jmap::calendar::RefreshedRange>(&result);
            summary != nullptr)
        {
            if (summary->calendarMetadataAuthoritative)
            {
                if (m_calendarMetadataUsableOwners.insert(ownerAccountId).second)
                    Q_EMIT calendarMetadataReady(QString::fromStdString(ownerAccountId));
                m_calendarMetadataReadyOwners.insert(ownerAccountId);
            }
            m_calendarCatchUpRequiredOwners.erase(ownerAccountId);
            if (summary->accountCount > 0)
                Q_EMIT calendarCacheCommitted(
                    {.ownerAccountId = QString::fromStdString(ownerAccountId),
                     .interval = summary->interval,
                     .displayTimeZone = summary->displayTimeZone,
                     .accountCount = summary->accountCount,
                     .eventCount = summary->eventCount});
            if (const auto historyError =
                    settleReconciledHistory(m_undoManager, summary->reconciledMutations))
                co_return observeResult(
                    m_errorCoordinator, configuration->second.settings, ownerAccountId,
                    i18n("Synchronize calendar changes"),
                    javelin::jmap::calendar::CalendarRefreshResult{*historyError});
        }
        co_return observeResult(m_errorCoordinator, configuration->second.settings, ownerAccountId,
                                i18n("Synchronize calendar changes"), std::move(result));
    }

    QCoro::Task<javelin::jmap::calendar::AuthoritativeCalendarEventResult>
    CalendarApplicationService::getAuthoritativeCalendarEvent(std::string ownerAccountId,
                                                              std::string accountId,
                                                              std::optional<std::string> eventId,
                                                              std::string uid)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_accountRuntime.configurationFor(ownerAccountId);
        if (!configuration.has_value())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = accountSynchronizationNotConfigured(),
            };
        co_return co_await m_calendarProtocolClient.getAuthoritativeEvent(
            toLiveConnectionSettings(configuration->second.settings), std::move(ownerAccountId),
            std::move(accountId), std::move(eventId), std::move(uid));
    }

    javelin::jmap::calendar::AuthoritativeCalendarEventResult
    CalendarApplicationService::getEffectiveCalendarEvent(const std::string_view accountId,
                                                          const std::optional<std::string>& eventId)
    {
        javelin::jmap::cache::CalendarRepository repository{m_databaseConnection};
        const auto stateResult = repository.stateToken(accountId, "CalendarEvent");
        const auto* state = std::get_if<std::optional<std::string>>(&stateResult);
        if (state == nullptr)
            return javelin::jmap::operationError(
                std::get<javelin::jmap::cache::DatabaseError>(stateResult));
        if (!state->has_value())
            return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::InvalidRequest,
                .message = i18n("Calendar event state is not available offline."),
            };
        std::optional<javelin::jmap::calendar::CalendarEvent> event;
        if (eventId.has_value())
        {
            const auto found = repository.findEvent(accountId, *eventId);
            const auto* cached =
                std::get_if<std::optional<javelin::jmap::calendar::CalendarEvent>>(&found);
            if (cached == nullptr)
                return javelin::jmap::operationError(
                    std::get<javelin::jmap::cache::DatabaseError>(found));
            event = *cached;
        }
        return javelin::jmap::calendar::AuthoritativeCalendarEvent{
            .state = **state,
            .event = std::move(event),
        };
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    CalendarApplicationService::createCalendarEvent(
        std::string ownerAccountId, javelin::jmap::calendar::CreateEventCommand command,
        const javelin::app::undo::CommandOrigin origin)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_accountRuntime.configurationFor(ownerAccountId);
        if (!configuration.has_value())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = accountSynchronizationNotConfigured(),
            };
        if (command.event.uid.empty())
            command.event.uid = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        const auto afterDocument =
            javelin::jmap::api::serializeCalendarEventDocument(command.event);
        if (!afterDocument.has_value())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::InvalidRequest,
                .message = i18n("Unable to serialize the calendar event history."),
            };
        if (!command.operationGroupId.has_value())
            command.operationGroupId =
                QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        const auto calendar = std::ranges::find_if(command.event.calendarIds,
                                                   [](const auto& value) { return value.second; });
        auto preparedResult = m_undoManager.prepareNormal(
            i18n("Create “%1”", QString::fromStdString(command.event.title)),
            javelin::app::undo::HistoryDomain::Calendar,
            javelin::app::undo::CalendarEventHistory{
                .connectionId = ownerAccountId,
                .accountId = command.accountId,
                .calendarId =
                    calendar == command.event.calendarIds.end() ? std::string{} : calendar->first,
                .currentEventId = std::nullopt,
                .uid = command.event.uid,
                .beforeDocumentJson = std::nullopt,
                .afterDocumentJson = afterDocument,
            },
            QString::fromStdString(*command.operationGroupId), std::nullopt, origin);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&preparedResult))
            co_return javelin::jmap::operationError(*error);
        auto prepared =
            std::get<std::optional<javelin::app::undo::HistoryEntry>>(std::move(preparedResult));
        const auto projectionCommitted = [this, ownerAccountId]
        {
            const auto range = m_visibleCalendarRanges.find(ownerAccountId);
            if (range == m_visibleCalendarRanges.end())
                return;
            Q_EMIT calendarCacheCommitted({.ownerAccountId = QString::fromStdString(ownerAccountId),
                                           .interval = range->second.interval,
                                           .displayTimeZone = range->second.displayTimeZone,
                                           .accountCount = 1,
                                           .eventCount = 0});
        };
        auto result = co_await m_calendarMutationEngine.create(
            toLiveConnectionSettings(configuration->second.settings), ownerAccountId,
            std::move(command), projectionCommitted);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            if (const auto historyError = retainUnknownOrDiscard(m_undoManager, prepared, *error))
                co_return *historyError;
        }
        else if (prepared.has_value())
        {
            auto& history = std::get<javelin::app::undo::CalendarEventHistory>(prepared->payload);
            const auto& committedMutation =
                std::get<javelin::jmap::calendar::CommittedMutation>(result);
            history.currentEventId = committedMutation.createdId;
            if (!history.currentEventId.has_value())
            {
                static_cast<void>(m_undoManager.discardNormal(prepared->entryId));
                co_return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::ProtocolViolation,
                    .message = i18n("The created calendar event has no server ID."),
                };
            }
            const auto accepted =
                m_calendarReader.event(history.accountId, *history.currentEventId);
            if (const auto* event =
                    std::get_if<std::optional<javelin::jmap::calendar::CalendarEvent>>(&accepted);
                event != nullptr && event->has_value())
            {
                const auto acceptedDocument =
                    javelin::jmap::api::serializeCalendarEventDocument(**event);
                if (acceptedDocument.has_value())
                    history.afterDocumentJson = acceptedDocument;
            }
            auto committed = m_undoManager.commitNormal(std::move(*prepared));
            if (const auto* historyError =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
                co_return javelin::jmap::operationError(*historyError);
        }
        co_return observeResult(m_errorCoordinator, configuration->second.settings, ownerAccountId,
                                i18n("Create calendar event"), std::move(result));
    }

    javelin::jmap::calendar::CalendarPreferenceResult
    CalendarApplicationService::setCalendarVisible(std::string accountId, std::string calendarId,
                                                   const bool visible,
                                                   const javelin::app::undo::CommandOrigin origin)
    {
        const auto listed = m_calendarReader.calendars(accountId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&listed))
            return *error;
        const auto& calendars = std::get<std::vector<javelin::jmap::calendar::Calendar>>(listed);
        const auto calendar =
            std::ranges::find(calendars, calendarId, &javelin::jmap::calendar::Calendar::id);
        if (calendar == calendars.end())
            return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::NotFound,
                .message = i18n("The calendar is no longer available."),
            };
        if (calendar->isVisible == visible)
            return std::monostate{};
        std::string ownerAccountId;
        if (const auto accountResult = m_calendarReader.accounts();
            const auto* accounts =
                std::get_if<std::vector<javelin::jmap::cache::CalendarAccount>>(&accountResult))
        {
            const auto account = std::ranges::find(
                *accounts, accountId, &javelin::jmap::cache::CalendarAccount::accountId);
            if (account != accounts->end())
                ownerAccountId = account->ownerAccountId;
        }
        auto preparedResult =
            m_undoManager.prepareNormal(visible ? i18n("Show Calendar") : i18n("Hide Calendar"),
                                        javelin::app::undo::HistoryDomain::LocalPreference,
                                        javelin::app::undo::CalendarPreferenceHistory{
                                            .connectionId = {},
                                            .accountId = accountId,
                                            .preferenceKind = "visibility",
                                            .objectId = calendarId,
                                            .beforeValue = calendar->isVisible ? "true" : "false",
                                            .afterValue = visible ? "true" : "false",
                                        },
                                        std::nullopt, std::nullopt, origin);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&preparedResult))
            return javelin::jmap::operationError(*error);
        auto prepared =
            std::get<std::optional<javelin::app::undo::HistoryEntry>>(std::move(preparedResult));
        javelin::jmap::cache::CalendarRepository repository{m_databaseConnection};
        if (const auto cacheError = repository.setCalendarVisible(accountId, calendarId, visible))
        {
            if (prepared.has_value())
                static_cast<void>(m_undoManager.discardNormal(prepared->entryId));
            return javelin::jmap::operationError(*cacheError);
        }
        if (prepared.has_value())
        {
            auto committed = m_undoManager.commitNormal(std::move(*prepared));
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
                return javelin::jmap::operationError(*error);
        }
        Q_EMIT calendarCacheCommitted({.ownerAccountId = QString::fromStdString(ownerAccountId),
                                       .interval = {},
                                       .displayTimeZone = {},
                                       .accountCount = 1,
                                       .eventCount = 0});
        return std::monostate{};
    }

    std::variant<std::optional<std::string>, javelin::jmap::OperationError>
    CalendarApplicationService::currentCalendarPreference(
        const javelin::app::undo::CalendarPreferenceHistory& history) const
    {
        const auto listed = m_calendarReader.calendars(history.accountId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&listed))
            return *error;
        const auto& calendars = std::get<std::vector<javelin::jmap::calendar::Calendar>>(listed);
        if (history.preferenceKind == "default_calendar")
        {
            const auto current =
                std::ranges::find(calendars, true, &javelin::jmap::calendar::Calendar::isDefault);
            return current == calendars.end() ? std::optional<std::string>{std::nullopt}
                                              : std::optional<std::string>{current->id};
        }
        if (history.preferenceKind == "visibility" || history.preferenceKind == "subscription")
        {
            const auto calendar = std::ranges::find(calendars, history.objectId,
                                                    &javelin::jmap::calendar::Calendar::id);
            if (calendar == calendars.end())
                return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::NotFound,
                    .message = i18n("The calendar is no longer available."),
                };
            const bool enabled = history.preferenceKind == "visibility" ? calendar->isVisible
                                                                        : calendar->isSubscribed;
            return std::optional<std::string>{enabled ? "true" : "false"};
        }
        return javelin::jmap::OperationError{
            .code = javelin::jmap::OperationErrorCode::InvalidRequest,
            .message = i18n("Unknown calendar preference history."),
        };
    }

    QCoro::Task<std::optional<javelin::jmap::OperationError>>
    CalendarApplicationService::applyCalendarPreference(
        javelin::app::undo::CalendarPreferenceHistory history, std::optional<std::string> value,
        const javelin::app::undo::CommandOrigin origin)
    {
        if (history.preferenceKind == "visibility")
        {
            if (!value.has_value() || (*value != "true" && *value != "false"))
                co_return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::InvalidRequest,
                    .message = i18n("The calendar visibility history is invalid."),
                };
            auto result =
                setCalendarVisible(history.accountId, history.objectId, *value == "true", origin);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                co_return *error;
            co_return std::nullopt;
        }
        if (history.preferenceKind == "subscription")
        {
            if (!value.has_value() || (*value != "true" && *value != "false"))
                co_return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::InvalidRequest,
                    .message = i18n("The calendar subscription history is invalid."),
                };
            auto result =
                co_await setCalendarSubscribed(history.connectionId, history.accountId,
                                               history.objectId, *value == "true", origin);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                co_return *error;
            co_return std::nullopt;
        }
        if (history.preferenceKind == "default_calendar" && value.has_value())
        {
            auto result = co_await setDefaultCalendar(history.connectionId, history.accountId,
                                                      *value, origin);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                co_return *error;
            co_return std::nullopt;
        }
        co_return javelin::jmap::OperationError{
            .code = javelin::jmap::OperationErrorCode::InvalidRequest,
            .message = i18n("The calendar preference history is invalid."),
        };
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    CalendarApplicationService::setCalendarSubscribed(
        std::string ownerAccountId, std::string accountId, std::string calendarId,
        const bool subscribed, const javelin::app::undo::CommandOrigin origin)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_accountRuntime.configurationFor(ownerAccountId);
        if (!configuration.has_value())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = accountSynchronizationNotConfigured(),
            };
        const auto listed = m_calendarReader.calendars(accountId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&listed))
            co_return *error;
        const auto& calendars = std::get<std::vector<javelin::jmap::calendar::Calendar>>(listed);
        const auto calendar =
            std::ranges::find(calendars, calendarId, &javelin::jmap::calendar::Calendar::id);
        if (calendar == calendars.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::NotFound,
                .message = i18n("The calendar is no longer available."),
            };
        if (calendar->isSubscribed == subscribed)
            co_return javelin::jmap::calendar::CommittedMutation{
                .accountId = accountId,
                .newState = {},
                .createdId = std::nullopt,
                .receipt = {},
            };

        auto preparedResult = m_undoManager.prepareNormal(
            subscribed ? i18n("Subscribe to Calendar") : i18n("Unsubscribe from Calendar"),
            javelin::app::undo::HistoryDomain::LocalPreference,
            javelin::app::undo::CalendarPreferenceHistory{
                .connectionId = ownerAccountId,
                .accountId = accountId,
                .preferenceKind = "subscription",
                .objectId = calendarId,
                .beforeValue = calendar->isSubscribed ? "true" : "false",
                .afterValue = subscribed ? "true" : "false",
            },
            std::nullopt, std::nullopt, origin);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&preparedResult))
            co_return javelin::jmap::operationError(*error);
        auto prepared =
            std::get<std::optional<javelin::app::undo::HistoryEntry>>(std::move(preparedResult));
        auto result = co_await m_calendarMutationEngine.setCalendarSubscribed(
            toLiveConnectionSettings(configuration->second.settings), ownerAccountId, accountId,
            std::move(calendarId), subscribed);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            if (const auto historyError = retainUnknownOrDiscard(m_undoManager, prepared, *error))
                co_return *historyError;
        }
        else if (prepared.has_value())
        {
            auto committed = m_undoManager.commitNormal(std::move(*prepared));
            if (const auto* historyError =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
                co_return javelin::jmap::operationError(*historyError);
        }
        if (std::holds_alternative<javelin::jmap::calendar::CommittedMutation>(result))
        {
            const auto range = m_visibleCalendarRanges.find(ownerAccountId);
            if (range != m_visibleCalendarRanges.end())
                Q_EMIT calendarCacheCommitted(
                    {.ownerAccountId = QString::fromStdString(ownerAccountId),
                     .interval = range->second.interval,
                     .displayTimeZone = range->second.displayTimeZone,
                     .accountCount = 1,
                     .eventCount = 0});
        }
        co_return observeResult(m_errorCoordinator, configuration->second.settings, ownerAccountId,
                                i18n("Change calendar subscription"), std::move(result));
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    CalendarApplicationService::setDefaultCalendar(std::string ownerAccountId,
                                                   std::string accountId, std::string calendarId,
                                                   const javelin::app::undo::CommandOrigin origin)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_accountRuntime.configurationFor(ownerAccountId);
        if (!configuration.has_value())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = accountSynchronizationNotConfigured(),
            };
        const auto listed = m_calendarReader.calendars(accountId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&listed))
            co_return *error;
        const auto& calendars = std::get<std::vector<javelin::jmap::calendar::Calendar>>(listed);
        const auto current =
            std::ranges::find(calendars, true, &javelin::jmap::calendar::Calendar::isDefault);
        const std::optional<std::string> before =
            current == calendars.end() ? std::nullopt : std::optional{current->id};
        if (before == std::optional{calendarId})
            co_return javelin::jmap::calendar::CommittedMutation{
                .accountId = accountId,
                .newState = {},
                .createdId = std::nullopt,
                .receipt = {},
            };
        auto preparedResult = m_undoManager.prepareNormal(
            i18n("Change Default Calendar"), javelin::app::undo::HistoryDomain::LocalPreference,
            javelin::app::undo::CalendarPreferenceHistory{
                .connectionId = ownerAccountId,
                .accountId = accountId,
                .preferenceKind = "default_calendar",
                .objectId = "default",
                .beforeValue = before,
                .afterValue = calendarId,
            },
            std::nullopt, std::nullopt, origin);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&preparedResult))
            co_return javelin::jmap::operationError(*error);
        auto prepared =
            std::get<std::optional<javelin::app::undo::HistoryEntry>>(std::move(preparedResult));
        auto result = co_await m_calendarMutationEngine.setDefaultCalendar(
            toLiveConnectionSettings(configuration->second.settings), ownerAccountId, accountId,
            std::move(calendarId));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            if (const auto historyError = retainUnknownOrDiscard(m_undoManager, prepared, *error))
                co_return *historyError;
        }
        else if (prepared.has_value())
        {
            auto committed = m_undoManager.commitNormal(std::move(*prepared));
            if (const auto* historyError =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
                co_return javelin::jmap::operationError(*historyError);
        }
        if (std::holds_alternative<javelin::jmap::calendar::CommittedMutation>(result))
        {
            const auto range = m_visibleCalendarRanges.find(ownerAccountId);
            if (range != m_visibleCalendarRanges.end())
                Q_EMIT calendarCacheCommitted(
                    {.ownerAccountId = QString::fromStdString(ownerAccountId),
                     .interval = range->second.interval,
                     .displayTimeZone = range->second.displayTimeZone,
                     .accountCount = 1,
                     .eventCount = 0});
        }
        co_return observeResult(m_errorCoordinator, configuration->second.settings, ownerAccountId,
                                i18n("Change default calendar"), std::move(result));
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    CalendarApplicationService::setCalendarColor(std::string ownerAccountId, std::string accountId,
                                                 std::string calendarId,
                                                 std::optional<std::string> color)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_accountRuntime.configurationFor(ownerAccountId);
        if (!configuration.has_value())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = accountSynchronizationNotConfigured(),
            };
        auto result = co_await m_calendarMutationEngine.setCalendarColor(
            toLiveConnectionSettings(configuration->second.settings), ownerAccountId,
            std::move(accountId), std::move(calendarId), std::move(color));
        if (std::holds_alternative<javelin::jmap::calendar::CommittedMutation>(result))
        {
            const auto range = m_visibleCalendarRanges.find(ownerAccountId);
            if (range != m_visibleCalendarRanges.end())
                Q_EMIT calendarCacheCommitted(
                    {.ownerAccountId = QString::fromStdString(ownerAccountId),
                     .interval = range->second.interval,
                     .displayTimeZone = range->second.displayTimeZone,
                     .accountCount = 1,
                     .eventCount = 0});
            else
                Q_EMIT calendarCacheCommitted(
                    {.ownerAccountId = QString::fromStdString(ownerAccountId),
                     .interval = {},
                     .displayTimeZone = {},
                     .accountCount = 1,
                     .eventCount = 0});
        }
        co_return observeResult(m_errorCoordinator, configuration->second.settings, ownerAccountId,
                                i18n("Change calendar color"), std::move(result));
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    CalendarApplicationService::setCalendarDefaultAlerts(
        std::string ownerAccountId, std::string accountId, std::string calendarId,
        std::unordered_map<std::string, javelin::jmap::calendar::Alert> withTime,
        std::unordered_map<std::string, javelin::jmap::calendar::Alert> withoutTime)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_accountRuntime.configurationFor(ownerAccountId);
        if (!configuration.has_value())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = accountSynchronizationNotConfigured(),
            };
        auto result = co_await m_calendarMutationEngine.setCalendarDefaultAlerts(
            toLiveConnectionSettings(configuration->second.settings), ownerAccountId,
            std::move(accountId), std::move(calendarId), std::move(withTime),
            std::move(withoutTime));
        if (const auto* mutationError = std::get_if<javelin::jmap::OperationError>(&result);
            mutationError != nullptr && mutationError->outcomeUnknown)
        {
            requireCatchUp(ownerAccountId);
        }
        else if (std::holds_alternative<javelin::jmap::calendar::CommittedMutation>(result))
        {
            const auto range = m_visibleCalendarRanges.find(ownerAccountId);
            if (range != m_visibleCalendarRanges.end())
                Q_EMIT calendarCacheCommitted(
                    {.ownerAccountId = QString::fromStdString(ownerAccountId),
                     .interval = range->second.interval,
                     .displayTimeZone = range->second.displayTimeZone,
                     .accountCount = 1,
                     .eventCount = 0});
            else
                Q_EMIT calendarCacheCommitted(
                    {.ownerAccountId = QString::fromStdString(ownerAccountId),
                     .interval = {},
                     .displayTimeZone = {},
                     .accountCount = 1,
                     .eventCount = 0});
        }
        co_return observeResult(m_errorCoordinator, configuration->second.settings, ownerAccountId,
                                i18n("Change calendar default notifications"), std::move(result));
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    CalendarApplicationService::createCalendar(
        std::string ownerAccountId, javelin::jmap::calendar::CreateCalendarCommand command)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_accountRuntime.configurationFor(ownerAccountId);
        if (!configuration.has_value())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = accountSynchronizationNotConfigured(),
            };
        auto result = co_await m_calendarMutationEngine.createCalendar(
            toLiveConnectionSettings(configuration->second.settings), ownerAccountId,
            std::move(command));
        Q_EMIT calendarCacheCommitted({.ownerAccountId = QString::fromStdString(ownerAccountId),
                                       .interval = {},
                                       .displayTimeZone = {},
                                       .accountCount = 1,
                                       .eventCount = 0});
        co_return observeResult(m_errorCoordinator, configuration->second.settings, ownerAccountId,
                                i18n("Create calendar"), std::move(result));
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    CalendarApplicationService::deleteCalendar(
        std::string ownerAccountId, javelin::jmap::calendar::DeleteCalendarCommand command)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_accountRuntime.configurationFor(ownerAccountId);
        if (!configuration.has_value())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = accountSynchronizationNotConfigured(),
            };
        auto result = co_await m_calendarMutationEngine.deleteCalendar(
            toLiveConnectionSettings(configuration->second.settings), ownerAccountId,
            std::move(command));
        Q_EMIT calendarCacheCommitted({.ownerAccountId = QString::fromStdString(ownerAccountId),
                                       .interval = {},
                                       .displayTimeZone = {},
                                       .accountCount = 1,
                                       .eventCount = 0});
        co_return observeResult(m_errorCoordinator, configuration->second.settings, ownerAccountId,
                                i18n("Delete calendar"), std::move(result));
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    CalendarApplicationService::updateCalendarEvent(
        std::string ownerAccountId, javelin::jmap::calendar::UpdateEventCommand command,
        const javelin::app::undo::CommandOrigin origin)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_accountRuntime.configurationFor(ownerAccountId);
        if (!configuration.has_value())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = accountSynchronizationNotConfigured(),
            };
        javelin::jmap::cache::CalendarRepository repository{m_databaseConnection};
        const auto cached = repository.findEvent(command.accountId, command.event.id);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&cached))
            co_return javelin::jmap::operationError(*error);
        const auto& before =
            std::get<std::optional<javelin::jmap::calendar::CalendarEvent>>(cached);
        if (!before.has_value())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::NotFound,
                .message = i18n("The calendar event is no longer in the cache."),
            };
        if (*before == command.event)
            co_return javelin::jmap::calendar::CommittedMutation{
                .accountId = command.accountId,
                .newState = command.ifInState.value_or(std::string{}),
                .createdId = std::nullopt,
                .receipt = {},
            };
        const auto beforeDocument = javelin::jmap::api::serializeCalendarEventDocument(*before);
        const auto afterDocument =
            javelin::jmap::api::serializeCalendarEventDocument(command.event);
        if (!beforeDocument.has_value() || !afterDocument.has_value())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::InvalidRequest,
                .message = i18n("Unable to serialize the calendar event history."),
            };
        if (!command.operationGroupId.has_value())
            command.operationGroupId =
                QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        const auto calendar = std::ranges::find_if(command.event.calendarIds,
                                                   [](const auto& value) { return value.second; });
        auto preparedResult = m_undoManager.prepareNormal(
            i18n("Edit “%1”", QString::fromStdString(command.event.title)),
            javelin::app::undo::HistoryDomain::Calendar,
            javelin::app::undo::CalendarEventHistory{
                .connectionId = ownerAccountId,
                .accountId = command.accountId,
                .calendarId =
                    calendar == command.event.calendarIds.end() ? std::string{} : calendar->first,
                .currentEventId = command.event.id,
                .uid = command.event.uid,
                .beforeDocumentJson = beforeDocument,
                .afterDocumentJson = afterDocument,
            },
            QString::fromStdString(*command.operationGroupId), std::nullopt, origin);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&preparedResult))
            co_return javelin::jmap::operationError(*error);
        auto prepared =
            std::get<std::optional<javelin::app::undo::HistoryEntry>>(std::move(preparedResult));
        const auto projectionCommitted = [this, ownerAccountId]
        {
            const auto range = m_visibleCalendarRanges.find(ownerAccountId);
            if (range == m_visibleCalendarRanges.end())
                return;
            Q_EMIT calendarCacheCommitted({.ownerAccountId = QString::fromStdString(ownerAccountId),
                                           .interval = range->second.interval,
                                           .displayTimeZone = range->second.displayTimeZone,
                                           .accountCount = 1,
                                           .eventCount = 0});
        };
        auto result = co_await m_calendarMutationEngine.update(
            toLiveConnectionSettings(configuration->second.settings), ownerAccountId,
            std::move(command), projectionCommitted);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            if (const auto historyError = retainUnknownOrDiscard(m_undoManager, prepared, *error))
                co_return *historyError;
        }
        else if (prepared.has_value())
        {
            auto committed = m_undoManager.commitNormal(std::move(*prepared));
            if (const auto* historyError =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
                co_return javelin::jmap::operationError(*historyError);
        }
        co_return observeResult(m_errorCoordinator, configuration->second.settings, ownerAccountId,
                                i18n("Update calendar event"), std::move(result));
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    CalendarApplicationService::respondToCalendarEvent(
        std::string ownerAccountId, javelin::jmap::calendar::RespondToEventCommand command)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_accountRuntime.configurationFor(ownerAccountId);
        if (!configuration.has_value())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = accountSynchronizationNotConfigured(),
            };
        const auto projectionCommitted = [this, ownerAccountId]
        {
            const auto range = m_visibleCalendarRanges.find(ownerAccountId);
            if (range == m_visibleCalendarRanges.end())
                return;
            Q_EMIT calendarCacheCommitted({.ownerAccountId = QString::fromStdString(ownerAccountId),
                                           .interval = range->second.interval,
                                           .displayTimeZone = range->second.displayTimeZone,
                                           .accountCount = 1,
                                           .eventCount = 0});
        };
        auto result = co_await m_calendarMutationEngine.respond(
            toLiveConnectionSettings(configuration->second.settings), ownerAccountId,
            std::move(command), projectionCommitted);
        co_return observeResult(m_errorCoordinator, configuration->second.settings, ownerAccountId,
                                i18n("Respond to calendar invitation"), std::move(result));
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    CalendarApplicationService::deleteCalendarEvent(
        std::string ownerAccountId, javelin::jmap::calendar::DeleteEventCommand command,
        const javelin::app::undo::CommandOrigin origin)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_accountRuntime.configurationFor(ownerAccountId);
        if (!configuration.has_value())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = accountSynchronizationNotConfigured(),
            };
        javelin::jmap::cache::CalendarRepository repository{m_databaseConnection};
        const auto cached = repository.findEvent(command.accountId, command.eventId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&cached))
            co_return javelin::jmap::operationError(*error);
        const auto& before =
            std::get<std::optional<javelin::jmap::calendar::CalendarEvent>>(cached);
        if (!before.has_value())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::NotFound,
                .message = i18n("The calendar event is no longer in the cache."),
            };
        const auto beforeDocument = javelin::jmap::api::serializeCalendarEventDocument(*before);
        if (!beforeDocument.has_value())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::InvalidRequest,
                .message = i18n("Unable to serialize the calendar event history."),
            };
        if (!command.operationGroupId.has_value())
            command.operationGroupId =
                QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        const auto calendar = std::ranges::find_if(before->calendarIds,
                                                   [](const auto& value) { return value.second; });
        auto preparedResult = m_undoManager.prepareNormal(
            i18n("Delete “%1”", QString::fromStdString(before->title)),
            javelin::app::undo::HistoryDomain::Calendar,
            javelin::app::undo::CalendarEventHistory{
                .connectionId = ownerAccountId,
                .accountId = command.accountId,
                .calendarId =
                    calendar == before->calendarIds.end() ? std::string{} : calendar->first,
                .currentEventId = command.eventId,
                .uid = before->uid,
                .beforeDocumentJson = beforeDocument,
                .afterDocumentJson = std::nullopt,
            },
            QString::fromStdString(*command.operationGroupId), std::nullopt, origin);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&preparedResult))
            co_return javelin::jmap::operationError(*error);
        auto prepared =
            std::get<std::optional<javelin::app::undo::HistoryEntry>>(std::move(preparedResult));
        const auto projectionCommitted = [this, ownerAccountId]
        {
            const auto range = m_visibleCalendarRanges.find(ownerAccountId);
            if (range == m_visibleCalendarRanges.end())
                return;
            Q_EMIT calendarCacheCommitted({.ownerAccountId = QString::fromStdString(ownerAccountId),
                                           .interval = range->second.interval,
                                           .displayTimeZone = range->second.displayTimeZone,
                                           .accountCount = 1,
                                           .eventCount = 0});
        };
        auto result = co_await m_calendarMutationEngine.remove(
            toLiveConnectionSettings(configuration->second.settings), ownerAccountId,
            std::move(command), projectionCommitted);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            if (const auto historyError = retainUnknownOrDiscard(m_undoManager, prepared, *error))
                co_return *historyError;
        }
        else if (prepared.has_value())
        {
            auto committed = m_undoManager.commitNormal(std::move(*prepared));
            if (const auto* historyError =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
                co_return javelin::jmap::operationError(*historyError);
        }
        co_return observeResult(m_errorCoordinator, configuration->second.settings, ownerAccountId,
                                i18n("Delete calendar event"), std::move(result));
    }

    QCoro::Task<javelin::jmap::sieve::SieveListResult>
    SieveApplicationService::requestSieveScripts(std::string ownerAccountId)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_accountRuntime.configurationFor(ownerAccountId);
        if (!configuration.has_value())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = accountSynchronizationNotConfigured(),
            };
        co_return observeResult(
            m_errorCoordinator, configuration->second.settings, ownerAccountId,
            i18n("Load Sieve scripts"),
            co_await m_sieveProtocolClient.list(
                toLiveConnectionSettings(configuration->second.settings), ownerAccountId));
    }

    QCoro::Task<javelin::jmap::sieve::SieveContentResult>
    SieveApplicationService::requestSieveScript(std::string ownerAccountId,
                                                javelin::jmap::sieve::SieveScript script)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_accountRuntime.configurationFor(ownerAccountId);
        if (!configuration.has_value())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = accountSynchronizationNotConfigured(),
            };
        co_return observeResult(m_errorCoordinator, configuration->second.settings, ownerAccountId,
                                i18n("Load Sieve script"),
                                co_await m_sieveProtocolClient.load(
                                    toLiveConnectionSettings(configuration->second.settings),
                                    ownerAccountId, std::move(script)));
    }

    QCoro::Task<javelin::jmap::sieve::SieveValidationResult>
    SieveApplicationService::validateSieveScript(std::string ownerAccountId, QByteArray content)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_accountRuntime.configurationFor(ownerAccountId);
        if (!configuration.has_value())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = accountSynchronizationNotConfigured(),
            };
        co_return observeResult(m_errorCoordinator, configuration->second.settings, ownerAccountId,
                                i18n("Validate Sieve script"),
                                co_await m_sieveProtocolClient.validate(
                                    toLiveConnectionSettings(configuration->second.settings),
                                    ownerAccountId, std::move(content)));
    }

    QCoro::Task<javelin::jmap::sieve::SieveSaveResult> SieveApplicationService::saveSieveScript(
        std::string ownerAccountId, javelin::jmap::sieve::SieveScript script, QByteArray content,
        const javelin::app::undo::CommandOrigin origin)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_accountRuntime.configurationFor(ownerAccountId);
        if (!configuration.has_value())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = accountSynchronizationNotConfigured(),
            };
        const auto operationGroupId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        std::optional<javelin::app::undo::HistoryEntry> prepared;
        if (origin == javelin::app::undo::CommandOrigin::User)
        {
            std::optional<std::string> beforeContent;
            if (!script.id.empty())
            {
                auto loaded = co_await m_sieveProtocolClient.load(
                    toLiveConnectionSettings(configuration->second.settings), ownerAccountId,
                    script);
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&loaded))
                    co_return *error;
                beforeContent = std::get<QByteArray>(loaded).toStdString();
            }
            javelin::app::undo::SieveHistory history{
                .connectionId = configuration->second.settings.connectionId,
                .accountId = ownerAccountId,
                .currentScriptId = script.id.empty() ? std::nullopt : std::optional{script.id},
                .previousScriptId = std::nullopt,
                .beforeName = script.id.empty() ? std::nullopt : std::optional{script.name},
                .beforeContent = std::move(beforeContent),
                .afterName = script.name,
                .afterContent = content.toStdString(),
                .activeScriptIdBefore = script.isActive ? std::optional{script.id} : std::nullopt,
                .activeScriptIdAfter = script.isActive ? std::optional{script.id} : std::nullopt,
            };
            auto preparedResult = m_undoManager.prepareNormal(
                script.id.empty()
                    ? i18n("Create Sieve Script “%1”", QString::fromStdString(script.name))
                    : i18n("Edit Sieve Script “%1”", QString::fromStdString(script.name)),
                javelin::app::undo::HistoryDomain::Mail, std::move(history), operationGroupId,
                std::nullopt, origin);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&preparedResult))
                co_return javelin::jmap::operationError(*error);
            prepared = std::get<std::optional<javelin::app::undo::HistoryEntry>>(
                std::move(preparedResult));
        }

        auto result = observeResult(m_errorCoordinator, configuration->second.settings,
                                    ownerAccountId, i18n("Save Sieve script"),
                                    co_await m_sieveMutationEngine.save(
                                        toLiveConnectionSettings(configuration->second.settings),
                                        ownerAccountId, std::move(script), std::move(content),
                                        operationGroupId.toStdString()));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            if (const auto historyError = retainUnknownOrDiscard(m_undoManager, prepared, *error))
                co_return *historyError;
            co_return *error;
        }
        if (prepared.has_value())
        {
            auto& history = std::get<javelin::app::undo::SieveHistory>(prepared->payload);
            const auto& saved = std::get<javelin::jmap::sieve::SieveScript>(result);
            history.currentScriptId = saved.id;
            if (history.activeScriptIdBefore.has_value())
            {
                history.activeScriptIdBefore = saved.id;
                history.activeScriptIdAfter = saved.id;
            }
            auto committed = m_undoManager.commitNormal(std::move(*prepared));
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
                co_return javelin::jmap::operationError(*error);
        }
        co_return result;
    }

    QCoro::Task<javelin::jmap::sieve::SieveDeleteResult>
    SieveApplicationService::deleteSieveScript(std::string ownerAccountId,
                                               javelin::jmap::sieve::SieveScript script,
                                               const javelin::app::undo::CommandOrigin origin)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_accountRuntime.configurationFor(ownerAccountId);
        if (!configuration.has_value())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = accountSynchronizationNotConfigured(),
            };
        const auto operationGroupId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        std::optional<javelin::app::undo::HistoryEntry> prepared;
        if (origin == javelin::app::undo::CommandOrigin::User)
        {
            auto loaded = co_await m_sieveProtocolClient.load(
                toLiveConnectionSettings(configuration->second.settings), ownerAccountId, script);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&loaded))
                co_return *error;
            javelin::app::undo::SieveHistory history{
                .connectionId = configuration->second.settings.connectionId,
                .accountId = ownerAccountId,
                .currentScriptId = script.id,
                .previousScriptId = script.id,
                .beforeName = script.name,
                .beforeContent = std::get<QByteArray>(loaded).toStdString(),
                .afterName = std::nullopt,
                .afterContent = std::nullopt,
                .activeScriptIdBefore = script.isActive ? std::optional{script.id} : std::nullopt,
                .activeScriptIdAfter = std::nullopt,
            };
            auto preparedResult = m_undoManager.prepareNormal(
                i18n("Delete Sieve Script “%1”", QString::fromStdString(script.name)),
                javelin::app::undo::HistoryDomain::Mail, std::move(history), operationGroupId,
                std::nullopt, origin);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&preparedResult))
                co_return javelin::jmap::operationError(*error);
            prepared = std::get<std::optional<javelin::app::undo::HistoryEntry>>(
                std::move(preparedResult));
        }
        auto result =
            observeResult(m_errorCoordinator, configuration->second.settings, ownerAccountId,
                          i18n("Delete Sieve script"),
                          co_await m_sieveMutationEngine.remove(
                              toLiveConnectionSettings(configuration->second.settings),
                              ownerAccountId, std::move(script), operationGroupId.toStdString()));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            if (const auto historyError = retainUnknownOrDiscard(m_undoManager, prepared, *error))
                co_return *historyError;
            co_return *error;
        }
        if (prepared.has_value())
        {
            auto& history = std::get<javelin::app::undo::SieveHistory>(prepared->payload);
            history.currentScriptId = std::nullopt;
            auto committed = m_undoManager.commitNormal(std::move(*prepared));
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
                co_return javelin::jmap::operationError(*error);
        }
        co_return result;
    }

    QCoro::Task<javelin::jmap::sieve::SieveActivationResult>
    SieveApplicationService::setSieveScriptActive(std::string ownerAccountId,
                                                  javelin::jmap::sieve::SieveScript script,
                                                  const bool active,
                                                  const javelin::app::undo::CommandOrigin origin)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_accountRuntime.configurationFor(ownerAccountId);
        if (!configuration.has_value())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = accountSynchronizationNotConfigured(),
            };
        const auto operationGroupId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        std::optional<javelin::app::undo::HistoryEntry> prepared;
        if (origin == javelin::app::undo::CommandOrigin::User)
        {
            auto listed = co_await m_sieveProtocolClient.list(
                toLiveConnectionSettings(configuration->second.settings), ownerAccountId);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&listed))
                co_return *error;
            const auto& scripts = std::get<std::vector<javelin::jmap::sieve::SieveScript>>(listed);
            const auto currentActive =
                std::ranges::find(scripts, true, &javelin::jmap::sieve::SieveScript::isActive);
            const std::optional<std::string> activeBefore =
                currentActive == scripts.end() ? std::nullopt : std::optional{currentActive->id};
            const std::optional<std::string> activeAfter =
                active ? std::optional{script.id} : std::nullopt;
            if (activeBefore == activeAfter)
                co_return std::monostate{};
            javelin::app::undo::SieveHistory history{
                .connectionId = configuration->second.settings.connectionId,
                .accountId = ownerAccountId,
                .currentScriptId = script.id,
                .previousScriptId = std::nullopt,
                .beforeName = std::nullopt,
                .beforeContent = std::nullopt,
                .afterName = std::nullopt,
                .afterContent = std::nullopt,
                .activeScriptIdBefore = activeBefore,
                .activeScriptIdAfter = activeAfter,
            };
            auto preparedResult = m_undoManager.prepareNormal(
                active ? i18n("Activate Sieve Script “%1”", QString::fromStdString(script.name))
                       : i18n("Deactivate Sieve Script “%1”", QString::fromStdString(script.name)),
                javelin::app::undo::HistoryDomain::Mail, std::move(history), operationGroupId,
                std::nullopt, origin);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&preparedResult))
                co_return javelin::jmap::operationError(*error);
            prepared = std::get<std::optional<javelin::app::undo::HistoryEntry>>(
                std::move(preparedResult));
        }
        auto result = observeResult(m_errorCoordinator, configuration->second.settings,
                                    ownerAccountId, i18n("Change active Sieve script"),
                                    co_await m_sieveMutationEngine.setActive(
                                        toLiveConnectionSettings(configuration->second.settings),
                                        ownerAccountId, std::move(script), active,
                                        operationGroupId.toStdString()));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            if (const auto historyError = retainUnknownOrDiscard(m_undoManager, prepared, *error))
                co_return *historyError;
            co_return *error;
        }
        if (prepared.has_value())
        {
            auto committed = m_undoManager.commitNormal(std::move(*prepared));
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
                co_return javelin::jmap::operationError(*error);
        }
        co_return result;
    }

    void AccountRuntimeManager::connectCoordinator(const std::string& accountId,
                                                   AccountSyncCoordinator& coordinator)
    {
        connect(&coordinator, &AccountSyncCoordinator::statusChanged, this,
                [this, accountId](const auto status)
                { Q_EMIT accountStatusChanged(QString::fromStdString(accountId), status); });
        connect(&coordinator, &AccountSyncCoordinator::cacheCommitted, this,
                &AccountRuntimeManager::cacheCommitted);
        connect(&coordinator, &AccountSyncCoordinator::contactStateChanged, this,
                [this](const QString& ownerAccountId, const auto& changedStates)
                {
                    static_cast<void>(changedStates);
                    Q_EMIT contactStateChanged(ownerAccountId);
                });
        connect(&coordinator, &AccountSyncCoordinator::identityStateChanged, this,
                [this](const QString& ownerAccountId, const auto& changedStates)
                {
                    static_cast<void>(ownerAccountId);
                    for (const auto& [changedAccountId, states] : changedStates)
                    {
                        if (states.contains("Identity"))
                            Q_EMIT senderIdentityStateChanged(
                                QString::fromStdString(changedAccountId));
                    }
                });
        connect(&coordinator, &AccountSyncCoordinator::calendarStateChanged, this,
                [this](const QString& ownerAccountId, const auto& changedStates)
                { Q_EMIT calendarStateChanged(ownerAccountId, changedStates); });
        connect(&coordinator, &AccountSyncCoordinator::notificationEventsCommitted, this,
                &AccountRuntimeManager::notificationEventsCommitted);
        connect(
            &coordinator, &AccountSyncCoordinator::operationFailed, this,
            [this, accountId](const QString& operation, const javelin::jmap::OperationError& error)
            {
                const auto configuration = m_configurations.find(accountId);
                if (configuration != m_configurations.end())
                    m_errorCoordinator.reportFailure(configuration->second.settings, accountId,
                                                     operation, error);
            });
        connect(&coordinator, &AccountSyncCoordinator::operationSucceeded, this,
                [this, accountId]
                {
                    const auto configuration = m_configurations.find(accountId);
                    if (configuration != m_configurations.end())
                        m_errorCoordinator.reportSuccess(
                            configuration->second.settings.connectionId);
                });
        connect(&coordinator, &AccountSyncCoordinator::stateChangeCatchUpRequired, this,
                [this, accountId]
                { Q_EMIT stateChangeCatchUpRequired(QString::fromStdString(accountId)); });
    }

} // namespace javelin::app
