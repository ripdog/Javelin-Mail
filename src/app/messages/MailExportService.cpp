#include "app/MailExportService.h"

#include "app/AccountConnectionProvider.h"
#include "app/AccountConnectionSettings.h"
#include "app/FileNameUtils.h"
#include "app/MailExportRepository.h"
#include "app/MailExportWriter.h"
#include "app/MailSaveNaming.h"
#include "app/RawMailMaterializer.h"
#include "app/WorkScheduler.h"
#include "jmap/MessageContentClient.h"
#include "jmap/OperationError.h"
#include "jmap/api/LiveConnectionSettings.h"
#include "jmap/api/MethodCaller.h"
#include "jmap/cache/AccountRepository.h"
#include "jmap/cache/MailboxReadRepository.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/query/MailQueryClient.h"
#include "jmap/sync/MailboxStateRefreshExecutor.h"

#include <KLocalizedString>

#include <QCoroFuture>
#include <QCoroTask>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QScopeGuard>
#include <QStorageInfo>
#include <QTimer>
#include <QUuid>
#include <QtConcurrentRun>

#include <algorithm>
#include <ranges>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace javelin::app
{
    namespace
    {
        using javelin::jmap::OperationError;
        using javelin::jmap::OperationErrorCode;
        using javelin::jmap::cache::DatabaseError;

        constexpr std::string_view jobPrefix = "mail-export:";
        constexpr std::size_t manifestPageSize = 250;
        constexpr int maximumManifestAttempts = 4;

        [[nodiscard]] std::string jobIdFor(const std::string_view operationId)
        {
            return std::string{jobPrefix} + std::string{operationId};
        }

        [[nodiscard]] std::optional<std::string> operationIdFor(const std::string_view jobId)
        {
            if (!jobId.starts_with(jobPrefix) || jobId.size() <= jobPrefix.size())
                return std::nullopt;
            return std::string{jobId.substr(jobPrefix.size())};
        }

        [[nodiscard]] javelin::jmap::LiveConnectionSettings
        liveSettings(const AccountConnectionSettings& settings)
        {
            return {
                .sessionUrl = settings.sessionUrl,
                .loginEmail = settings.loginEmail,
                .apiKey = settings.apiKey,
            };
        }

        [[nodiscard]] javelin::jmap::api::ApiRequestContext
        apiRequestContext(const AccountConnectionSettings& settings,
                          const std::string_view localAccountId,
                          const javelin::jmap::api::Session& session)
        {
            return {
                .credentials =
                    {
                        .accountId = std::string{localAccountId},
                        .emailAddress = settings.loginEmail,
                        .sessionUrl = settings.sessionUrl,
                        .token = {.accessToken = settings.apiKey,
                                  .refreshToken = std::nullopt,
                                  .expiry = std::nullopt},
                    },
                .apiUrl = session.apiUrl,
                .requestLimits = javelin::jmap::api::coreRequestLimits(session),
            };
        }

        [[nodiscard]] bool retryable(const OperationError& error)
        {
            return javelin::jmap::isAuthenticationError(error) ||
                   javelin::jmap::isTransientError(error);
        }

        [[nodiscard]] MailExportStatus waitStatus(const OperationError& error)
        {
            return javelin::jmap::isAuthenticationError(error)
                       ? MailExportStatus::WaitingForAuth
                       : MailExportStatus::WaitingForNetwork;
        }

        [[nodiscard]] WorkStatus workStatus(const MailExportStatus status)
        {
            switch (status)
            {
            case MailExportStatus::Preparing:
            case MailExportStatus::Running:
                return WorkStatus::Queued;
            case MailExportStatus::WaitingForNetwork:
                return WorkStatus::WaitingForNetwork;
            case MailExportStatus::WaitingForAuth:
                return WorkStatus::WaitingForAuth;
            case MailExportStatus::WaitingForSpace:
                return WorkStatus::WaitingForSpace;
            case MailExportStatus::Complete:
            case MailExportStatus::Partial:
                return WorkStatus::Complete;
            case MailExportStatus::Failed:
                return WorkStatus::Failed;
            }
            return WorkStatus::Failed;
        }

        [[nodiscard]] QString detail(const MailExportStatus status, const bool sealed)
        {
            switch (status)
            {
            case MailExportStatus::Preparing:
                return i18n("Preparing export");
            case MailExportStatus::Running:
                return sealed ? i18n("Exporting messages") : i18n("Preparing export");
            case MailExportStatus::WaitingForNetwork:
                return i18n("Waiting for network");
            case MailExportStatus::WaitingForAuth:
                return i18n("Waiting for sign-in");
            case MailExportStatus::WaitingForSpace:
                return i18n("Waiting for storage space");
            case MailExportStatus::Partial:
                return i18n("Export completed partially");
            case MailExportStatus::Failed:
                return i18n("Export failed");
            case MailExportStatus::Complete:
                return i18n("Export complete");
            }
            return {};
        }

        [[nodiscard]] QString checkpoint(const std::string_view operationId)
        {
            return QString::fromUtf8(
                QJsonDocument{QJsonObject{{QStringLiteral("operationId"),
                                           QString::fromStdString(std::string{operationId})}}}
                    .toJson(QJsonDocument::Compact));
        }

        [[nodiscard]] WorkProgress progressFor(const MailExportOperationRecord& operation,
                                               const MailExportProgressSnapshot& progress)
        {
            return {
                .completedUnits = progress.completedItems,
                .totalUnits = operation.manifestSealed
                                  ? std::optional<std::uint64_t>{progress.totalItems}
                                  : std::nullopt,
                .completedBytes = progress.completedBytes,
                .totalBytes = operation.manifestSealed
                                  ? std::optional<std::uint64_t>{progress.totalBytes}
                                  : std::nullopt,
                .detail = detail(operation.status, operation.manifestSealed),
            };
        }

        [[nodiscard]] std::optional<OperationError>
        syncJob(WorkScheduler& scheduler, MailExportRepository& repository,
                const MailExportOperationRecord& operation, const std::string_view jobId)
        {
            const auto progressResult = repository.progress(operation.operationId);
            if (const auto* error = std::get_if<DatabaseError>(&progressResult))
                return javelin::jmap::operationError(*error);
            auto status = workStatus(operation.status);
            const auto existing = scheduler.find(jobId);
            if (const auto* record = std::get_if<std::optional<WorkRecord>>(&existing);
                record != nullptr && record->has_value() &&
                ((*record)->pauseRequested || (*record)->status == WorkStatus::Paused))
            {
                status = WorkStatus::Paused;
            }
            auto errorText = operation.lastError;
            if (status != WorkStatus::Failed)
                errorText.reset();
            if (const auto error = scheduler.update(
                    jobId, status,
                    progressFor(operation, std::get<MailExportProgressSnapshot>(progressResult)),
                    checkpoint(operation.operationId), errorText))
                return javelin::jmap::operationError(*error);
            return std::nullopt;
        }

        [[nodiscard]] QString stableSuffix(const std::string_view value)
        {
            return QString::fromLatin1(
                QCryptographicHash::hash(QByteArray::fromStdString(std::string{value}),
                                         QCryptographicHash::Sha256)
                    .toHex()
                    .left(8));
        }

        [[nodiscard]] QString mailboxBaseComponent(const std::string_view name)
        {
            return truncateGeneratedFileName(
                sanitizeMailSaveComponent(QString::fromStdString(std::string{name}), u"Mailbox"),
                120);
        }

        [[nodiscard]] std::variant<std::vector<MailExportMailboxRecord>, OperationError>
        planMailboxSnapshot(const MailExportOperationRecord& operation,
                            const std::vector<javelin::jmap::cache::MailboxTreeItem>& allMailboxes)
        {
            std::unordered_map<std::string, const javelin::jmap::cache::MailboxTreeItem*> byId;
            for (const auto& mailbox : allMailboxes)
            {
                if (!mailbox.pendingCreate)
                    byId.emplace(mailbox.id, &mailbox);
            }

            std::vector<const javelin::jmap::cache::MailboxTreeItem*> included;
            if (operation.scopeKind == MailExportScopeKind::Mailbox)
            {
                if (!operation.mailboxId.has_value())
                {
                    return OperationError{.code = OperationErrorCode::InvalidRequest,
                                          .message = i18n("The mailbox export has no mailbox.")};
                }
                const auto found = byId.find(*operation.mailboxId);
                if (found == byId.end() || !found->second->myRights.mayReadItems)
                {
                    return OperationError{.code = OperationErrorCode::NotFound,
                                          .message = i18n("The mailbox is no longer readable.")};
                }
                included.push_back(found->second);
            }
            else
            {
                for (const auto& mailbox : allMailboxes)
                {
                    if (!mailbox.pendingCreate && mailbox.myRights.mayReadItems)
                        included.push_back(&mailbox);
                }
                if (included.empty())
                {
                    return OperationError{.code = OperationErrorCode::NotFound,
                                          .message =
                                              i18n("This account has no readable mailboxes.")};
                }
            }

            std::unordered_map<std::string, QString> componentById;
            std::unordered_map<std::string, std::size_t> siblingNameCounts;
            for (const auto& [mailboxId, mailbox] : byId)
            {
                const auto base = mailboxBaseComponent(mailbox->name);
                const auto parentKey = mailbox->parentId.value_or(std::string{});
                const auto collisionKey = parentKey + '\n' + base.toCaseFolded().toStdString();
                ++siblingNameCounts[collisionKey];
                componentById.emplace(mailboxId, base);
            }
            for (const auto& [mailboxId, mailbox] : byId)
            {
                auto& component = componentById[mailboxId];
                const auto parentKey = mailbox->parentId.value_or(std::string{});
                const auto collisionKey = parentKey + '\n' + component.toCaseFolded().toStdString();
                if (siblingNameCounts[collisionKey] > 1)
                {
                    component = truncateGeneratedFileName(
                        component + QStringLiteral("--") + stableSuffix(mailboxId), 120);
                }
            }

            std::unordered_map<std::string, QString> pathCache;
            std::unordered_set<std::string> visiting;
            const auto pathFor =
                [&](const auto& self,
                    const std::string& mailboxId) -> std::variant<QString, OperationError>
            {
                if (const auto cached = pathCache.find(mailboxId); cached != pathCache.end())
                    return cached->second;
                const auto found = byId.find(mailboxId);
                if (found == byId.end())
                    return QString{};
                if (!visiting.insert(mailboxId).second)
                {
                    return OperationError{.code = OperationErrorCode::ProtocolViolation,
                                          .message =
                                              i18n("The mailbox hierarchy contains a cycle.")};
                }
                QString path;
                if (found->second->parentId.has_value() && byId.contains(*found->second->parentId))
                {
                    auto parentPath = self(self, *found->second->parentId);
                    if (const auto* error = std::get_if<OperationError>(&parentPath))
                        return *error;
                    path = std::get<QString>(std::move(parentPath));
                }
                if (!path.isEmpty())
                    path += QLatin1Char('/');
                path += componentById[mailboxId];
                visiting.erase(mailboxId);
                pathCache.emplace(mailboxId, path);
                return path;
            };

            std::vector<MailExportMailboxRecord> result;
            result.reserve(included.size());
            for (const auto* mailbox : included)
            {
                auto pathResult = pathFor(pathFor, mailbox->id);
                if (const auto* error = std::get_if<OperationError>(&pathResult))
                    return *error;
                result.push_back({
                    .ordinal = 0,
                    .mailboxId = mailbox->id,
                    .displayName = QString::fromStdString(mailbox->name),
                    .relativePath = std::get<QString>(std::move(pathResult)),
                });
            }
            std::ranges::sort(result,
                              [](const auto& left, const auto& right)
                              {
                                  const auto compared = QString::compare(
                                      left.relativePath, right.relativePath, Qt::CaseInsensitive);
                                  return compared == 0 ? left.mailboxId < right.mailboxId
                                                       : compared < 0;
                              });
            for (std::size_t index = 0; index < result.size(); ++index)
                result[index].ordinal = index;
            return result;
        }

        [[nodiscard]] QString messageFileName(const MailExportItemRecord& item)
        {
            javelin::jmap::domain::Email email{
                .id = item.emailId,
                .blobId = item.blobId,
                .threadId = {},
                .mailboxIds = {},
                .keywords = {},
                .size = item.size,
                .receivedAt = item.receivedAt,
                .sentAt = std::nullopt,
                .messageId = {},
                .inReplyTo = {},
                .references = {},
                .hasAttachment = false,
                .subject = item.subject,
                .from = {},
                .to = {},
                .cc = {},
                .bcc = {},
                .replyTo = {},
                .preview = std::nullopt,
            };
            if (item.senderName.has_value() || item.senderEmail.has_value())
            {
                email.from.push_back(
                    {.name = item.senderName, .email = item.senderEmail.value_or(std::string{})});
            }
            const auto suggested = suggestedMailSaveFileName(email);
            QString baseName = QFileInfo{suggested}.completeBaseName();
            const QString uniqueSuffix =
                QStringLiteral("--%1.eml").arg(static_cast<qulonglong>(item.ordinal));
            constexpr qsizetype maximumUtf8Bytes = 240;
            const qsizetype baseBudget = maximumUtf8Bytes - uniqueSuffix.toUtf8().size();
            while (!baseName.isEmpty() && baseName.toUtf8().size() > baseBudget)
                chopLastUnicodeCodePoint(baseName);
            if (baseName.isEmpty())
                baseName = QStringLiteral("message");
            return baseName + uniqueSuffix;
        }

        [[nodiscard]] QString outputRelativePath(const MailExportOperationRecord& operation,
                                                 const MailExportMailboxRecord& mailbox,
                                                 const MailExportItemRecord& item)
        {
            if (operation.format == MailExportFormat::MboxRd)
                return mailbox.relativePath + QStringLiteral(".mbox");
            if (operation.scopeKind == MailExportScopeKind::Mailbox)
                return messageFileName(item);
            return mailbox.relativePath + QLatin1Char('/') + messageFileName(item);
        }

        [[nodiscard]] QString absoluteOutputPath(const MailExportOperationRecord& operation,
                                                 const QString& relativePath)
        {
            return QDir{operation.destinationDirectory}.filePath(relativePath);
        }

        [[nodiscard]] std::optional<OperationError>
        validateExportRelativePath(const MailExportOperationRecord& operation,
                                   const QString& relativePath)
        {
            const QString cleaned = QDir::cleanPath(relativePath);
            if (cleaned.isEmpty() || QDir::isAbsolutePath(cleaned) ||
                cleaned == QStringLiteral("..") || cleaned.startsWith(QStringLiteral("../")))
            {
                return OperationError{
                    .code = OperationErrorCode::PreconditionFailed,
                    .message = i18n("The export path is outside the selected destination: %1",
                                    relativePath),
                };
            }

            QString current = QDir{operation.destinationDirectory}.absolutePath();
            const auto parts = cleaned.split(QLatin1Char('/'), Qt::SkipEmptyParts);
            for (const auto& part : parts)
            {
                current = QDir{current}.filePath(part);
                const QFileInfo info{current};
                if (info.isSymLink())
                {
                    return OperationError{
                        .code = OperationErrorCode::PreconditionFailed,
                        .message = i18n("The export path contains a symbolic link: %1", current),
                    };
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<OperationError>
        ensureOutputStructure(const MailExportOperationRecord& operation,
                              const std::vector<MailExportMailboxRecord>& mailboxes)
        {
            QDir root{operation.destinationDirectory};
            for (const auto& mailbox : mailboxes)
            {
                const QString outputPath = operation.format == MailExportFormat::MboxRd
                                               ? mailbox.relativePath + QStringLiteral(".mbox")
                                               : mailbox.relativePath;
                if (const auto error = validateExportRelativePath(operation, outputPath))
                    return error;

                QString relativeDirectory;
                if (operation.format == MailExportFormat::Eml)
                {
                    if (operation.scopeKind == MailExportScopeKind::Account)
                        relativeDirectory = mailbox.relativePath;
                }
                else
                {
                    const QFileInfo output{mailbox.relativePath + QStringLiteral(".mbox")};
                    relativeDirectory =
                        output.path() == QStringLiteral(".") ? QString{} : output.path();
                    // A mailbox may itself have children. Create its directory as well so
                    // Parent.mbox and Parent/Child.mbox can coexist.
                    if (std::ranges::any_of(mailboxes,
                                            [&mailbox](const auto& candidate)
                                            {
                                                return candidate.relativePath.startsWith(
                                                    mailbox.relativePath + QLatin1Char('/'));
                                            }))
                    {
                        if (!root.mkpath(mailbox.relativePath))
                        {
                            return OperationError{
                                .code = OperationErrorCode::LocalStorageFailure,
                                .message = i18n("Could not create export directory %1.",
                                                root.filePath(mailbox.relativePath)),
                            };
                        }
                    }
                }
                if (!relativeDirectory.isEmpty() && !root.mkpath(relativeDirectory))
                {
                    return OperationError{
                        .code = OperationErrorCode::LocalStorageFailure,
                        .message = i18n("Could not create export directory %1.",
                                        root.filePath(relativeDirectory)),
                    };
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<OperationError>
        writeMarker(const MailExportOperationRecord& operation, const QStringView status)
        {
            const auto path = QDir{operation.destinationDirectory}.filePath(
                QStringLiteral(".javelin-mail-export.json"));
            QSaveFile marker{path};
            if (!marker.open(QIODevice::WriteOnly))
            {
                return OperationError{
                    .code = OperationErrorCode::LocalStorageFailure,
                    .message = i18n("Could not update export metadata: %1", marker.errorString())};
            }
            const QJsonObject document{
                {QStringLiteral("version"), 1},
                {QStringLiteral("operationId"), QString::fromStdString(operation.operationId)},
                {QStringLiteral("status"), status.toString()},
                {QStringLiteral("format"), operation.format == MailExportFormat::Eml
                                               ? QStringLiteral("eml")
                                               : QStringLiteral("mboxrd")},
            };
            const auto bytes = QJsonDocument{document}.toJson(QJsonDocument::Indented);
            if (marker.write(bytes) != bytes.size() || !marker.commit())
            {
                return OperationError{
                    .code = OperationErrorCode::LocalStorageFailure,
                    .message = i18n("Could not update export metadata: %1", marker.errorString())};
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<OperationError>
        publishMboxFiles(const MailExportOperationRecord& operation,
                         const std::vector<MailExportMailboxRecord>& mailboxes)
        {
            QDir root{operation.destinationDirectory};
            for (const auto& mailbox : mailboxes)
            {
                const QString finalRelative = mailbox.relativePath + QStringLiteral(".mbox");
                const QString finalPath = root.filePath(finalRelative);
                const QString partPath = finalPath + QStringLiteral(".javelin-part");
                if (QFileInfo::exists(finalPath) && !QFileInfo::exists(partPath))
                    continue;
                const QFileInfo finalInfo{finalPath};
                if (!root.mkpath(finalInfo.path().startsWith(operation.destinationDirectory)
                                     ? QDir{operation.destinationDirectory}.relativeFilePath(
                                           finalInfo.path())
                                     : QStringLiteral(".")))
                {
                    return OperationError{
                        .code = OperationErrorCode::LocalStorageFailure,
                        .message = i18n("Could not create export directory %1.", finalInfo.path())};
                }
                if (!QFileInfo::exists(partPath))
                {
                    QFile empty{partPath};
                    if (!empty.open(QIODevice::WriteOnly))
                    {
                        return OperationError{.code = OperationErrorCode::LocalStorageFailure,
                                              .message = i18n("Could not create %1: %2", partPath,
                                                              empty.errorString())};
                    }
                    empty.close();
                }
                if (QFileInfo::exists(finalPath))
                {
                    return OperationError{
                        .code = OperationErrorCode::PreconditionFailed,
                        .message = i18n("The export target was modified externally: %1", finalPath),
                    };
                }
                if (!QFile::rename(partPath, finalPath))
                {
                    return OperationError{.code = OperationErrorCode::LocalStorageFailure,
                                          .message =
                                              i18n("Could not publish mbox file %1.", finalPath)};
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<OperationError>
        availableSpaceError(const QString& destinationDirectory, const std::uint64_t bytesNeeded)
        {
            QStorageInfo storage{destinationDirectory};
            if (!storage.isValid() || !storage.isReady() || storage.bytesAvailable() < 0)
                return std::nullopt;
            constexpr std::uint64_t safetyMargin = 1024 * 1024;
            if (static_cast<std::uint64_t>(storage.bytesAvailable()) >= bytesNeeded + safetyMargin)
                return std::nullopt;
            return OperationError{.code = OperationErrorCode::LocalStorageFailure,
                                  .message = i18n("The export destination does not have enough "
                                                  "free space for the next message.")};
        }
    } // namespace

    MailExportService::MailExportService(
        javelin::jmap::cache::DatabaseConnection& databaseConnection,
        javelin::jmap::api::JmapMethodTransport& methodTransport,
        javelin::jmap::MailQueryClient& mailQueryClient,
        javelin::jmap::MessageContentClient& messageContentClient,
        const AccountConnectionProvider& connectionProvider, WorkScheduler& workScheduler,
        QObject* parent)
        : QObject(parent), m_databaseConnection(databaseConnection),
          m_methodTransport(methodTransport), m_mailQueryClient(mailQueryClient),
          m_messageContentClient(messageContentClient), m_connectionProvider(connectionProvider),
          m_workScheduler(workScheduler)
    {
        connect(&m_workScheduler, &WorkScheduler::jobsChanged, this, [this] { schedulePump(); });
        connect(&m_workScheduler, &WorkScheduler::foregroundAvailabilityChanged, this,
                [this] { schedulePump(); });
    }

    QCoro::Task<MailExportStartResult> MailExportService::startExport(MailExportIntent intent)
    {
        if (intent.accountId.empty() || intent.destinationDirectory.isEmpty() ||
            !QDir::isAbsolutePath(intent.destinationDirectory))
        {
            co_return OperationError{.code = OperationErrorCode::InvalidRequest,
                                     .message = i18n("The mail export request is incomplete.")};
        }
        if (intent.scopeKind == MailExportScopeKind::Mailbox && !intent.mailboxId.has_value())
        {
            co_return OperationError{.code = OperationErrorCode::InvalidRequest,
                                     .message = i18n("Choose a mailbox to export.")};
        }
        if (intent.scopeKind == MailExportScopeKind::Account && intent.mailboxId.has_value())
        {
            co_return OperationError{.code = OperationErrorCode::InvalidRequest,
                                     .message =
                                         i18n("An account export cannot target one mailbox.")};
        }
        const auto settings = m_connectionProvider.connectionSettingsFor(intent.accountId);
        if (!settings.has_value())
        {
            co_return OperationError{.code = OperationErrorCode::NotFound,
                                     .message = i18n("The selected mail account is unavailable.")};
        }

        QString scopeName;
        if (intent.scopeKind == MailExportScopeKind::Mailbox)
        {
            const auto mailboxResult =
                javelin::jmap::cache::MailboxReadRepository{m_databaseConnection}.listMailboxTree(
                    intent.accountId);
            if (const auto* error = std::get_if<DatabaseError>(&mailboxResult))
                co_return javelin::jmap::operationError(*error);
            const auto& mailboxes =
                std::get<std::vector<javelin::jmap::cache::MailboxTreeItem>>(mailboxResult);
            const auto found = std::ranges::find(mailboxes, *intent.mailboxId,
                                                 &javelin::jmap::cache::MailboxTreeItem::id);
            if (found == mailboxes.end() || found->pendingCreate || !found->myRights.mayReadItems)
            {
                co_return OperationError{.code = OperationErrorCode::NotFound,
                                         .message = i18n("The selected mailbox is not readable.")};
            }
            scopeName = QString::fromStdString(found->name);
        }
        else
        {
            scopeName = QString::fromStdString(
                settings->displayName.empty() ? settings->loginEmail : settings->displayName);
        }

        const QFileInfo initialDestinationInfo{intent.destinationDirectory};
        if (initialDestinationInfo.isSymLink())
        {
            co_return OperationError{
                .code = OperationErrorCode::PreconditionFailed,
                .message = i18n("Choose a real directory rather than a symbolic link for the mail "
                                "export: %1",
                                intent.destinationDirectory),
            };
        }

        QDir destination{intent.destinationDirectory};
        bool createdDirectory = false;
        if (!destination.exists())
        {
            if (!QDir{}.mkpath(intent.destinationDirectory))
            {
                co_return OperationError{.code = OperationErrorCode::LocalStorageFailure,
                                         .message = i18n("Could not create export directory %1.",
                                                         intent.destinationDirectory)};
            }
            createdDirectory = true;
            destination = QDir{intent.destinationDirectory};
        }
        if (!QFileInfo{intent.destinationDirectory}.isDir() ||
            !destination.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty())
        {
            co_return OperationError{
                .code = OperationErrorCode::PreconditionFailed,
                .message = i18n("Choose a new or empty directory for the mail export: %1",
                                intent.destinationDirectory),
            };
        }

        const auto operationId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        const QString markerPath =
            destination.filePath(QStringLiteral(".javelin-mail-export.json"));
        QFile marker{markerPath};
        if (!marker.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        {
            if (createdDirectory)
                QDir{}.rmdir(intent.destinationDirectory);
            co_return OperationError{.code = OperationErrorCode::LocalStorageFailure,
                                     .message =
                                         i18n("Could not claim export directory %1: %2",
                                              intent.destinationDirectory, marker.errorString())};
        }
        const QJsonObject markerDocument{
            {QStringLiteral("version"), 1},
            {QStringLiteral("operationId"), QString::fromStdString(operationId)},
            {QStringLiteral("status"), QStringLiteral("preparing")},
        };
        const auto markerBytes = QJsonDocument{markerDocument}.toJson(QJsonDocument::Indented);
        if (marker.write(markerBytes) != markerBytes.size())
        {
            marker.close();
            QFile::remove(markerPath);
            if (createdDirectory)
                QDir{}.rmdir(intent.destinationDirectory);
            co_return OperationError{.code = OperationErrorCode::LocalStorageFailure,
                                     .message = i18n("Could not initialize export directory %1.",
                                                     intent.destinationDirectory)};
        }
        marker.close();

        MailExportOperationRecord operation{
            .operationId = operationId,
            .accountId = intent.accountId,
            .scopeKind = intent.scopeKind,
            .mailboxId = intent.mailboxId,
            .format = intent.format,
            .destinationDirectory = intent.destinationDirectory,
            .status = MailExportStatus::Preparing,
            .manifestSealed = false,
            .manifestEmailState = std::nullopt,
            .title = intent.scopeKind == MailExportScopeKind::Mailbox
                         ? i18n("Export mailbox “%1”", scopeName)
                         : i18n("Export account “%1”", scopeName),
            .createdAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs),
            .lastError = std::nullopt,
        };
        MailExportRepository repository{m_databaseConnection};
        if (const auto error = repository.createOperation(operation))
        {
            QFile::remove(markerPath);
            if (createdDirectory)
                QDir{}.rmdir(intent.destinationDirectory);
            co_return javelin::jmap::operationError(*error);
        }
        if (const auto trackingError = ensureTracked(operationId))
        {
            static_cast<void>(repository.setStatus(operationId, MailExportStatus::Failed,
                                                   trackingError->message));
            static_cast<void>(writeMarker(operation, u"failed"));
            co_return *trackingError;
        }
        m_backgroundEnabled = true;
        schedulePump();
        co_return MailExportAdmission{.operationId = operationId, .jobId = jobIdFor(operationId)};
    }

    void MailExportService::restoreRecoverable()
    {
        m_backgroundEnabled = true;
        MailExportRepository repository{m_databaseConnection};
        const auto result = repository.listRecoverable();
        const auto* operations = std::get_if<std::vector<MailExportOperationRecord>>(&result);
        if (operations == nullptr)
            return;
        for (const auto& operation : *operations)
        {
            if (ensureTracked(operation.operationId).has_value())
                continue;
            static_cast<void>(
                syncJob(m_workScheduler, repository, operation, jobIdFor(operation.operationId)));
        }
        schedulePump();
    }

    void MailExportService::networkBecameReachable()
    {
        restoreRecoverable();
        requeueWaiting(false);
    }

    void MailExportService::authenticationBecameAvailable()
    {
        restoreRecoverable();
        requeueWaiting(true);
    }

    void MailExportService::schedulePump()
    {
        if (m_pumpScheduled)
            return;
        m_pumpScheduled = true;
        QTimer::singleShot(0, this,
                           [this]
                           {
                               m_pumpScheduled = false;
                               pump();
                           });
    }

    void MailExportService::pump()
    {
        if (!m_backgroundEnabled)
            return;
        const auto listed = m_workScheduler.list();
        const auto* jobs = std::get_if<std::vector<WorkRecord>>(&listed);
        if (jobs == nullptr)
            return;
        for (const auto& job : *jobs)
        {
            if (job.kind != WorkKind::MailExport || job.status != WorkStatus::Queued ||
                job.pauseRequested)
                continue;
            const auto operationId = operationIdFor(job.jobId);
            if (!operationId.has_value() || m_runningOperations.contains(*operationId))
                continue;
            if (!m_workScheduler.admit(job.jobId).has_value())
                continue;
            m_runningOperations.insert(*operationId);
            auto task = runOne(*operationId, job.jobId);
            QCoro::connect(std::move(task), this, [] {});
        }
    }

    std::optional<OperationError>
    MailExportService::ensureTracked(const std::string_view operationId)
    {
        MailExportRepository repository{m_databaseConnection};
        const auto result = repository.findOperation(operationId);
        if (const auto* error = std::get_if<DatabaseError>(&result))
            return javelin::jmap::operationError(*error);
        const auto& operation = std::get<std::optional<MailExportOperationRecord>>(result);
        if (!operation.has_value())
        {
            return OperationError{.code = OperationErrorCode::NotFound,
                                  .message = i18n("The mail export job is no longer available.")};
        }
        const auto jobId = jobIdFor(operationId);
        if (const auto error = m_workScheduler.ensure({
                .jobId = jobId,
                .parentJobId = std::nullopt,
                .accountId = operation->accountId,
                .kind = WorkKind::MailExport,
                .priority = WorkPriority::Bulk,
                .title = operation->title,
                .checkpointJson = checkpoint(operationId),
            }))
            return javelin::jmap::operationError(*error);
        return std::nullopt;
    }

    void MailExportService::requeueWaiting(const bool authentication)
    {
        MailExportRepository repository{m_databaseConnection};
        const auto listed = m_workScheduler.list();
        const auto* jobs = std::get_if<std::vector<WorkRecord>>(&listed);
        if (jobs == nullptr)
            return;
        for (const auto& job : *jobs)
        {
            const auto wanted =
                authentication ? WorkStatus::WaitingForAuth : WorkStatus::WaitingForNetwork;
            if (job.kind != WorkKind::MailExport || job.status != wanted)
                continue;
            const auto operationId = operationIdFor(job.jobId);
            if (!operationId.has_value())
                continue;
            const auto operationResult = repository.findOperation(*operationId);
            const auto* operation =
                std::get_if<std::optional<MailExportOperationRecord>>(&operationResult);
            if (operation == nullptr || !operation->has_value())
                continue;
            static_cast<void>(repository.setStatus(
                *operationId, (*operation)->manifestSealed ? MailExportStatus::Running
                                                           : MailExportStatus::Preparing));
            static_cast<void>(m_workScheduler.update(job.jobId, WorkStatus::Queued, job.progress,
                                                     job.checkpointJson));
        }
        schedulePump();
    }

    QCoro::Task<void> MailExportService::runOne(std::string operationId, std::string jobId)
    {
        const auto guard = qScopeGuard(
            [this, &operationId, &jobId]
            {
                m_workScheduler.release(jobId);
                m_runningOperations.erase(operationId);
                schedulePump();
            });
        MailExportRepository repository{m_databaseConnection};
        const auto before = repository.findOperation(operationId);
        if (const auto* operation = std::get_if<std::optional<MailExportOperationRecord>>(&before);
            operation != nullptr && operation->has_value())
        {
            if ((*operation)->manifestSealed &&
                ((*operation)->status == MailExportStatus::WaitingForSpace ||
                 (*operation)->status == MailExportStatus::WaitingForNetwork ||
                 (*operation)->status == MailExportStatus::WaitingForAuth ||
                 (*operation)->status == MailExportStatus::Failed))
            {
                static_cast<void>(
                    repository.setStatus(operationId, MailExportStatus::Running, std::nullopt));
            }
            else if (!(*operation)->manifestSealed &&
                     (*operation)->status == MailExportStatus::Failed)
            {
                static_cast<void>(
                    repository.setStatus(operationId, MailExportStatus::Preparing, std::nullopt));
            }
            const auto refreshedOperationResult = repository.findOperation(operationId);
            const auto* refreshedOperation =
                std::get_if<std::optional<MailExportOperationRecord>>(&refreshedOperationResult);
            const auto& effectiveOperation =
                refreshedOperation != nullptr && refreshedOperation->has_value()
                    ? **refreshedOperation
                    : **operation;
            const auto progressResult = repository.progress(operationId);
            if (const auto* progress = std::get_if<MailExportProgressSnapshot>(&progressResult))
            {
                auto running = effectiveOperation;
                if (running.status == MailExportStatus::Running ||
                    running.status == MailExportStatus::Preparing)
                {
                    static_cast<void>(m_workScheduler.update(jobId, WorkStatus::Running,
                                                             progressFor(running, *progress),
                                                             checkpoint(operationId)));
                }
            }
        }
        co_await advanceOne(operationId, jobId);
    }

    QCoro::Task<void> MailExportService::advanceOne(std::string operationId, std::string jobId)
    {
        MailExportRepository repository{m_databaseConnection};
        auto result = repository.findOperation(operationId);
        auto* operation = std::get_if<std::optional<MailExportOperationRecord>>(&result);
        if (operation == nullptr || !operation->has_value())
            co_return;

        if (!(*operation)->manifestSealed)
        {
            if (const auto error = co_await prepareManifest(**operation, jobId))
            {
                if (!retryable(*error))
                    static_cast<void>(repository.setStatus(operationId, MailExportStatus::Failed,
                                                           error->message));
            }
        }
        else
        {
            if (const auto error = co_await writeNextItem(**operation, jobId))
            {
                if (!retryable(*error))
                    static_cast<void>(repository.setStatus(operationId, MailExportStatus::Failed,
                                                           error->message));
            }
        }

        result = repository.findOperation(operationId);
        operation = std::get_if<std::optional<MailExportOperationRecord>>(&result);
        if (operation != nullptr && operation->has_value())
            static_cast<void>(syncJob(m_workScheduler, repository, **operation, jobId));
    }

    QCoro::Task<std::optional<OperationError>>
    MailExportService::prepareManifest(MailExportOperationRecord operation, std::string jobId)
    {
        MailExportRepository repository{m_databaseConnection};
        const auto settings = m_connectionProvider.connectionSettingsFor(operation.accountId);
        if (!settings.has_value())
        {
            co_return OperationError{.code = OperationErrorCode::NotFound,
                                     .message = i18n("The mail account is no longer available.")};
        }
        javelin::jmap::cache::AccountRepository accounts{m_databaseConnection};
        const auto accountResult = accounts.findById(operation.accountId);
        if (const auto* error = std::get_if<DatabaseError>(&accountResult))
            co_return javelin::jmap::operationError(*error);
        const auto& account =
            std::get<std::optional<javelin::jmap::cache::CachedAccount>>(accountResult);
        if (!account.has_value())
        {
            co_return OperationError{.code = OperationErrorCode::NotFound,
                                     .message = i18n("The mail account is no longer available.")};
        }

        javelin::jmap::cache::SessionRepository sessions{m_databaseConnection};
        const auto sessionResult = sessions.load(operation.accountId);
        if (const auto* error = std::get_if<DatabaseError>(&sessionResult))
            co_return javelin::jmap::operationError(*error);
        const auto& session = std::get<std::optional<javelin::jmap::api::Session>>(sessionResult);
        if (!session.has_value())
        {
            co_return OperationError{
                .code = OperationErrorCode::NetworkUnavailable,
                .message = i18n("The JMAP session is not available for this export."),
            };
        }

        javelin::jmap::api::MethodCaller caller{m_methodTransport};
        javelin::jmap::sync::MailboxStateRefreshExecutor mailboxRefresh{
            m_databaseConnection, caller,
            apiRequestContext(*settings, operation.accountId, *session)};
        const auto mailboxRefreshResult =
            co_await mailboxRefresh.refresh(operation.accountId, account->remoteAccountId);
        if (const auto* error = std::get_if<OperationError>(&mailboxRefreshResult))
        {
            if (retryable(*error))
                static_cast<void>(repository.setStatus(operation.operationId, waitStatus(*error),
                                                       error->message));
            co_return *error;
        }

        const auto mailboxesResult =
            javelin::jmap::cache::MailboxReadRepository{m_databaseConnection}.listMailboxTree(
                operation.accountId);
        if (const auto* error = std::get_if<DatabaseError>(&mailboxesResult))
            co_return javelin::jmap::operationError(*error);
        auto planned = planMailboxSnapshot(
            operation,
            std::get<std::vector<javelin::jmap::cache::MailboxTreeItem>>(mailboxesResult));
        if (const auto* error = std::get_if<OperationError>(&planned))
            co_return *error;
        const auto mailboxes = std::get<std::vector<MailExportMailboxRecord>>(std::move(planned));
        if (const auto error = repository.replaceMailboxes(operation.operationId, mailboxes))
            co_return javelin::jmap::operationError(*error);
        if (const auto error = ensureOutputStructure(operation, mailboxes))
            co_return *error;

        for (int attempt = 0; attempt < maximumManifestAttempts; ++attempt)
        {
            if (const auto error = repository.resetManifest(operation.operationId))
                co_return javelin::jmap::operationError(*error);
            std::optional<std::string> baselineEmailState;
            std::uint64_t ordinal = 0;
            bool changedDuringEnumeration = false;

            for (const auto& mailbox : mailboxes)
            {
                std::size_t position = 0;
                std::optional<std::string> queryState;
                while (true)
                {
                    const auto jobResult = m_workScheduler.find(jobId);
                    if (const auto* job = std::get_if<std::optional<WorkRecord>>(&jobResult);
                        job != nullptr && job->has_value() &&
                        ((*job)->pauseRequested || (*job)->status == WorkStatus::Paused))
                    {
                        co_return std::nullopt;
                    }
                    auto pageResult = co_await m_mailQueryClient.fetchFullMailboxPage(
                        liveSettings(*settings), operation.accountId, mailbox.mailboxId, position,
                        manifestPageSize);
                    if (const auto* error = std::get_if<OperationError>(&pageResult))
                    {
                        if (retryable(*error))
                            static_cast<void>(repository.setStatus(
                                operation.operationId, waitStatus(*error), error->message));
                        co_return *error;
                    }
                    auto page = std::get<javelin::jmap::FullMailboxPage>(std::move(pageResult));
                    if (!baselineEmailState.has_value())
                        baselineEmailState = page.emailState;
                    else if (*baselineEmailState != page.emailState)
                        changedDuringEnumeration = true;
                    if (!queryState.has_value())
                        queryState = page.queryState;
                    else if (*queryState != page.queryState)
                        changedDuringEnumeration = true;
                    if (changedDuringEnumeration)
                        break;

                    std::vector<MailExportItemRecord> items;
                    items.reserve(page.emails.size());
                    for (const auto& email : page.emails)
                    {
                        std::optional<std::string> senderName;
                        std::optional<std::string> senderEmail;
                        if (!email.from.empty())
                        {
                            senderName = email.from.front().name;
                            if (!email.from.front().email.empty())
                                senderEmail = email.from.front().email;
                        }
                        items.push_back({
                            .itemId =
                                QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),
                            .ordinal = ordinal++,
                            .mailboxId = mailbox.mailboxId,
                            .emailId = email.id,
                            .blobId = email.blobId,
                            .size = email.size,
                            .subject = email.subject,
                            .receivedAt = email.receivedAt,
                            .senderName = std::move(senderName),
                            .senderEmail = std::move(senderEmail),
                            .phase = MailExportItemPhase::Pending,
                            .outputRelativePath = std::nullopt,
                            .rawContentHash = std::nullopt,
                            .mboxStartOffset = std::nullopt,
                            .mboxEndOffset = std::nullopt,
                            .lastError = std::nullopt,
                        });
                    }
                    if (const auto error = repository.appendItems(operation.operationId, items))
                        co_return javelin::jmap::operationError(*error);
                    position += page.emailIds.size();
                    if (page.emailIds.empty() ||
                        (page.total.has_value() && position >= *page.total))
                        break;
                }
                if (changedDuringEnumeration)
                    break;
            }

            if (!changedDuringEnumeration)
            {
                const auto state = baselineEmailState.value_or(std::string{});
                if (const auto error = repository.sealManifest(operation.operationId, state))
                    co_return javelin::jmap::operationError(*error);
                operation.manifestSealed = true;
                operation.status = MailExportStatus::Running;
                operation.manifestEmailState = state;
                static_cast<void>(writeMarker(operation, u"running"));
                co_return std::nullopt;
            }
        }

        co_return OperationError{
            .code = OperationErrorCode::PreconditionFailed,
            .message = i18n("Mail changed repeatedly while preparing the export. Try again when "
                            "the account is less active."),
        };
    }

    QCoro::Task<std::optional<OperationError>>
    MailExportService::writeNextItem(MailExportOperationRecord operation, std::string)
    {
        MailExportRepository repository{m_databaseConnection};
        auto nextResult = repository.nextIncompleteItem(operation.operationId);
        if (const auto* error = std::get_if<DatabaseError>(&nextResult))
            co_return javelin::jmap::operationError(*error);
        auto next = std::get<std::optional<MailExportItemRecord>>(std::move(nextResult));
        if (!next.has_value())
        {
            if (const auto error = finalizeExport(operation))
                co_return *error;
            co_return std::nullopt;
        }

        const auto mailboxesResult = repository.listMailboxes(operation.operationId);
        if (const auto* error = std::get_if<DatabaseError>(&mailboxesResult))
            co_return javelin::jmap::operationError(*error);
        const auto& mailboxes = std::get<std::vector<MailExportMailboxRecord>>(mailboxesResult);
        const auto mailbox =
            std::ranges::find(mailboxes, next->mailboxId, &MailExportMailboxRecord::mailboxId);
        if (mailbox == mailboxes.end())
        {
            co_return OperationError{.code = OperationErrorCode::ProtocolViolation,
                                     .message = i18n("The export manifest lost a mailbox record.")};
        }

        if (next->phase == MailExportItemPhase::Writing)
        {
            if (!next->rawContentHash.has_value() || !next->outputRelativePath.has_value())
            {
                co_return OperationError{.code = OperationErrorCode::ProtocolViolation,
                                         .message =
                                             i18n("The interrupted export item is invalid.")};
            }
            if (const auto error = validateExportRelativePath(operation, *next->outputRelativePath))
                co_return error;

            if (operation.format == MailExportFormat::Eml)
            {
                const auto targetPath = absoluteOutputPath(operation, *next->outputRelativePath);
                if (QFileInfo::exists(targetPath))
                {
                    auto hashFuture = QtConcurrent::run(hashFileSha256, targetPath);
                    const auto hash = co_await qCoro(hashFuture).takeResult();
                    if (!hash.error.isEmpty())
                    {
                        co_return OperationError{
                            .code = OperationErrorCode::LocalStorageFailure,
                            .message = i18n("Could not verify exported message %1: %2", targetPath,
                                            hash.error),
                        };
                    }
                    if (hash.sha256.toStdString() != *next->rawContentHash)
                    {
                        co_return OperationError{
                            .code = OperationErrorCode::PreconditionFailed,
                            .message =
                                i18n("The export target was modified externally: %1", targetPath),
                        };
                    }
                    if (const auto error = repository.markComplete(next->itemId))
                        co_return javelin::jmap::operationError(*error);
                    co_return std::nullopt;
                }
            }
            else
            {
                if (!next->outputRelativePath.has_value() || !next->mboxStartOffset.has_value())
                {
                    co_return OperationError{
                        .code = OperationErrorCode::ProtocolViolation,
                        .message = i18n("The interrupted mbox record has no recovery offset."),
                    };
                }
                const auto partPath = absoluteOutputPath(operation, *next->outputRelativePath) +
                                      QStringLiteral(".javelin-part");
                const auto committedResult =
                    repository.committedMboxSize(operation.operationId, next->mailboxId);
                if (const auto* error = std::get_if<DatabaseError>(&committedResult))
                    co_return javelin::jmap::operationError(*error);
                const auto committedSize = std::get<std::uint64_t>(committedResult);
                if (*next->mboxStartOffset != committedSize)
                {
                    co_return OperationError{
                        .code = OperationErrorCode::PreconditionFailed,
                        .message =
                            i18n("The mbox recovery checkpoint is inconsistent for %1.", partPath),
                    };
                }
                QFile part{partPath};
                if (!part.open(QIODevice::ReadWrite))
                {
                    co_return OperationError{.code = OperationErrorCode::LocalStorageFailure,
                                             .message = i18n("Could not recover interrupted mbox "
                                                             "output %1: %2",
                                                             partPath, part.errorString())};
                }
                if (static_cast<std::uint64_t>(part.size()) < committedSize)
                {
                    co_return OperationError{
                        .code = OperationErrorCode::PreconditionFailed,
                        .message = i18n("The export target was modified externally: %1", partPath),
                    };
                }
                if (!part.resize(static_cast<qint64>(committedSize)))
                {
                    co_return OperationError{.code = OperationErrorCode::LocalStorageFailure,
                                             .message = i18n("Could not recover interrupted mbox "
                                                             "output %1: %2",
                                                             partPath, part.errorString())};
                }
            }
            if (const auto error = repository.markSourceReady(next->itemId, *next->rawContentHash,
                                                              *next->outputRelativePath))
                co_return javelin::jmap::operationError(*error);
            co_return std::nullopt;
        }

        const auto settings = m_connectionProvider.connectionSettingsFor(operation.accountId);
        if (!settings.has_value())
        {
            co_return OperationError{.code = OperationErrorCode::NotFound,
                                     .message = i18n("The mail account is no longer available.")};
        }

        RawMailMaterializer rawMailMaterializer{m_databaseConnection, m_messageContentClient};
        auto materializedResult = co_await rawMailMaterializer.materialize(
            liveSettings(*settings), operation.accountId, next->emailId, next->blobId);
        if (const auto* error = std::get_if<OperationError>(&materializedResult))
        {
            if (retryable(*error))
            {
                static_cast<void>(repository.setStatus(operation.operationId, waitStatus(*error),
                                                       error->message));
                co_return *error;
            }
            static_cast<void>(repository.markFailed(next->itemId, error->message));
            co_return std::nullopt;
        }
        auto materialized = std::get<MaterializedRawMail>(std::move(materializedResult));

        const auto relativePath = outputRelativePath(operation, *mailbox, *next);
        if (const auto error =
                repository.markSourceReady(next->itemId, materialized.contentHash, relativePath))
            co_return javelin::jmap::operationError(*error);
        next->rawContentHash = materialized.contentHash;
        next->outputRelativePath = relativePath;
        next->phase = MailExportItemPhase::SourceReady;

        if (const auto spaceError = availableSpaceError(operation.destinationDirectory, next->size))
        {
            static_cast<void>(repository.setStatus(
                operation.operationId, MailExportStatus::WaitingForSpace, spaceError->message));
            co_return std::nullopt;
        }

        const auto& sourcePath = materialized.filePath;
        if (const auto error = validateExportRelativePath(operation, relativePath))
            co_return error;
        const auto targetPath = absoluteOutputPath(operation, relativePath);
        const QFileInfo targetInfo{targetPath};
        QDir root{operation.destinationDirectory};
        const auto relativeParent = root.relativeFilePath(targetInfo.path());
        if (relativeParent != QStringLiteral(".") && !root.mkpath(relativeParent))
        {
            co_return OperationError{
                .code = OperationErrorCode::LocalStorageFailure,
                .message = i18n("Could not create export directory %1.", targetInfo.path())};
        }

        if (operation.format == MailExportFormat::Eml && QFileInfo::exists(targetPath))
        {
            auto hashFuture = QtConcurrent::run(hashFileSha256, targetPath);
            const auto hash = co_await qCoro(hashFuture).takeResult();
            if (!hash.error.isEmpty())
            {
                co_return OperationError{
                    .code = OperationErrorCode::LocalStorageFailure,
                    .message =
                        i18n("Could not verify exported message %1: %2", targetPath, hash.error),
                };
            }
            if (hash.sha256.toStdString() != materialized.contentHash)
            {
                co_return OperationError{
                    .code = OperationErrorCode::PreconditionFailed,
                    .message = i18n("The export target was modified externally: %1", targetPath),
                };
            }
            if (const auto error = repository.markComplete(next->itemId))
                co_return javelin::jmap::operationError(*error);
            co_return std::nullopt;
        }

        std::optional<std::uint64_t> mboxStartOffset;
        QString writePath = targetPath;
        if (operation.format == MailExportFormat::MboxRd)
        {
            writePath += QStringLiteral(".javelin-part");
            const auto committedResult =
                repository.committedMboxSize(operation.operationId, next->mailboxId);
            if (const auto* error = std::get_if<DatabaseError>(&committedResult))
                co_return javelin::jmap::operationError(*error);
            const auto committedSize = std::get<std::uint64_t>(committedResult);
            const QFileInfo partInfo{writePath};
            const auto actualSize =
                partInfo.exists() ? static_cast<std::uint64_t>(partInfo.size()) : 0;
            if (actualSize != committedSize)
            {
                co_return OperationError{
                    .code = OperationErrorCode::PreconditionFailed,
                    .message = i18n("The export target was modified externally: %1", writePath),
                };
            }
            mboxStartOffset = committedSize;
        }
        if (const auto error = repository.markWriting(next->itemId, mboxStartOffset))
            co_return javelin::jmap::operationError(*error);

        QFuture<MailExportFileWriteResult> writeFuture;
        if (operation.format == MailExportFormat::Eml)
        {
            writeFuture = QtConcurrent::run(copyEmlFile, sourcePath, writePath);
        }
        else
        {
            writeFuture = QtConcurrent::run(appendMboxRdRecord, sourcePath, writePath,
                                            next->senderEmail, next->receivedAt);
        }
        const auto written = co_await qCoro(writeFuture).takeResult();
        if (!written.error.isEmpty())
        {
            if (operation.format == MailExportFormat::MboxRd && mboxStartOffset.has_value())
            {
                QFile part{writePath};
                if (part.open(QIODevice::ReadWrite))
                    static_cast<void>(part.resize(static_cast<qint64>(*mboxStartOffset)));
            }
            if (written.fileError == QFileDevice::ResourceError)
            {
                static_cast<void>(repository.setStatus(
                    operation.operationId, MailExportStatus::WaitingForSpace, written.error));
                co_return std::nullopt;
            }
            static_cast<void>(repository.markFailed(
                next->itemId, i18n("Could not write %1: %2", written.path, written.error)));
            co_return std::nullopt;
        }
        std::optional<std::uint64_t> mboxEndOffset;
        if (operation.format == MailExportFormat::MboxRd)
        {
            const QFileInfo completedPart{writePath};
            if (!completedPart.exists() || completedPart.size() < 0)
            {
                co_return OperationError{
                    .code = OperationErrorCode::LocalStorageFailure,
                    .message = i18n("Could not verify completed mbox output %1.", writePath),
                };
            }
            mboxEndOffset = static_cast<std::uint64_t>(completedPart.size());
        }
        if (const auto error = repository.markComplete(next->itemId, mboxEndOffset))
            co_return javelin::jmap::operationError(*error);
        co_return std::nullopt;
    }

    std::optional<OperationError>
    MailExportService::finalizeExport(const MailExportOperationRecord& operation)
    {
        MailExportRepository repository{m_databaseConnection};
        const auto mailboxesResult = repository.listMailboxes(operation.operationId);
        if (const auto* error = std::get_if<DatabaseError>(&mailboxesResult))
            return javelin::jmap::operationError(*error);
        const auto& mailboxes = std::get<std::vector<MailExportMailboxRecord>>(mailboxesResult);
        if (operation.format == MailExportFormat::MboxRd)
        {
            if (const auto error = publishMboxFiles(operation, mailboxes))
                return error;
        }
        const auto progressResult = repository.progress(operation.operationId);
        if (const auto* error = std::get_if<DatabaseError>(&progressResult))
            return javelin::jmap::operationError(*error);
        const auto progress = std::get<MailExportProgressSnapshot>(progressResult);
        const auto status =
            progress.failedItems == 0 ? MailExportStatus::Complete : MailExportStatus::Partial;
        const auto statusText = status == MailExportStatus::Complete ? u"complete" : u"partial";
        if (const auto error = writeMarker(operation, statusText))
            return error;
        const auto summary = progress.failedItems == 0
                                 ? std::optional<QString>{std::nullopt}
                                 : std::optional<QString>{i18np(
                                       "One message could not be exported.",
                                       "%1 messages could not be exported.", progress.failedItems)};
        if (const auto error = repository.setStatus(operation.operationId, status, summary))
            return javelin::jmap::operationError(*error);
        return std::nullopt;
    }
} // namespace javelin::app
