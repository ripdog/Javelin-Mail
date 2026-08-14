#include "gui/compose/InlineImageController.h"

#include "gui/compose/ComposeBodyConverter.h"
#include "gui/compose/ComposerInlineImageCodec.h"
#include "gui/compose/JavelinComposerEdit.h"
#include "jmap/submission/ComposeTypes.h"

#include <KPIMTextEdit/RichTextComposerControler>
#include <KPIMTextEdit/RichTextComposerImages>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImage>
#include <QLoggingCategory>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QUrl>
#include <QUuid>
#include <QtConcurrentRun>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>

namespace javelin::gui::compose
{
    Q_LOGGING_CATEGORY(logComposeInlineImage, "gui.compose.image")

    namespace
    {
        struct PreparedInlineImage
        {
            QString filePath;
            QString displayName;
            QString mediaType;
            QString error;
            qint64 size = 0;
            qint64 processingMilliseconds = 0;
            bool reencoded = false;

            [[nodiscard]] bool succeeded() const
            {
                return error.isEmpty();
            }
        };

        [[nodiscard]] QString detectedMediaType(const QString& filePath)
        {
            QMimeDatabase mimeDatabase;
            const auto mimeType =
                mimeDatabase.mimeTypeForFile(filePath, QMimeDatabase::MatchContent);
            return mimeType.isValid() ? mimeType.name()
                                      : QStringLiteral("application/octet-stream");
        }

        [[nodiscard]] std::string newContentId()
        {
            const auto uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            return QStringLiteral("javelin-%1@inline").arg(uuid).toStdString();
        }

        [[nodiscard]] QString draftAssetDirectory(const std::string& composeSessionId)
        {
            return QDir{QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)}.filePath(
                QStringLiteral("draft-assets/%1").arg(QString::fromStdString(composeSessionId)));
        }

        [[nodiscard]] bool canPreserveInlineImage(const QString& mediaType)
        {
            return mediaType == QStringLiteral("image/png") ||
                   mediaType == QStringLiteral("image/jpeg") ||
                   mediaType == QStringLiteral("image/gif");
        }

        [[nodiscard]] PreparedInlineImage prepareInlineImage(QString sourceFilePath, QImage image,
                                                             QString destinationDirectory,
                                                             QString assetId)
        {
            QElapsedTimer elapsed;
            elapsed.start();
            PreparedInlineImage result;
            if (!QDir{}.mkpath(destinationDirectory))
            {
                result.error = QStringLiteral("Could not create storage for the inserted image.");
                result.processingMilliseconds = elapsed.elapsed();
                return result;
            }

            const QFileInfo sourceFile{sourceFilePath};
            const auto sourceMediaType = sourceFile.exists() && sourceFile.isFile()
                                             ? detectedMediaType(sourceFilePath)
                                             : QString{};
            if (!sourceMediaType.isEmpty() && canPreserveInlineImage(sourceMediaType))
            {
                const auto suffix =
                    sourceMediaType == QStringLiteral("image/jpeg")  ? QStringLiteral("jpg")
                    : sourceMediaType == QStringLiteral("image/gif") ? QStringLiteral("gif")
                                                                     : QStringLiteral("png");
                result.filePath = QDir{destinationDirectory}.filePath(
                    QStringLiteral("inserted-%1.%2").arg(assetId, suffix));
                if (!QFile::copy(sourceFile.absoluteFilePath(), result.filePath))
                {
                    result.error = QStringLiteral("Could not stage the inserted image.");
                    result.filePath.clear();
                    result.processingMilliseconds = elapsed.elapsed();
                    return result;
                }
                result.displayName = sourceFile.fileName();
                result.mediaType = sourceMediaType;
            }
            else
            {
                result.reencoded = true;
                result.filePath = QDir{destinationDirectory}.filePath(
                    QStringLiteral("inserted-%1.png").arg(assetId));
                if (!image.save(result.filePath, "PNG"))
                {
                    result.error = QStringLiteral("Could not encode the inserted image as PNG.");
                    result.filePath.clear();
                    result.processingMilliseconds = elapsed.elapsed();
                    return result;
                }
                const auto sourceBaseName = sourceFile.completeBaseName().trimmed();
                result.displayName = sourceBaseName.isEmpty()
                                         ? QStringLiteral("image.png")
                                         : QStringLiteral("%1.png").arg(sourceBaseName);
                result.mediaType = QStringLiteral("image/png");
            }

            result.size = QFileInfo{result.filePath}.size();
            result.processingMilliseconds = elapsed.elapsed();
            return result;
        }

