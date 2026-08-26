#include "gui/shell/MessageFileController.h"

#include "app/MailSaveNaming.h"
#include "app/MessageContentApplicationPorts.h"
#include "gui/settings/GuiSettings.h"
#include "gui/shell/MessageFileUtils.h"
#include "jmap/cache/MessageViewReader.h"

#include <QCoroTask>

#include <KLocalizedString>

#include <KIO/ApplicationLauncherJob>
#include <KIO/JobUiDelegateFactory>
#include <KIO/OpenUrlJob>
#include <KJob>
#include <KUiServerV2JobTracker>

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QDrag>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMimeData>
#include <QTimer>
#include <QUrl>
#include <QtConcurrentRun>

#include <ranges>
#include <utility>

namespace javelin::gui::shell
{
    namespace
    {
        class FileSaveJob final : public KJob
        {
          public:
            FileSaveJob(KUiServerV2JobTracker& tracker, QString activeTitle, QString completedTitle,
                        QString destinationPath, QObject* parent)
                : KJob(parent), m_tracker(tracker), m_activeTitle(std::move(activeTitle)),
                  m_completedTitle(std::move(completedTitle))
            {
                setProperty("desktopFileName", QStringLiteral("javelinmail"));
                setDestination(std::move(destinationPath));
            }

            void start() override
            {
                startElapsedTimer();
                // Plasma's tracker suppresses jobs that finish during its own delay. Delay
                // registration here instead so short saves still get a completed-file entry,
                // while progress UI appears only when the operation lasts long enough to matter.
                QTimer::singleShot(500, this, [this] { ensureRegistered(); });
            }

            void setDestination(QString path)
            {
                m_destinationPath = std::move(path);
                if (!m_destinationPath.isEmpty())
                {
                    setProperty("destUrl", QUrl::fromLocalFile(m_destinationPath).toString());
                }
                if (m_registered)
                    publishDescription(m_activeTitle);
            }

            void finishSuccessfully()
            {
                setPercent(100);
                if (!m_registered)
                    ensureRegistered();
                publishDescription(m_completedTitle);
                emitResult();
            }

            void finishWithError(const QString& errorText)
            {
                if (!m_registered)
                {
                    deleteLater();
                    return;
                }
                setError(KJob::UserDefinedError);
                setErrorText(errorText);
                emitResult();
            }

          private:
            void ensureRegistered()
            {
                if (m_registered)
                    return;
                setProperty("immediateProgressReporting", true);
                m_registered = true;
                m_tracker.registerJob(this);
                publishDescription(m_activeTitle);
            }

            void publishDescription(const QString& title)
            {
                const QPair<QString, QString> destination =
                    m_destinationPath.isEmpty()
                        ? QPair<QString, QString>{}
                        : QPair<QString, QString>{
                              i18nc("@label file job destination", "Destination"),
                              m_destinationPath};
                Q_EMIT description(this, title, destination, {});
            }

            KUiServerV2JobTracker& m_tracker;
            QString m_activeTitle;
            QString m_completedTitle;
            QString m_destinationPath;
            bool m_registered = false;
        };
    } // namespace

    MessageFileController::MessageFileController(
        javelin::gui::settings::GuiSettings& settings,
        javelin::app::MessageContentPort& contentPort,
        javelin::jmap::cache::MessageViewReader& messageViewReader, QWidget* dialogParent,
        QObject* parent)
        : QObject(parent), m_settings(settings), m_contentPort(contentPort),
          m_messageViewReader(messageViewReader), m_dialogParent(dialogParent),
          m_externalDragRootPath(defaultExternalDragRootPath())
    {
        m_fileJobTracker = new KUiServerV2JobTracker(this);
        cleanupExpiredExternalDragDirectories(m_externalDragRootPath,
                                              QDateTime::currentMSecsSinceEpoch());
    }

