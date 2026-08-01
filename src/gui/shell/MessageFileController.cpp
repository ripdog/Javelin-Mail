#include "gui/shell/MessageFileController.h"

#include "app/MessageContentApplicationPorts.h"
#include "gui/settings/GuiSettings.h"
#include "gui/shell/MessageFileUtils.h"
#include "jmap/cache/MessageViewReader.h"

#include <QCoroTask>

#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QUrl>
#include <QtConcurrentRun>

namespace javelin::gui::shell
{

    MessageFileController::MessageFileController(
        javelin::gui::settings::GuiSettings& settings,
        javelin::app::MessageContentPort& contentPort,
        javelin::jmap::cache::MessageViewReader& messageViewReader, QWidget* dialogParent,
        QObject* parent)
        : QObject(parent), m_settings(settings), m_contentPort(contentPort),
          m_messageViewReader(messageViewReader), m_dialogParent(dialogParent)
    {
    }

    void MessageFileController::saveAttachment(std::string accountId, std::string emailId,
                                               std::string partId)
    {
        const auto attachmentSettings = m_settings.attachmentSaveSettings();
        if (!attachmentSettings.alwaysAsk && (attachmentSettings.directory.isEmpty() ||
                                              !QDir{attachmentSettings.directory}.exists()))
        {
            Q_EMIT userInterventionRequired(
                QStringLiteral("Select a valid attachment save directory in Preferences."));
            return;
        }

        Q_EMIT statusMessage(QStringLiteral("Downloading attachment..."), 0);
        auto task = m_contentPort.requestAttachment(std::move(accountId), std::move(emailId),
                                                    std::move(partId));
        QCoro::connect(
            std::move(task), this,
            [this, attachmentSettings](javelin::jmap::AttachmentDownloadResult result)
            {
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    Q_EMIT operationFailed(*error);
                    return;
                }

                const auto& download = std::get<javelin::jmap::AttachmentDownload>(result);
                const QString targetPath =
                    attachmentSettings.alwaysAsk
                        ? QFileDialog::getSaveFileName(m_dialogParent,
                                                       QStringLiteral("Save Attachment"),
                                                       suggestedFileName(download))
                        : uniqueFilePath(attachmentSettings.directory, suggestedFileName(download));
                if (targetPath.isEmpty())
                {
                    Q_EMIT statusMessage(QStringLiteral("Attachment save canceled."), 3000);
                    return;
                }

                Q_EMIT statusMessage(QStringLiteral("Saving attachment..."), 0);
                auto* watcher = new QFutureWatcher<FileWriteResult>(this);
                connect(watcher, &QFutureWatcher<FileWriteResult>::finished, this,
                        [this, watcher]
                        {
                            const auto writeResult = watcher->result();
                            if (!writeResult.errorMessage.isEmpty())
                            {
                                Q_EMIT userInterventionRequired(
                                    QStringLiteral("Failed to save attachment: %1")
                                        .arg(writeResult.errorMessage));
                            }
                            else
                            {
                                Q_EMIT statusMessage(
                                    QStringLiteral("Saved attachment to %1").arg(writeResult.path),
                                    5000);
                            }
                            watcher->deleteLater();
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
            Q_EMIT statusMessage(QStringLiteral("The selected message is unavailable."), 5000);
            return;
        }

        const auto attachments = visibleDownloadableAttachments(*snapshot);
        if (attachments.empty())
        {
            Q_EMIT statusMessage(QStringLiteral("No downloadable attachments are available."),
                                 5000);
            return;
        }

        const auto attachmentSettings = m_settings.attachmentSaveSettings();
        const QString targetDirectory =
            attachmentSettings.alwaysAsk
                ? QFileDialog::getExistingDirectory(
                      m_dialogParent, QStringLiteral("Save All Attachments"), QDir::homePath())
                : attachmentSettings.directory;
        if (targetDirectory.isEmpty())
        {
            Q_EMIT statusMessage(QStringLiteral("Save all attachments canceled."), 3000);
            return;
        }
        if (!QDir{targetDirectory}.exists())
        {
            Q_EMIT userInterventionRequired(
                QStringLiteral("Select a valid attachment save directory in Preferences."));
            return;
        }

        Q_EMIT statusMessage(QStringLiteral("Downloading attachments..."), 0);
        auto task = downloadAttachments(m_contentPort, accountId, emailId, attachments);
        QCoro::connect(
            std::move(task), this,
            [this, targetDirectory](SaveAllDownloadResult result)
            {
                if (!result.errorMessage.isEmpty())
                {
                    Q_EMIT userInterventionRequired(result.errorMessage);
                    return;
                }

                Q_EMIT statusMessage(QStringLiteral("Saving attachments..."), 0);
                auto* watcher = new QFutureWatcher<BatchWriteResult>(this);
                connect(
                    watcher, &QFutureWatcher<BatchWriteResult>::finished, this,
                    [this, watcher]
                    {
                        const auto writeResult = watcher->result();
                        if (!writeResult.errorMessage.isEmpty())
                        {
                            Q_EMIT userInterventionRequired(
                                QStringLiteral("Failed to save attachments to %1: %2")
                                    .arg(writeResult.failedPath, writeResult.errorMessage));
                        }
                        else
                        {
                            Q_EMIT statusMessage(
                                QStringLiteral("Saved %1 attachments.").arg(writeResult.savedCount),
                                5000);
                        }
                        watcher->deleteLater();
                    });
                watcher->setFuture(QtConcurrent::run(
                    [targetDirectory, files = std::move(result.files)]
                    { return writePayloadBatchToDirectory(targetDirectory, files); }));
            });
    }

    void MessageFileController::openAttachment(std::string accountId, std::string emailId,
                                               std::string partId)
    {
        if (!m_temporaryDirectory.isValid())
        {
            Q_EMIT userInterventionRequired(
                QStringLiteral("A temporary directory for attachments is unavailable."));
            return;
        }

        Q_EMIT statusMessage(QStringLiteral("Downloading attachment..."), 0);
        auto task = m_contentPort.requestAttachment(std::move(accountId), std::move(emailId),
                                                    std::move(partId));
        QCoro::connect(
            std::move(task), this,
            [this](javelin::jmap::AttachmentDownloadResult result)
            {
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    Q_EMIT operationFailed(*error);
                    return;
                }

                const auto& download = std::get<javelin::jmap::AttachmentDownload>(result);
                const QString targetPath = tempAttachmentPath(m_temporaryDirectory, download);
                Q_EMIT statusMessage(QStringLiteral("Preparing attachment..."), 0);

                auto* watcher = new QFutureWatcher<FileWriteResult>(this);
                connect(watcher, &QFutureWatcher<FileWriteResult>::finished, this,
                        [this, watcher]
                        {
                            const auto writeResult = watcher->result();
                            if (!writeResult.errorMessage.isEmpty())
                            {
                                Q_EMIT userInterventionRequired(
                                    QStringLiteral("Failed to prepare attachment: %1")
                                        .arg(writeResult.errorMessage));
                                watcher->deleteLater();
                                return;
                            }

                            const bool opened =
                                QDesktopServices::openUrl(QUrl::fromLocalFile(writeResult.path));
                            Q_EMIT statusMessage(
                                opened ? QStringLiteral("Opened attachment.")
                                       : QStringLiteral(
                                             "The attachment was saved, but no app opened it."),
                                5000);
                            watcher->deleteLater();
                        });
                watcher->setFuture(
                    QtConcurrent::run([targetPath, payload = download.payload]
                                      { return writePayloadToPath(targetPath, payload); }));
            });
    }

    void MessageFileController::viewMessageSource(std::string accountId, std::string emailId)
    {
        if (!m_temporaryDirectory.isValid())
        {
            Q_EMIT userInterventionRequired(
                QStringLiteral("A temporary directory for message source files is unavailable."));
            return;
        }

        Q_EMIT statusMessage(QStringLiteral("Preparing message source..."), 0);
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
                const QString targetPath = tempMessageSourcePath(m_temporaryDirectory, download);
                auto* watcher = new QFutureWatcher<FileWriteResult>(this);
                connect(watcher, &QFutureWatcher<FileWriteResult>::finished, this,
                        [this, watcher]
                        {
                            const auto writeResult = watcher->result();
                            if (!writeResult.errorMessage.isEmpty())
                            {
                                Q_EMIT userInterventionRequired(
                                    QStringLiteral("Failed to prepare message source: %1")
                                        .arg(writeResult.errorMessage));
                                watcher->deleteLater();
                                return;
                            }

                            const bool opened =
                                QDesktopServices::openUrl(QUrl::fromLocalFile(writeResult.path));
                            Q_EMIT statusMessage(
                                opened ? QStringLiteral("Opened message source.")
                                       : QStringLiteral(
                                             "The source file was saved, but no app opened it."),
                                5000);
                            watcher->deleteLater();
                        });
                watcher->setFuture(
                    QtConcurrent::run([targetPath, payload = download.payload]
                                      { return writePayloadToPath(targetPath, payload); }));
            });
    }

} // namespace javelin::gui::shell