        [[nodiscard]] bool documentContainsImageResource(const QTextDocument& document,
                                                         const QString& resourceName)
        {
            for (auto block = document.begin(); block.isValid(); block = block.next())
            {
                for (auto fragment = block.begin(); !fragment.atEnd(); ++fragment)
                {
                    const auto textFragment = fragment.fragment();
                    if (textFragment.isValid() && textFragment.charFormat().isImageFormat() &&
                        textFragment.charFormat().toImageFormat().name() == resourceName)
                        return true;
                }
            }
            return false;
        }

        void removeImageResource(QTextDocument& document, const QString& resourceName)
        {
            for (auto block = document.begin(); block.isValid(); block = block.next())
            {
                for (auto fragment = block.begin(); !fragment.atEnd(); ++fragment)
                {
                    const auto textFragment = fragment.fragment();
                    if (!textFragment.isValid() || !textFragment.charFormat().isImageFormat() ||
                        textFragment.charFormat().toImageFormat().name() != resourceName)
                        continue;
                    QTextCursor cursor{&document};
                    cursor.setPosition(textFragment.position());
                    cursor.setPosition(textFragment.position() + textFragment.length(),
                                       QTextCursor::KeepAnchor);
                    cursor.removeSelectedText();
                    return;
                }
            }
        }
    } // namespace

    InlineImageController::InlineImageController(JavelinComposerEdit& editor,
                                                 javelin::jmap::submission::DraftSnapshot& snapshot,
                                                 std::function<void()> attachmentsChanged,
                                                 std::function<void(QString, int)> statusMessage,
                                                 std::function<void()> pendingStateChanged,
                                                 std::function<void(bool)> allProcessingFinished,
                                                 QObject* parent)
        : QObject(parent), m_editor(editor), m_snapshot(snapshot),
          m_attachmentsChanged(std::move(attachmentsChanged)),
          m_statusMessage(std::move(statusMessage)),
          m_pendingStateChanged(std::move(pendingStateChanged)),
          m_allProcessingFinished(std::move(allProcessingFinished))
    {
    }

    bool InlineImageController::hasPendingJobs() const
    {
        return m_pendingJobs != 0;
    }

    void InlineImageController::addImagePath(const QString& filePath)
    {
        const QFileInfo info{filePath};
        const QImage image{filePath};
        if (!info.exists() || !info.isFile() || image.isNull())
        {
            if (m_statusMessage)
                m_statusMessage(tr("The selected image could not be loaded."), 7000);
            return;
        }

        m_snapshot.attachments.push_back(javelin::jmap::submission::DraftAttachment{
            .localFilePath = filePath.toStdString(),
            .displayName = info.fileName().toStdString(),
            .mediaType = detectedMediaType(filePath).toStdString(),
            .size = static_cast<std::uint64_t>(info.size()),
            .blobId = std::nullopt,
            .inlineDisposition = true,
            .contentId = newContentId(),
            .contentHash = std::nullopt,
        });
        insertEmbeddedImage(m_snapshot.attachments.size() - 1);
        if (m_attachmentsChanged)
            m_attachmentsChanged();
    }

    void InlineImageController::addPastedImage(const QImage& image)
    {
        if (image.isNull())
            return;
        const auto directory = draftAssetDirectory(m_snapshot.composeSessionId);
        if (!QDir{}.mkpath(directory))
        {
            if (m_statusMessage)
                m_statusMessage(tr("Could not create storage for the pasted image."), 10000);
            return;
        }

        const auto fileName =
            QStringLiteral("pasted-%1.png").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        const auto filePath = QDir{directory}.filePath(fileName);
        if (!image.save(filePath, "PNG"))
        {
            if (m_statusMessage)
                m_statusMessage(tr("Could not save the pasted image."), 10000);
            return;
        }
        addImagePath(filePath);
    }

    void InlineImageController::adoptInsertedComposerImage(const int insertionPosition,
                                                           const QString& sourceFilePath)
    {
        QElapsedTimer dispatchElapsed;
        dispatchElapsed.start();
        auto* document = m_editor.document();
        const auto block = document->findBlock(insertionPosition);
        QTextFragment insertedFragment;
        for (auto fragment = block.begin(); !fragment.atEnd(); ++fragment)
        {
            const auto candidate = fragment.fragment();
            if (candidate.isValid() && candidate.position() <= insertionPosition &&
                insertionPosition < candidate.position() + candidate.length() &&
                candidate.charFormat().isImageFormat())
            {
                insertedFragment = candidate;
                break;
            }
        }
        if (!insertedFragment.isValid())
        {
            qCWarning(logComposeInlineImage).noquote()
                << "inserted image adoption failed to locate the image fragment"
                << "elapsedMs" << dispatchElapsed.elapsed() << "insertionPosition"
                << insertionPosition;
            if (m_statusMessage)
                m_statusMessage(tr("The inserted image could not be tracked."), 10000);
            return;
        }

        auto imageFormat = insertedFragment.charFormat().toImageFormat();
        const auto originalResourceName = imageFormat.name();
        const auto image = qvariant_cast<QImage>(
            document->resource(QTextDocument::ImageResource, QUrl{originalResourceName}));
        if (image.isNull())
        {
            if (m_statusMessage)
                m_statusMessage(tr("The inserted image could not be loaded."), 10000);
            return;
        }

        const auto contentId = newContentId();
        const auto resourceName = composerEditorResourceName(contentId);
        document->addResource(QTextDocument::ImageResource, QUrl{resourceName}, image);
        imageFormat.setName(resourceName);
        QTextCursor imageCursor{document};
        imageCursor.setPosition(insertedFragment.position());
        imageCursor.setPosition(insertedFragment.position() + insertedFragment.length(),
                                QTextCursor::KeepAnchor);
        imageCursor.setCharFormat(imageFormat);

        const auto destinationDirectory = draftAssetDirectory(m_snapshot.composeSessionId);
        const auto assetId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto* watcher = new QFutureWatcher<PreparedInlineImage>(this);
        if (m_pendingJobs == 0)
            m_processingSucceeded = true;
        ++m_pendingJobs;
        if (m_pendingStateChanged)
            m_pendingStateChanged();

        connect(watcher, &QFutureWatcher<PreparedInlineImage>::finished, this,
                [this, watcher, resourceName, contentId]
                {
                    const auto result = watcher->result();
                    watcher->deleteLater();
                    const bool stillReferenced =
                        documentContainsImageResource(*m_editor.document(), resourceName);
                    if (!result.succeeded())
                    {
                        m_processingSucceeded = false;
                        qCWarning(logComposeInlineImage).noquote()
                            << "inserted image preparation failed"
                            << "processingMs" << result.processingMilliseconds << "error"
                            << result.error;
                        if (stillReferenced)
                            removeImageResource(*m_editor.document(), resourceName);
                        if (m_statusMessage)
                            m_statusMessage(tr("The inserted image could not be prepared."), 10000);
                        finishPreparation();
                        return;
                    }
                    if (!stillReferenced)
                    {
                        QFile::remove(result.filePath);
                        finishPreparation();
                        return;
                    }

                    m_snapshot.attachments.push_back(javelin::jmap::submission::DraftAttachment{
                        .localFilePath = result.filePath.toStdString(),
                        .displayName = result.displayName.toStdString(),
                        .mediaType = result.mediaType.toStdString(),
                        .size = static_cast<std::uint64_t>(result.size),
                        .blobId = std::nullopt,
                        .inlineDisposition = true,
                        .contentId = contentId,
                        .contentHash = std::nullopt,
                    });
                    if (m_attachmentsChanged)
                        m_attachmentsChanged();
                    qCInfo(logComposeInlineImage).noquote()
                        << "inserted image preparation complete"
                        << "processingMs" << result.processingMilliseconds << "reencoded"
                        << result.reencoded << "encodedBytes" << result.size << "mediaType"
                        << result.mediaType << "attachmentCount" << m_snapshot.attachments.size();
                    finishPreparation();
                });

        watcher->setFuture(QtConcurrent::run(
            [sourceFilePath, image, destinationDirectory, assetId]
            { return prepareInlineImage(sourceFilePath, image, destinationDirectory, assetId); }));
        qCInfo(logComposeInlineImage).noquote()
            << "inserted image preparation dispatched"
            << "dispatchMs" << dispatchElapsed.elapsed() << "width" << image.width() << "height"
            << image.height() << "decodedBytes" << image.sizeInBytes() << "sourceFile"
            << sourceFilePath;
    }

    bool InlineImageController::setAttachmentEmbedded(const std::size_t index, const bool embedded)
    {
        if (index >= m_snapshot.attachments.size() ||
            m_snapshot.editorMode == javelin::jmap::submission::BodyEditorMode::PlainText ||
            embedded == m_snapshot.attachments[index].inlineDisposition)
            return false;

        auto& attachment = m_snapshot.attachments[index];
        attachment.inlineDisposition = embedded;
        if (embedded)
        {
            if (!attachment.contentId.has_value())
                attachment.contentId = newContentId();
            insertEmbeddedImage(index);
        }
        else if (attachment.contentId.has_value())
        {
            removeEmbeddedImageReference(*attachment.contentId);
            attachment.contentId = std::nullopt;
        }
        if (m_attachmentsChanged)
            m_attachmentsChanged();
        return true;
    }

    void InlineImageController::insertEmbeddedImage(const std::size_t index)
    {
        if (index >= m_snapshot.attachments.size())
            return;
        const auto& attachment = m_snapshot.attachments[index];
        if (!attachment.contentId.has_value())
            return;
        const QImage image{QString::fromStdString(attachment.localFilePath)};
        if (image.isNull())
            return;
        const auto resourceName = composerEditorResourceName(*attachment.contentId);
        const auto width = std::min(image.width(), 720);
        const auto height = image.width() > 0 ? image.height() * width / image.width() : -1;
        m_editor.composerControler()->composerImages()->addImageHelper(resourceName, image, width,
                                                                       height);
    }

    void InlineImageController::removeEmbeddedImageReference(const std::string& contentId)
    {
        const auto cidUrl = composerContentIdUrl(contentId);
        const QRegularExpression imageTagPattern{
            QStringLiteral("<img\\b[^>]*\\bsrc\\s*=\\s*([\"'])%1\\1[^>]*>")
                .arg(QRegularExpression::escape(cidUrl)),
            QRegularExpression::CaseInsensitiveOption};
        auto html = stableHtml();
        html.remove(imageTagPattern);
        setEditorHtml(html);
    }

    void InlineImageController::setEditorHtml(const QString& html)
    {
        if (m_editor.textMode() == KPIMTextEdit::RichTextComposer::Plain)
            m_editor.activateRichText();
        m_editor.setTextOrHtml(
            htmlForQtDocument(editorHtmlForInlineAttachments(html, m_snapshot.attachments)));
        loadResources();
    }

    void InlineImageController::loadResources()
    {
        auto* images = m_editor.composerControler()->composerImages();
        for (const auto& attachment : m_snapshot.attachments)
        {
            if (!attachment.inlineDisposition || !attachment.contentId.has_value() ||
                attachment.localFilePath.empty())
                continue;
            const QImage image{QString::fromStdString(attachment.localFilePath)};
            if (image.isNull())
                continue;
            const auto resourceName = composerEditorResourceName(*attachment.contentId);
            images->loadImage(image, resourceName, resourceName);
        }
    }

    QString InlineImageController::stableHtml() const
    {
        return stableHtmlForInlineAttachments(m_editor.toCleanHtml(), m_snapshot.attachments);
    }

    void InlineImageController::reconcileAttachmentReferences(const QString& html)
    {
        if (reconcileInlineAttachments(m_snapshot.attachments, html) && m_attachmentsChanged)
            m_attachmentsChanged();
    }

    void InlineImageController::finishPreparation()
    {
        if (m_pendingJobs == 0)
            return;
        --m_pendingJobs;
        if (m_pendingStateChanged)
            m_pendingStateChanged();
        if (m_pendingJobs == 0 && m_allProcessingFinished)
            m_allProcessingFinished(m_processingSucceeded);
    }
} // namespace javelin::gui::compose