    void MessageFileController::saveAttachment(std::string accountId, std::string emailId,
                                               std::string partId)
    {
        const auto attachmentSettings = m_settings.attachmentSaveSettings();
        if (!attachmentSettings.alwaysAsk && (attachmentSettings.directory.isEmpty() ||
                                              !QDir{attachmentSettings.directory}.exists()))
        {
            Q_EMIT userInterventionRequired(
                i18n("Select a valid attachment save directory in Preferences."));
            return;
        }

        const auto snapshotResult = m_messageViewReader.load(accountId, emailId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&snapshotResult))
        {
            Q_EMIT userInterventionRequired(error->message);
            return;
        }
        const auto& snapshot =
            std::get<std::optional<javelin::jmap::cache::MessageViewSnapshot>>(snapshotResult);
        if (!snapshot.has_value())
        {
            Q_EMIT statusMessage(i18n("The selected message is unavailable."), 5000);
            return;
        }

        const auto downloadableAttachments = visibleDownloadableAttachments(*snapshot);
        const auto attachment = std::ranges::find(downloadableAttachments, partId,
                                                  &javelin::jmap::cache::MessageAttachment::partId);
        if (attachment == downloadableAttachments.end())
        {
            Q_EMIT statusMessage(i18n("The selected attachment is not downloadable."), 5000);
            return;
        }
        const QString suggestedName = suggestedFileName(*attachment);

        const QString targetPath =
            attachmentSettings.alwaysAsk
                ? QFileDialog::getSaveFileName(m_dialogParent, i18n("Save Attachment"),
                                               suggestedName)
                : uniqueFilePath(attachmentSettings.directory, suggestedName);
        if (targetPath.isEmpty())
        {
            Q_EMIT statusMessage(i18n("Attachment save canceled."), 3000);
            return;
        }

        auto* fileJob = new FileSaveJob(*m_fileJobTracker, i18n("Saving attachment"),
                                        i18n("Attachment saved"), targetPath, this);
        fileJob->start();
        Q_EMIT statusMessage(i18n("Downloading attachment..."), 0);
        auto task = m_contentPort.requestAttachment(std::move(accountId), std::move(emailId),
                                                    std::move(partId));
        QCoro::connect(
            std::move(task), this,
            [this, fileJob, targetPath](javelin::jmap::AttachmentDownloadResult result)
            {
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    fileJob->finishWithError(error->message);
                    Q_EMIT operationFailed(*error);
                    return;
                }

                const auto& download = std::get<javelin::jmap::AttachmentDownload>(result);
                Q_EMIT statusMessage(i18n("Saving attachment..."), 0);
                auto* watcher = new QFutureWatcher<FileWriteResult>(this);
                connect(watcher, &QFutureWatcher<FileWriteResult>::finished, this,
                        [this, watcher, fileJob]
                        {
                            const auto writeResult = watcher->result();
                            watcher->deleteLater();
                            if (!writeResult.errorMessage.isEmpty())
                            {
                                fileJob->finishWithError(writeResult.errorMessage);
                                Q_EMIT userInterventionRequired(i18n(
                                    "Failed to save attachment: %1", writeResult.errorMessage));
                                return;
                            }

                            fileJob->finishSuccessfully();
                            Q_EMIT statusMessage(i18n("Saved attachment to %1", writeResult.path),
                                                 5000);
                        });
                watcher->setFuture(
                    QtConcurrent::run([targetPath, payload = download.payload]
                                      { return writePayloadToPath(targetPath, payload); }));
            });
    }

    void MessageFileController::saveAllAttachments(std::string accountId, std::string emailId)
    {
        const auto snapshotResult = m_messageViewReader.load(accountId, emailId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&snapshotResult))
        {
            Q_EMIT userInterventionRequired(error->message);
            return;
        }

        const auto& snapshot =
            std::get<std::optional<javelin::jmap::cache::MessageViewSnapshot>>(snapshotResult);
        if (!snapshot.has_value())
        {
            Q_EMIT statusMessage(i18n("The selected message is unavailable."), 5000);
            return;
        }

        const auto attachments = visibleDownloadableAttachments(*snapshot);
        if (attachments.empty())
        {
            Q_EMIT statusMessage(i18n("No downloadable attachments are available."), 5000);
            return;
        }

        const auto attachmentSettings = m_settings.attachmentSaveSettings();
        const QString targetDirectory =
            attachmentSettings.alwaysAsk
                ? QFileDialog::getExistingDirectory(m_dialogParent, i18n("Save All Attachments"),
                                                    QDir::homePath())
                : attachmentSettings.directory;
        if (targetDirectory.isEmpty())
        {
            Q_EMIT statusMessage(i18n("Save all attachments canceled."), 3000);
            return;
        }
        if (!QDir{targetDirectory}.exists())
        {
            Q_EMIT userInterventionRequired(
                i18n("Select a valid attachment save directory in Preferences."));
            return;
        }

        auto* fileJob = new FileSaveJob(*m_fileJobTracker, i18n("Saving attachments"),
                                        i18n("Attachments saved"), targetDirectory, this);
        fileJob->start();
        Q_EMIT statusMessage(i18n("Downloading attachments..."), 0);
        auto task = downloadAttachments(m_contentPort, accountId, emailId, attachments);
        QCoro::connect(std::move(task), this,
                       [this, fileJob, targetDirectory](SaveAllDownloadResult result)
                       {
                           if (!result.errorMessage.isEmpty())
                           {
                               fileJob->finishWithError(result.errorMessage);
                               Q_EMIT userInterventionRequired(result.errorMessage);
                               return;
                           }

                           Q_EMIT statusMessage(i18n("Saving attachments..."), 0);
                           auto* watcher = new QFutureWatcher<BatchWriteResult>(this);
                           connect(watcher, &QFutureWatcher<BatchWriteResult>::finished, this,
                                   [this, watcher, fileJob]
                                   {
                                       const auto writeResult = watcher->result();
                                       watcher->deleteLater();
                                       if (!writeResult.errorMessage.isEmpty())
                                       {
                                           fileJob->finishWithError(writeResult.errorMessage);
                                           Q_EMIT userInterventionRequired(i18n(
                                               "Failed to save attachments to %1: %2",
                                               writeResult.failedPath, writeResult.errorMessage));
                                           return;
                                       }

                                       fileJob->finishSuccessfully();
                                       Q_EMIT statusMessage(i18np("Saved %1 attachment.",
                                                                  "Saved %1 attachments.",
                                                                  writeResult.savedCount),
                                                            5000);
                                   });
                           watcher->setFuture(QtConcurrent::run(
                               [targetDirectory, files = std::move(result.files)]
                               { return writePayloadBatchToDirectory(targetDirectory, files); }));
                       });
    }

    void MessageFileController::dragAttachment(QString accountId, QString emailId, QString partId,
                                               QWidget* source)
    {
        if (source == nullptr || accountId.isEmpty() || emailId.isEmpty() || partId.isEmpty())
            return;

        const auto key = attachmentDragKey(accountId, emailId, partId);
        const auto cached = m_preparedAttachmentDragFiles.constFind(key);
        if (cached != m_preparedAttachmentDragFiles.cend() && cached->isLocalFile() &&
            QFileInfo::exists(cached->toLocalFile()))
        {
            startExternalFileDrag(source, {*cached});
            return;
        }

        const auto directory = createExternalDragDirectory(m_externalDragRootPath);
        if (!directory.errorMessage.isEmpty())
        {
            Q_EMIT userInterventionRequired(
                i18n("Failed to prepare attachment drag: %1", directory.errorMessage));
            return;
        }

        QPointer<QWidget> sourceGuard{source};
        Q_EMIT statusMessage(i18n("Preparing attachment for drag…"), 0);
        auto task = m_contentPort.requestAttachment(accountId.toStdString(), emailId.toStdString(),
                                                    partId.toStdString());
        QCoro::connect(
            std::move(task), this,
            [this, sourceGuard, key,
             directoryPath = directory.path](javelin::jmap::AttachmentDownloadResult result)
            {
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    QDir{directoryPath}.removeRecursively();
                    Q_EMIT operationFailed(*error);
                    return;
                }

                const auto& download = std::get<javelin::jmap::AttachmentDownload>(result);
                const auto targetPath = QDir{directoryPath}.filePath(suggestedFileName(download));
                auto* watcher = new QFutureWatcher<FileWriteResult>(this);
                connect(watcher, &QFutureWatcher<FileWriteResult>::finished, this,
                        [this, watcher, sourceGuard, key, directoryPath]
                        {
                            const auto writeResult = watcher->result();
                            watcher->deleteLater();
                            if (!writeResult.errorMessage.isEmpty())
                            {
                                QDir{directoryPath}.removeRecursively();
                                Q_EMIT userInterventionRequired(
                                    i18n("Failed to prepare attachment drag: %1",
                                         writeResult.errorMessage));
                                return;
                            }

                            const QUrl url = QUrl::fromLocalFile(writeResult.path);
                            m_preparedAttachmentDragFiles.insert(key, url);
                            if (sourceGuard != nullptr &&
                                QApplication::mouseButtons().testFlag(Qt::LeftButton))
                            {
                                startExternalFileDrag(sourceGuard, {url});
                                return;
                            }
                            Q_EMIT statusMessage(i18n("Attachment is ready; drag it again."), 5000);
                        });
                watcher->setFuture(
                    QtConcurrent::run([targetPath, payload = download.payload]
                                      { return writePayloadToPath(targetPath, payload); }));
            });
    }

    QList<QUrl> MessageFileController::materializeMessageDragFiles(
        const javelin::gui::messages::MessageDragPayload& payload)
    {
        const auto directory = createExternalDragDirectory(m_externalDragRootPath);
        if (!directory.errorMessage.isEmpty())
        {
            Q_EMIT userInterventionRequired(
                i18n("Failed to prepare message drag: %1", directory.errorMessage));
            return {};
        }

        Q_EMIT statusMessage(i18n("Preparing messages for drag…"), 0);
        auto task = m_contentPort.saveMessages({
            .accountId = payload.sourceAccountId,
            .sourceMailboxId = payload.sourceMailboxId,
            .selection = payload.selection,
            .targetKind = javelin::app::MessageSaveTargetKind::Directory,
            .destinationPath = directory.path,
        });

        std::optional<javelin::app::SaveMessagesResult> completedResult;
        QEventLoop completionLoop;
        QCoro::connect(std::move(task), &completionLoop,
                       [&completedResult, &completionLoop](javelin::app::SaveMessagesResult result)
                       {
                           completedResult = std::move(result);
                           completionLoop.quit();
                       });
        if (!completedResult.has_value())
            completionLoop.exec(QEventLoop::ExcludeUserInputEvents);

        if (!completedResult.has_value())
        {
            QDir{directory.path}.removeRecursively();
            Q_EMIT userInterventionRequired(i18n("Message drag preparation did not complete."));
            return {};
        }
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&*completedResult))
        {
            QDir{directory.path}.removeRecursively();
            Q_EMIT operationFailed(*error);
            return {};
        }

        const auto& saved = std::get<javelin::app::SaveMessagesSummary>(*completedResult);
        auto urls = externalDragFileUrls(directory.path);
        if (urls.size() != static_cast<qsizetype>(saved.savedMessageCount) || urls.isEmpty())
        {
            QDir{directory.path}.removeRecursively();
            Q_EMIT userInterventionRequired(
                i18n("Failed to prepare the complete message selection for dragging."));
            return {};
        }

        Q_EMIT statusMessage(i18n("Messages ready to drag."), 2000);
        return urls;
    }

    void MessageFileController::saveMessages(std::string accountId,
                                             std::optional<std::string> sourceMailboxId,
                                             javelin::app::MessageSelection selection)
    {
        if (selection.empty())
        {
            Q_EMIT statusMessage(i18n("Select a message to save."), 3000);
            return;
        }

        const auto* singleEmail = selection.size() == 1
                                      ? std::get_if<javelin::app::SelectedEmail>(&selection.front())
                                      : nullptr;
        javelin::app::MessageSaveTargetKind targetKind =
            javelin::app::MessageSaveTargetKind::Directory;
        QString destinationPath;
        if (singleEmail != nullptr)
        {
            QString suggestedName = QStringLiteral("message.eml");
            const auto snapshotResult = m_messageViewReader.load(accountId, singleEmail->emailId);
            if (const auto* snapshot =
                    std::get_if<std::optional<javelin::jmap::cache::MessageViewSnapshot>>(
                        &snapshotResult);
                snapshot != nullptr && snapshot->has_value())
            {
                suggestedName = javelin::app::suggestedMailSaveFileName((*snapshot)->email);
            }
            destinationPath =
                QFileDialog::getSaveFileName(m_dialogParent, i18n("Save Message As"), suggestedName,
                                             i18n("Email files (*.eml);;All files (*)"));
            targetKind = javelin::app::MessageSaveTargetKind::SingleFile;
        }
        else
        {
            destinationPath = QFileDialog::getExistingDirectory(
                m_dialogParent, i18n("Save Messages"), QDir::homePath());
        }
        if (destinationPath.isEmpty())
            return;

        auto* fileJob = new FileSaveJob(
            *m_fileJobTracker,
            singleEmail != nullptr ? i18n("Saving message") : i18n("Saving messages"),
            singleEmail != nullptr ? i18n("Message saved") : i18n("Messages saved"),
            destinationPath, this);
        fileJob->start();
        Q_EMIT statusMessage(
            singleEmail != nullptr ? i18n("Saving message…") : i18n("Saving messages…"), 0);
        auto task = m_contentPort.saveMessages({
            .accountId = std::move(accountId),
            .sourceMailboxId = std::move(sourceMailboxId),
            .selection = std::move(selection),
            .targetKind = targetKind,
            .destinationPath = destinationPath,
        });
        QCoro::connect(
            std::move(task), this,
            [this, fileJob](javelin::app::SaveMessagesResult result)
            {
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    fileJob->finishWithError(error->message);
                    Q_EMIT operationFailed(*error);
                    return;
                }
                const auto& saved = std::get<javelin::app::SaveMessagesSummary>(result);
                fileJob->setDestination(saved.destinationPath);
                fileJob->finishSuccessfully();
                Q_EMIT statusMessage(i18np("Saved one message to %2.", "Saved %1 messages to %2.",
                                           saved.savedMessageCount, saved.destinationPath),
                                     5000);
            });
    }

    void MessageFileController::openAttachment(std::string accountId, std::string emailId,
                                               std::string partId)
    {
        openAttachment(std::move(accountId), std::move(emailId), std::move(partId), false);
    }

    void MessageFileController::openAttachmentWith(std::string accountId, std::string emailId,
                                                   std::string partId)
    {
        openAttachment(std::move(accountId), std::move(emailId), std::move(partId), true);
    }

    void MessageFileController::openAttachment(std::string accountId, std::string emailId,
                                               std::string partId, const bool chooseApplication)
    {
        Q_EMIT statusMessage(i18n("Downloading attachment..."), 0);
        auto task = m_contentPort.requestAttachment(std::move(accountId), std::move(emailId),
                                                    std::move(partId));
        QCoro::connect(
            std::move(task), this,
            [this, chooseApplication](javelin::jmap::AttachmentDownloadResult result)
            {
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    Q_EMIT operationFailed(*error);
                    return;
                }

                const auto& download = std::get<javelin::jmap::AttachmentDownload>(result);
                const auto fileName = suggestedFileName(download);
                Q_EMIT statusMessage(i18n("Preparing attachment..."), 0);

                auto* watcher = new QFutureWatcher<FileWriteResult>(this);
                connect(watcher, &QFutureWatcher<FileWriteResult>::finished, this,
                        [this, watcher, chooseApplication]
                        {
                            const auto writeResult = watcher->result();
                            watcher->deleteLater();
                            if (!writeResult.errorMessage.isEmpty())
                            {
                                Q_EMIT userInterventionRequired(i18n(
                                    "Failed to prepare attachment: %1", writeResult.errorMessage));
                                return;
                            }

                            launchTemporaryFile(writeResult.path, chooseApplication,
                                                i18n("Opened attachment."));
                        });
                watcher->setFuture(
                    QtConcurrent::run([fileName, payload = download.payload]
                                      { return writePayloadToTemporaryFile(fileName, payload); }));
            });
    }

    void MessageFileController::startExternalFileDrag(QWidget* source, const QList<QUrl>& urls)
    {
        if (source == nullptr || urls.isEmpty())
            return;
        auto* mimeData = new QMimeData;
        mimeData->setUrls(urls);
        QDrag drag{source};
        drag.setMimeData(mimeData);
        static_cast<void>(drag.exec(Qt::CopyAction, Qt::CopyAction));
    }

    QString MessageFileController::attachmentDragKey(const QString& accountId,
                                                     const QString& emailId,
                                                     const QString& partId) const
    {
        return accountId + QChar{0x1f} + emailId + QChar{0x1f} + partId;
    }

    void MessageFileController::launchTemporaryFile(QString path, const bool chooseApplication,
                                                    QString successMessage)
    {
        KJob* job = nullptr;
        if (chooseApplication)
        {
            auto* launcher = new KIO::ApplicationLauncherJob(this);
            launcher->setUrls({QUrl::fromLocalFile(path)});
            launcher->setRunFlags(KIO::ApplicationLauncherJob::DeleteTemporaryFiles);
            launcher->setUiDelegate(KIO::createDefaultJobUiDelegate(
                KJobUiDelegate::AutoHandlingEnabled, m_dialogParent.data()));
            job = launcher;
        }
        else
        {
            auto* opener = new KIO::OpenUrlJob(QUrl::fromLocalFile(path), this);
            opener->setDeleteTemporaryFile(true);
            opener->setRunExecutables(false);
            opener->setUiDelegate(KIO::createDefaultJobUiDelegate(
                KJobUiDelegate::AutoHandlingEnabled, m_dialogParent.data()));
            job = opener;
        }

        connect(job, &KJob::result, this,
                [this, path = std::move(path),
                 successMessage = std::move(successMessage)](KJob* completedJob)
                {
                    if (completedJob->error() != 0)
                    {
                        QFile::remove(path);
                        Q_EMIT statusMessage(i18n("The file was not opened."), 3000);
                        return;
                    }
                    Q_EMIT statusMessage(successMessage, 5000);
                });
        job->start();
    }

    void MessageFileController::viewMessageSource(std::string accountId, std::string emailId)
    {
        Q_EMIT statusMessage(i18n("Preparing message source..."), 0);
        auto task = m_contentPort.requestMessageSource(std::move(accountId), std::move(emailId));
        QCoro::connect(
            std::move(task), this,
            [this](javelin::jmap::MessageSourceDownloadResult result)
            {
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    Q_EMIT operationFailed(*error);
                    return;
                }

                const auto& download = std::get<javelin::jmap::MessageSourceDownload>(result);
                const auto fileName = suggestedSourceFileName(download);
                auto* watcher = new QFutureWatcher<FileWriteResult>(this);
                connect(watcher, &QFutureWatcher<FileWriteResult>::finished, this,
                        [this, watcher]
                        {
                            const auto writeResult = watcher->result();
                            watcher->deleteLater();
                            if (!writeResult.errorMessage.isEmpty())
                            {
                                Q_EMIT userInterventionRequired(
                                    i18n("Failed to prepare message source: %1",
                                         writeResult.errorMessage));
                                return;
                            }

                            launchTemporaryFile(writeResult.path, false,
                                                i18n("Opened message source."));
                        });
                watcher->setFuture(
                    QtConcurrent::run([fileName, payload = download.payload]
                                      { return writePayloadToTemporaryFile(fileName, payload); }));
            });
    }

} // namespace javelin::gui::shell
