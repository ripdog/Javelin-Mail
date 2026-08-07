#include "gui/compose/ComposeTabWidget.h"

#include "app/ComposeApplicationPorts.h"
#include "gui/compose/ComposeBodyConverter.h"
#include "gui/compose/ComposeUiPreferences.h"
#include "gui/compose/ComposerInlineImageCodec.h"
#include "gui/compose/IdentityPresentation.h"
#include "gui/compose/JavelinComposerEdit.h"
#include "gui/compose/SignatureTrackingPolicy.h"
#include "gui/messageview/HtmlMessageView.h"
#include "gui/settings/ConnectionSettingsAdapter.h"
#include "gui/settings/GuiSettings.h"
#include "gui/widgets/EmailAddressLineEdit.h"
#include "jmap/cache/AccountReadRepository.h"
#include "jmap/cache/IdentityReader.h"
#include "jmap/contacts/ContactIdentityLookup.h"

#include <QCoroTask>

#include <KActionCollection>
#include <KLocalizedString>
#include <KPIMTextEdit/RichTextComposerControler>
#include <KPIMTextEdit/RichTextComposerImages>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QLoggingCategory>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMimeDatabase>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QStringList>
#include <QStyle>
#include <QTabWidget>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QVariant>
#include <QtConcurrentRun>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>

namespace javelin::gui::compose
{
    Q_LOGGING_CATEGORY(logComposeImage, "gui.compose.image")

    namespace
    {

        constexpr auto richEditorTabIndex = 0;
        constexpr auto previewTabIndex = 1;
        constexpr auto senderIdentityIdRole = Qt::UserRole;
        constexpr auto senderAccountIdRole = Qt::UserRole + 1;
        constexpr auto senderEmailRole = Qt::UserRole + 2;
        constexpr auto senderTextSignatureRole = Qt::UserRole + 3;
        constexpr auto senderHtmlSignatureRole = Qt::UserRole + 4;
        constexpr auto senderBccRole = Qt::UserRole + 5;

        struct ImageInsertTiming
        {
            QElapsedTimer elapsed;
            std::optional<qint64> acceptedAtMilliseconds;
            QString sourceFilePath;
            QMetaObject::Connection documentChangeConnection;
        };

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

        [[nodiscard]] QString imageDialogSourceFilePath(const QDialog& dialog)
        {
            for (const auto* lineEdit : dialog.findChildren<QLineEdit*>())
            {
                const auto value = lineEdit->text().trimmed();
                if (value.isEmpty())
                    continue;

                const auto url =
                    QUrl::fromUserInput(value, QDir::currentPath(), QUrl::AssumeLocalFile);
                const auto path = url.isLocalFile() ? url.toLocalFile() : value;
                const QFileInfo file{path};
                if (file.exists() && file.isFile())
                    return file.absoluteFilePath();
            }
            return {};
        }

        [[nodiscard]] QString defaultTitleForMode(const javelin::jmap::submission::ComposeMode mode)
        {
            switch (mode)
            {
            case javelin::jmap::submission::ComposeMode::NewMessage:
                return i18n("New Message");
            case javelin::jmap::submission::ComposeMode::Reply:
                return i18n("Reply");
            case javelin::jmap::submission::ComposeMode::ReplyAll:
                return i18n("Reply All");
            case javelin::jmap::submission::ComposeMode::Forward:
                return i18n("Forward");
            case javelin::jmap::submission::ComposeMode::EditDraft:
                return i18n("Edit Draft");
            }

            return i18n("Compose");
        }

        [[nodiscard]] QString displayAddress(const javelin::jmap::domain::EmailAddress& address)
        {
            if (address.name.has_value() && !address.name->empty())
            {
                return QStringLiteral("%1 <%2>").arg(QString::fromStdString(*address.name),
                                                     QString::fromStdString(address.email));
            }

            return QString::fromStdString(address.email);
        }

        [[nodiscard]] QString
        formatAddresses(const std::vector<javelin::jmap::domain::EmailAddress>& addresses)
        {
            QStringList parts;
            for (const auto& address : addresses)
            {
                parts.push_back(displayAddress(address));
            }
            return parts.join(QStringLiteral(", "));
        }

        [[nodiscard]] std::optional<javelin::jmap::domain::EmailAddress>
        parseAddressToken(const QString& token)
        {
            const auto trimmed = token.trimmed();
            if (trimmed.isEmpty())
            {
                return std::nullopt;
            }

            const auto openBracket = trimmed.lastIndexOf(QLatin1Char('<'));
            const auto closeBracket = trimmed.lastIndexOf(QLatin1Char('>'));
            if (openBracket >= 0 && closeBracket > openBracket)
            {
                const auto name = trimmed.left(openBracket).trimmed().remove(QLatin1Char('"'));
                const auto email =
                    trimmed.mid(openBracket + 1, closeBracket - openBracket - 1).trimmed();
                if (!email.contains(QLatin1Char('@')))
                {
                    return std::nullopt;
                }
                return javelin::jmap::domain::EmailAddress{
                    .name = name.isEmpty() ? std::nullopt
                                           : std::optional<std::string>{name.toStdString()},
                    .email = email.toStdString(),
                };
            }

            if (!trimmed.contains(QLatin1Char('@')))
            {
                return std::nullopt;
            }

            return javelin::jmap::domain::EmailAddress{
                .name = std::nullopt,
                .email = trimmed.toStdString(),
            };
        }

        [[nodiscard]] std::vector<javelin::jmap::domain::EmailAddress>
        parseAddresses(const QString& value)
        {
            std::vector<javelin::jmap::domain::EmailAddress> addresses;
            QString current;
            bool insideAngleBrackets = false;
            for (const auto character : value)
            {
                if (character == QLatin1Char('<'))
                {
                    insideAngleBrackets = true;
                }
                else if (character == QLatin1Char('>'))
                {
                    insideAngleBrackets = false;
                }

                if (!insideAngleBrackets &&
                    (character == QLatin1Char(',') || character == QLatin1Char(';')))
                {
                    if (const auto parsed = parseAddressToken(current))
                    {
                        addresses.push_back(*parsed);
                    }
                    current.clear();
                    continue;
                }

                current.append(character);
            }

            if (const auto parsed = parseAddressToken(current))
            {
                addresses.push_back(*parsed);
            }

            return addresses;
        }

        [[nodiscard]] QString attachmentSizeLabel(const std::uint64_t size)
        {
            constexpr double kib = 1024.0;
            constexpr double mib = 1024.0 * kib;
            constexpr double gib = 1024.0 * mib;

            if (size >= static_cast<std::uint64_t>(gib))
            {
                return i18nc("@item file size", "%1 GB",
                             QString::number(static_cast<double>(size) / gib, 'f', 1));
            }
            if (size >= static_cast<std::uint64_t>(mib))
            {
                return i18nc("@item file size", "%1 MB",
                             QString::number(static_cast<double>(size) / mib, 'f', 1));
            }
            if (size >= static_cast<std::uint64_t>(kib))
            {
                return i18nc("@item file size", "%1 KB",
                             QString::number(static_cast<double>(size) / kib, 'f', 1));
            }

            return i18nc("@item file size", "%1 B", static_cast<qulonglong>(size));
        }

        [[nodiscard]] QString
        attachmentItemText(const javelin::jmap::submission::DraftAttachment& attachment)
        {
            const auto displayName =
                !attachment.displayName.empty()
                    ? QString::fromStdString(attachment.displayName)
                    : QFileInfo{QString::fromStdString(attachment.localFilePath)}.fileName();
            const auto mediaType = attachment.mediaType.empty()
                                       ? i18nc("@item unknown attachment media type", "attachment")
                                       : QString::fromStdString(attachment.mediaType);
            return QStringLiteral("%1  •  %2  •  %3")
                .arg(displayName, mediaType, attachmentSizeLabel(attachment.size));
        }

        [[nodiscard]] QString
        attachmentDisplayName(const javelin::jmap::submission::DraftAttachment& attachment)
        {
            if (!attachment.displayName.empty())
            {
                return QString::fromStdString(attachment.displayName);
            }
            if (!attachment.localFilePath.empty())
            {
                return QFileInfo{QString::fromStdString(attachment.localFilePath)}.fileName();
            }
            return i18n("Attachment");
        }

        [[nodiscard]] QIcon
        attachmentIcon(const javelin::jmap::submission::DraftAttachment& attachment)
        {
            QFileIconProvider iconProvider;
            auto icon = iconProvider.icon(QFileInfo{attachmentDisplayName(attachment)});
            if (!icon.isNull())
            {
                return icon;
            }

            const QMimeDatabase mimeDatabase;
            const auto mimeType =
                mimeDatabase.mimeTypeForName(QString::fromStdString(attachment.mediaType));
            icon = QIcon::fromTheme(mimeType.iconName());
            if (icon.isNull())
            {
                icon = QIcon::fromTheme(mimeType.genericIconName());
            }
            if (icon.isNull())
            {
                icon = QIcon::fromTheme(QStringLiteral("mail-attachment"));
            }
            if (icon.isNull())
            {
                icon = QApplication::style()->standardIcon(QStyle::SP_FileIcon);
            }
            return icon;
        }

        [[nodiscard]] bool
        isImageAttachment(const javelin::jmap::submission::DraftAttachment& attachment)
        {
            if (!attachment.mediaType.empty())
            {
                return attachment.mediaType.rfind("image/", 0) == 0;
            }
            if (attachment.localFilePath.empty())
            {
                return false;
            }

            QMimeDatabase mimeDatabase;
            const auto mimeType = mimeDatabase.mimeTypeForFile(
                QString::fromStdString(attachment.localFilePath), QMimeDatabase::MatchContent);
            return mimeType.name().startsWith(QStringLiteral("image/"));
        }

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
                    {
                        return true;
                    }
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
                    {
                        continue;
                    }

                    QTextCursor cursor{&document};
                    cursor.setPosition(textFragment.position());
                    cursor.setPosition(textFragment.position() + textFragment.length(),
                                       QTextCursor::KeepAnchor);
                    cursor.removeSelectedText();
                    return;
                }
            }
        }

        class DraftAttachmentChip : public QWidget
        {
          public:
            DraftAttachmentChip(const javelin::jmap::submission::DraftAttachment& attachment,
                                const bool embeddingAllowed, std::function<void()> removeAction,
                                std::function<void(bool)> embedAction, QWidget* parent = nullptr)
                : QWidget(parent), m_removeAction(std::move(removeAction)),
                  m_embedAction(std::move(embedAction))
            {
                setToolTip(attachmentItemText(attachment));

                auto* layout = new QHBoxLayout(this);
                layout->setContentsMargins(4, 0, 2, 0);
                layout->setSpacing(4);

                auto* iconLabel = new QLabel(this);
                iconLabel->setPixmap(attachmentIcon(attachment).pixmap(16, 16));
                iconLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

                auto* nameLabel = new QLabel(attachmentDisplayName(attachment), this);
                nameLabel->setMinimumWidth(80);
                nameLabel->setMaximumWidth(220);
                nameLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

                auto* sizeLabel = new QLabel(attachmentSizeLabel(attachment.size), this);
                sizeLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

                if (embeddingAllowed && isImageAttachment(attachment))
                {
                    auto* attachRadio = new QRadioButton(i18nc("@option:radio", "Attach"), this);
                    auto* embedRadio = new QRadioButton(i18nc("@option:radio", "Embed"), this);
                    attachRadio->setChecked(!attachment.inlineDisposition);
                    embedRadio->setChecked(attachment.inlineDisposition);
                    attachRadio->setToolTip(i18n("Send this image as an attachment"));
                    embedRadio->setToolTip(i18n("Show this image in the message body"));
                    connect(attachRadio, &QRadioButton::toggled, this,
                            [this](const bool checked)
                            {
                                if (checked && m_embedAction)
                                {
                                    m_embedAction(false);
                                }
                            });
                    connect(embedRadio, &QRadioButton::toggled, this,
                            [this](const bool checked)
                            {
                                if (checked && m_embedAction)
                                {
                                    m_embedAction(true);
                                }
                            });
                    layout->addWidget(attachRadio);
                    layout->addWidget(embedRadio);
                }

                auto* removeButton = new QToolButton(this);
                removeButton->setText(QStringLiteral("x"));
                removeButton->setToolTip(i18n("Remove attachment"));
                removeButton->setAccessibleName(i18n("Remove attachment"));
                removeButton->setAutoRaise(true);
                removeButton->setFixedSize(22, 22);
                connect(removeButton, &QToolButton::clicked, this,
                        [this]
                        {
                            if (m_removeAction)
                            {
                                m_removeAction();
                            }
                        });

                layout->addWidget(iconLabel);
                layout->addWidget(nameLabel);
                layout->addWidget(sizeLabel);
                layout->addWidget(removeButton);
            }

          private:
            std::function<void()> m_removeAction;
            std::function<void(bool)> m_embedAction;
        };

        [[nodiscard]] std::optional<javelin::app::AccountConnectionSettings>
        liveSettings(const javelin::gui::settings::GuiSettings& guiSettings,
                     const std::string_view accountId, QString* errorMessage = nullptr)
        {
            const auto settings =
                guiSettings.accountForCachedId(QString::fromStdString(std::string{accountId}));
            if (settings.sessionUrl.isEmpty() || settings.loginEmail.isEmpty() ||
                !settings.hasCredentials)
            {
                if (errorMessage != nullptr)
                    *errorMessage = i18n("Sign in to this account in Preferences first.");
                return std::nullopt;
            }

            return javelin::gui::settings::toAccountConnectionSettings(settings);
        }

    } // namespace

    ComposeTabWidget::ComposeTabWidget(
        javelin::gui::settings::GuiSettings& settings,
        javelin::app::ComposeCommandPort& composeCommandPort,
        javelin::jmap::cache::AccountReader& accountReader,
        javelin::jmap::cache::IdentityReader& identityRepository,
        javelin::jmap::contacts::ContactIdentityLookup& contactIdentityLookup,
        javelin::jmap::submission::DraftSnapshot snapshot, QWidget* parent)
        : QWidget(parent), m_settings(settings), m_composeCommandPort(composeCommandPort),
          m_accountReader(accountReader), m_identityRepository(identityRepository),
          m_contactIdentityLookup(contactIdentityLookup), m_snapshot(std::move(snapshot))
    {
        setAcceptDrops(true);
        setupUi();
        createToolbarActions();
        loadIdentities();
        applySnapshotToUi();

        m_autosaveTimer = new QTimer(this);
        m_autosaveTimer->setSingleShot(true);
        m_autosaveTimer->setInterval(350);
        connect(m_autosaveTimer, &QTimer::timeout, this, &ComposeTabWidget::persistWorkingCopy);

        connect(
            m_fromCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](const int index)
            {
                if (m_syncingUi)
                {
                    return;
                }

                if (m_previousIdentityIndex >= 0)
                {
                    const auto previousAutomaticBcc =
                        m_fromCombo->itemData(m_previousIdentityIndex, senderBccRole).toString();
                    if (m_bccEdit->text().trimmed() == previousAutomaticBcc.trimmed())
                    {
                        const auto nextAutomaticBcc =
                            m_fromCombo->itemData(index, senderBccRole).toString();
                        m_bccEdit->setText(nextAutomaticBcc);
                        setOptionalRecipientVisible(m_bccRow, m_bccButton,
                                                    !nextAutomaticBcc.isEmpty());
                    }
                }
                replaceTrackedSignatureForIndex(index);
                m_previousIdentityIndex = index;
                syncSnapshotFromUi();
                scheduleWorkingCopySave();
                Q_EMIT toolbarStateChanged();
            });
        for (auto* edit : {m_toEdit, m_ccEdit, m_bccEdit, m_subjectEdit})
        {
            connect(edit, &QLineEdit::textChanged, this,
                    [this, edit](const QString&)
                    {
                        if (m_syncingUi)
                        {
                            return;
                        }

                        if (edit == m_subjectEdit)
                        {
                            updateTabTitle();
                        }
                        scheduleWorkingCopySave();
                    });
        }

        connect(m_richTextEdit, &JavelinComposerEdit::attachmentPathsRequested, this,
                &ComposeTabWidget::addAttachmentPaths);
        connect(m_richTextEdit, &JavelinComposerEdit::inlineImageRequested, this,
                &ComposeTabWidget::addPastedInlineImage);
        connect(m_richTextEdit, &QTextEdit::textChanged, this,
                [this]
                {
                    if (!m_syncingUi)
                    {
                        scheduleWorkingCopySave();
                    }
                });
        connect(m_richTextEdit->document(), &QTextDocument::contentsChange, this,
                [this](const int position, const int removed, const int added)
                {
                    if (m_syncingUi || m_signatureProgrammaticEdit || !m_signatureTracked)
                        return;
                    if (changeTouchesTrackedSignature({.start = m_signatureCursor.selectionStart(),
                                                       .end = m_signatureCursor.selectionEnd()},
                                                      position, removed, added))
                        m_signatureCustom = true;
                });
        connect(m_richTextEdit, &QTextEdit::currentCharFormatChanged, this,
                [this](const QTextCharFormat& format)
                {
                    if (m_codeAction != nullptr)
                    {
                        const QSignalBlocker blocker{m_codeAction};
                        m_codeAction->setChecked(format.fontFamilies().toStringList().contains(
                            QStringLiteral("monospace")));
                    }
                });
        connect(m_editorTabs, &QTabWidget::currentChanged, this,
                [this](const int index)
                {
                    if (m_syncingUi)
                    {
                        return;
                    }

                    if (index == previewTabIndex)
                    {
                        syncSnapshotFromUi();
                        refreshPreview();
                    }
                    updateEditorModeUi();
                });

        refreshPreview();
        updateEditorModeUi();
        updateTabTitle();
    }

    QString ComposeTabWidget::tabTitle() const
    {
        const auto subject = m_subjectEdit->text().trimmed();
        return subject.isEmpty() ? defaultTitleForMode(m_snapshot.mode) : subject;
    }

    std::string ComposeTabWidget::composeSessionId() const
    {
        return m_snapshot.composeSessionId;
    }

    std::optional<std::string> ComposeTabWidget::draftEmailId() const
    {
        return m_snapshot.draftEmailId;
    }

    bool ComposeTabWidget::isEmptyDraft() const
    {
        const auto subject = m_subjectEdit->text().trimmed();
        const auto body = m_richTextEdit->toPlainText();
        return subject.isEmpty() && m_toEdit->text().trimmed().isEmpty() &&
               m_ccEdit->text().trimmed().isEmpty() && m_bccEdit->text().trimmed().isEmpty() &&
               body.trimmed().isEmpty() && m_snapshot.attachments.empty();
    }

    bool ComposeTabWidget::closeWithoutPrompt() const
    {
        return m_closeWithoutPrompt;
    }

    bool ComposeTabWidget::operationInFlight() const
    {
        return m_operationInFlight;
    }

    bool ComposeTabWidget::canSend() const
    {
        const auto index = m_fromCombo->currentIndex();
        if (m_operationInFlight || index < 0)
            return false;
        return !m_fromCombo->itemData(index, senderAccountIdRole).toString().isEmpty() &&
               !m_fromCombo->itemData(index, senderIdentityIdRole).toString().isEmpty();
    }

    bool ComposeTabWidget::richTextEnabled() const
    {
        return m_snapshot.editorMode == javelin::jmap::submission::BodyEditorMode::RichText;
    }

    void ComposeTabWidget::setRichTextEnabled(const bool enabled)
    {
        switchBodyFormat(enabled);
    }

    void ComposeTabWidget::saveDraftAndClose()
    {
        startSaveDraft(true);
    }

    void ComposeTabWidget::dragEnterEvent(QDragEnterEvent* event)
    {
        if (event->mimeData()->hasUrls())
        {
            for (const auto& url : event->mimeData()->urls())
            {
                if (url.isLocalFile())
                {
                    event->acceptProposedAction();
                    return;
                }
            }
        }

        QWidget::dragEnterEvent(event);
    }

    void ComposeTabWidget::dragMoveEvent(QDragMoveEvent* event)
    {
        if (event->mimeData()->hasUrls())
        {
            for (const auto& url : event->mimeData()->urls())
            {
                if (url.isLocalFile())
                {
                    event->acceptProposedAction();
                    return;
                }
            }
        }

        QWidget::dragMoveEvent(event);
    }

    void ComposeTabWidget::dropEvent(QDropEvent* event)
    {
        QStringList filePaths;
        for (const auto& url : event->mimeData()->urls())
        {
            if (url.isLocalFile())
            {
                filePaths.push_back(url.toLocalFile());
            }
        }

        if (filePaths.empty())
        {
            QWidget::dropEvent(event);
            return;
        }

        addAttachmentPaths(filePaths);
        event->acceptProposedAction();
    }

    void ComposeTabWidget::setupUi()
    {
        auto* rootLayout = new QVBoxLayout(this);
        rootLayout->setContentsMargins(0, 0, 0, 0);
        rootLayout->setSpacing(4);

        auto* headerWidget = new QWidget(this);
        auto* headerLayout = new QVBoxLayout(headerWidget);
        headerLayout->setContentsMargins(6, 6, 6, 0);
        headerLayout->setSpacing(4);

        auto* fromRow = new QHBoxLayout();
        auto* fromLabel = new QLabel(i18nc("@label email sender", "From"), headerWidget);
        fromLabel->setMinimumWidth(52);
        m_fromCombo = new QComboBox(headerWidget);
        m_signatureButton = new QToolButton(headerWidget);
        m_signatureButton->setText(i18n("Signature"));
        m_signatureButton->setIcon(QIcon::fromTheme(QStringLiteral("mail-signature")));
        m_signatureButton->setPopupMode(QToolButton::InstantPopup);
        m_signatureButton->setAutoRaise(true);
        m_signatureMenu = new QMenu(m_signatureButton);
        m_signatureButton->setMenu(m_signatureMenu);
        connect(m_signatureMenu, &QMenu::aboutToShow, this,
                &ComposeTabWidget::refreshSignatureMenu);
        fromRow->addWidget(fromLabel);
        fromRow->addWidget(m_fromCombo, 1);
        fromRow->addWidget(m_signatureButton);
        headerLayout->addLayout(fromRow);

        auto* toRow = new QHBoxLayout();
        auto* toLabel = new QLabel(i18nc("@label email recipients", "To"), headerWidget);
        toLabel->setMinimumWidth(52);
        m_toEdit = new widgets::EmailAddressLineEdit(true, headerWidget);
        m_toEdit->setPlaceholderText(QStringLiteral("alice@example.com, Bob <bob@example.com>"));
        m_ccButton = new QToolButton(headerWidget);
        m_ccButton->setText(i18nc("@action:button show carbon-copy field", "Cc"));
        m_ccButton->setCheckable(true);
        m_ccButton->setAutoRaise(true);
        m_bccButton = new QToolButton(headerWidget);
        m_bccButton->setText(i18nc("@action:button show blind-carbon-copy field", "Bcc"));
        m_bccButton->setCheckable(true);
        m_bccButton->setAutoRaise(true);
        toRow->addWidget(toLabel);
        toRow->addWidget(m_toEdit, 1);
        toRow->addWidget(m_ccButton);
        toRow->addWidget(m_bccButton);
        headerLayout->addLayout(toRow);

        m_ccRow = new QWidget(headerWidget);
        auto* ccRowLayout = new QHBoxLayout(m_ccRow);
        ccRowLayout->setContentsMargins(0, 0, 0, 0);
        auto* ccLabel = new QLabel(i18nc("@label email carbon-copy recipients", "Cc"), m_ccRow);
        ccLabel->setMinimumWidth(52);
        m_ccEdit = new widgets::EmailAddressLineEdit(true, m_ccRow);
        m_ccEdit->setPlaceholderText(i18nc("@info:placeholder", "Optional"));
        ccRowLayout->addWidget(ccLabel);
        ccRowLayout->addWidget(m_ccEdit, 1);
        headerLayout->addWidget(m_ccRow);

        m_bccRow = new QWidget(headerWidget);
        auto* bccRowLayout = new QHBoxLayout(m_bccRow);
        bccRowLayout->setContentsMargins(0, 0, 0, 0);
        auto* bccLabel =
            new QLabel(i18nc("@label email blind-carbon-copy recipients", "Bcc"), m_bccRow);
        bccLabel->setMinimumWidth(52);
        m_bccEdit = new widgets::EmailAddressLineEdit(true, m_bccRow);
        m_bccEdit->setPlaceholderText(i18nc("@info:placeholder", "Optional"));
        bccRowLayout->addWidget(bccLabel);
        bccRowLayout->addWidget(m_bccEdit, 1);
        headerLayout->addWidget(m_bccRow);
        connect(m_ccButton, &QToolButton::toggled, this,
                [this](const bool visible)
                {
                    setOptionalRecipientVisible(m_ccRow, m_ccButton, visible);
                    if (visible)
                    {
                        m_ccEdit->setFocus();
                    }
                });
        connect(m_bccButton, &QToolButton::toggled, this,
                [this](const bool visible)
                {
                    setOptionalRecipientVisible(m_bccRow, m_bccButton, visible);
                    if (visible)
                    {
                        m_bccEdit->setFocus();
                    }
                });

        auto* subjectRow = new QHBoxLayout();
        auto* subjectLabel = new QLabel(i18nc("@label email subject", "Subject"), headerWidget);
        subjectLabel->setMinimumWidth(52);
        m_subjectEdit = new QLineEdit(headerWidget);
        m_subjectEdit->setPlaceholderText(i18n("Add a subject"));
        subjectRow->addWidget(subjectLabel);
        subjectRow->addWidget(m_subjectEdit, 1);
        headerLayout->addLayout(subjectRow);

        rootLayout->addWidget(headerWidget);

        m_formatToolbar = new QToolBar(this);
        m_formatToolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        rootLayout->addWidget(m_formatToolbar);

        m_editorTabs = new QTabWidget(this);
        m_richTextEdit = new JavelinComposerEdit(m_editorTabs);
        m_richTextEdit->setAcceptDrops(false);
        m_richTextEdit->setAcceptRichText(true);
        m_richTextEdit->document()->setDocumentMargin(8);
        m_previewView = new javelin::gui::messageview::HtmlMessageView(
            m_settings.messageAppearanceSettings(), m_editorTabs);
        m_previewView->setAcceptDrops(false);
        m_previewView->setRemoteContentEnabled(false);
        m_editorTabs->addTab(m_richTextEdit, i18n("Compose"));
        m_editorTabs->addTab(m_previewView, i18n("Preview"));
        rootLayout->addWidget(m_editorTabs, 1);

        m_attachmentScrollArea = new QScrollArea(this);
        m_attachmentScrollArea->setFrameShape(QFrame::NoFrame);
        m_attachmentScrollArea->setWidgetResizable(true);
        m_attachmentScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_attachmentScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        m_attachmentScrollArea->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_attachmentStrip = new QWidget(m_attachmentScrollArea);
        m_attachmentStripLayout = new QHBoxLayout(m_attachmentStrip);
        m_attachmentStripLayout->setContentsMargins(0, 0, 0, 0);
        m_attachmentStripLayout->setSpacing(6);
        m_attachmentStripLayout->addStretch(1);
        m_attachmentScrollArea->setWidget(m_attachmentStrip);
        m_attachmentScrollArea->setFixedHeight(fontMetrics().height() + 24);
        rootLayout->addWidget(m_attachmentScrollArea);
    }

    void ComposeTabWidget::createToolbarActions()
    {
        m_actionCollection = new KActionCollection(this, QStringLiteral("javelin-composer"));
        m_actionCollection->addAssociatedWidget(this);
        m_richTextEdit->createActions(m_actionCollection);

        const auto addKdeAction = [this](const QString& name) -> QAction*
        {
            auto* action = m_actionCollection->action(name);
            if (action != nullptr)
            {
                m_formatToolbar->addAction(action);
            }
            return action;
        };

        addKdeAction(QStringLiteral("format_heading_level"));
        addKdeAction(QStringLiteral("format_list_style"));
        addKdeAction(QStringLiteral("format_font_family"));
        addKdeAction(QStringLiteral("format_font_size"));
        m_formatToolbar->addSeparator();

        addKdeAction(QStringLiteral("format_text_bold"));
        addKdeAction(QStringLiteral("format_text_italic"));
        addKdeAction(QStringLiteral("format_text_underline"));
        addKdeAction(QStringLiteral("format_text_strikeout"));

        m_codeAction = new QAction(QIcon::fromTheme(QStringLiteral("format-text-code")),
                                   i18nc("@action text formatting", "Code"), this);
        m_codeAction->setCheckable(true);
        m_actionCollection->addAction(QStringLiteral("javelin_format_code"), m_codeAction);
        m_formatToolbar->addAction(m_codeAction);
        connect(m_codeAction, &QAction::triggered, this, &ComposeTabWidget::toggleCode);

        addKdeAction(QStringLiteral("format_text_foreground_color"));
        addKdeAction(QStringLiteral("format_text_background_color"));
        m_formatToolbar->addSeparator();

        addKdeAction(QStringLiteral("format_align_left"));
        addKdeAction(QStringLiteral("format_align_center"));
        addKdeAction(QStringLiteral("format_align_right"));
        addKdeAction(QStringLiteral("format_align_justify"));
        addKdeAction(QStringLiteral("format_list_indent_more"));
        addKdeAction(QStringLiteral("format_list_indent_less"));
        m_formatToolbar->addSeparator();

        addKdeAction(QStringLiteral("manage_link"));
        addKdeAction(QStringLiteral("insert_horizontal_rule"));

        m_insertImageAction = new QAction(QIcon::fromTheme(QStringLiteral("insert-image")),
                                          i18nc("@action insert image", "Image"), this);
        m_actionCollection->addAction(QStringLiteral("javelin_insert_image"), m_insertImageAction);
        m_formatToolbar->addAction(m_insertImageAction);
        connect(m_insertImageAction, &QAction::triggered, this, &ComposeTabWidget::insertImage);

        addKdeAction(QStringLiteral("insert_html"));
        addKdeAction(QStringLiteral("insert_table"));
        addKdeAction(QStringLiteral("format_list_checkbox"));
        addKdeAction(QStringLiteral("format_reset"));
        addKdeAction(QStringLiteral("format_painter"));
        addKdeAction(QStringLiteral("direction_ltr"));
        addKdeAction(QStringLiteral("direction_rtl"));
    }

    void ComposeTabWidget::loadIdentities()
    {
        const QSignalBlocker blocker{m_fromCombo};
        const auto selectedAccountId = QString::fromStdString(m_snapshot.accountId);
        const auto selectedIdentityId = QString::fromStdString(m_snapshot.identityId);
        int selectedIndex = -1;
        int optionCount = 0;

        const auto accountsResult = m_accountReader.listAll();
        const auto* accounts =
            std::get_if<std::vector<javelin::jmap::cache::CachedAccount>>(&accountsResult);
        if (accounts == nullptr)
        {
            const auto& error = std::get<javelin::jmap::cache::DatabaseError>(accountsResult);
            Q_EMIT statusMessageRequested(error.message, 10000);
            return;
        }

        std::unordered_set<std::string> mailAccountIds;
        mailAccountIds.reserve(accounts->size());
        for (const auto& account : *accounts)
        {
            if (account.hasMailCapability && account.hasSubmissionCapability)
                mailAccountIds.insert(account.accountId);
        }

        struct SenderIdentityOption
        {
            QString accountId;
            QString accountDisplayName;
            javelin::jmap::domain::Identity identity;
        };
        std::vector<SenderIdentityOption> options;
        std::unordered_map<std::string, std::size_t> identityCountByEmail;
        std::unordered_set<std::string> accountsWithIdentities;

        m_fromCombo->clear();
        for (const auto& connection : m_settings.accounts())
        {
            const auto accountDisplayName =
                connection.displayName.isEmpty() ? connection.loginEmail : connection.displayName;
            for (const auto& cachedAccountId : connection.cachedAccountIds)
            {
                const auto accountId = cachedAccountId.toStdString();
                if (!mailAccountIds.contains(accountId))
                    continue;
                const auto identitiesResult = m_identityRepository.listByAccount(accountId);
                if (const auto* error =
                        std::get_if<javelin::jmap::cache::DatabaseError>(&identitiesResult))
                {
                    Q_EMIT statusMessageRequested(error->message, 10000);
                    continue;
                }

                const auto& identities =
                    std::get<std::vector<javelin::jmap::domain::Identity>>(identitiesResult);
                bool hasSenderIdentity = false;
                for (const auto& identity : identities)
                {
                    if (isWildcardSenderIdentity(identity))
                        continue;
                    hasSenderIdentity = true;
                    auto emailKey = QString::fromStdString(identity.email)
                                        .trimmed()
                                        .toCaseFolded()
                                        .toStdString();
                    ++identityCountByEmail[emailKey];
                    accountsWithIdentities.insert(accountId);
                    options.push_back({
                        .accountId = cachedAccountId,
                        .accountDisplayName = accountDisplayName,
                        .identity = identity,
                    });
                }

                if (!hasSenderIdentity && !m_identityLoadsStarted.contains(accountId) &&
                    !connection.sessionUrl.isEmpty() && !connection.loginEmail.isEmpty() &&
                    connection.hasCredentials)
                {
                    m_identityLoadsStarted.insert(accountId);
                    auto task = m_composeCommandPort.loadSenderIdentities(
                        javelin::gui::settings::toAccountConnectionSettings(connection), accountId);
                    QCoro::connect(std::move(task), this,
                                   [this](std::variant<std::vector<javelin::jmap::domain::Identity>,
                                                       javelin::jmap::OperationError>
                                              result)
                                   {
                                       if (const auto* error =
                                               std::get_if<javelin::jmap::OperationError>(&result))
                                       {
                                           Q_EMIT statusMessageRequested(error->message, 10000);
                                           return;
                                       }
                                       loadIdentities();
                                   });
                }
            }
        }

        const bool includeAccountName = accountsWithIdentities.size() > 1;
        for (const auto& option : options)
        {
            const auto emailKey = QString::fromStdString(option.identity.email)
                                      .trimmed()
                                      .toCaseFolded()
                                      .toStdString();
            const int index = m_fromCombo->count();
            m_fromCombo->addItem(
                composeIdentityDisplayText(option.identity, option.accountDisplayName,
                                           identityCountByEmail[emailKey], includeAccountName));
            m_fromCombo->setItemData(index, QString::fromStdString(option.identity.id),
                                     senderIdentityIdRole);
            m_fromCombo->setItemData(index, option.accountId, senderAccountIdRole);
            m_fromCombo->setItemData(index, QString::fromStdString(option.identity.email),
                                     senderEmailRole);
            m_fromCombo->setItemData(index,
                                     option.identity.textSignature.has_value()
                                         ? QString::fromStdString(*option.identity.textSignature)
                                         : QString{},
                                     senderTextSignatureRole);
            m_fromCombo->setItemData(index,
                                     option.identity.htmlSignature.has_value()
                                         ? QString::fromStdString(*option.identity.htmlSignature)
                                         : QString{},
                                     senderHtmlSignatureRole);
            m_fromCombo->setItemData(index, formatAddresses(option.identity.bcc), senderBccRole);
            if (option.accountId == selectedAccountId &&
                QString::fromStdString(option.identity.id) == selectedIdentityId)
            {
                selectedIndex = index;
            }
        }
        optionCount = static_cast<int>(options.size());

        if (selectedIndex >= 0)
        {
            m_fromCombo->setCurrentIndex(selectedIndex);
        }
        else if (!selectedIdentityId.isEmpty())
        {
            m_fromCombo->setPlaceholderText(i18n("Sender identity unavailable — choose another"));
            m_fromCombo->setCurrentIndex(-1);
            Q_EMIT statusMessageRequested(i18n("The draft's sender identity is no longer "
                                               "available. Choose another sender before sending."),
                                          10000);
        }
        else if (optionCount > 0)
        {
            m_fromCombo->setCurrentIndex(0);
        }
        else
        {
            Q_EMIT statusMessageRequested(
                i18n("No sender identities are available for configured accounts."), 10000);
        }
        m_previousIdentityIndex = m_fromCombo->currentIndex();
    }

    void ComposeTabWidget::reloadSenderIdentities(const QString& changedAccountId)
    {
        const auto selectedAccountId = m_fromCombo->currentData(senderAccountIdRole).toString();
        const bool replaceNativeSignature =
            (changedAccountId.isEmpty() || changedAccountId == selectedAccountId) &&
            m_signatureTracked && !m_signatureCustom && !m_signatureExplicitlyRemoved;
        loadIdentities();
        const auto selectedIdentityStillAvailable =
            m_fromCombo->currentIndex() >= 0 &&
            m_fromCombo->currentData(senderAccountIdRole).toString().toStdString() ==
                m_snapshot.accountId &&
            m_fromCombo->currentData(senderIdentityIdRole).toString().toStdString() ==
                m_snapshot.identityId;
        if (replaceNativeSignature && selectedIdentityStillAvailable)
            replaceTrackedSignatureForIndex(m_fromCombo->currentIndex(), true);
        Q_EMIT toolbarStateChanged();
    }

    QString ComposeTabWidget::signaturePlainTextForIndex(const int index) const
    {
        if (index < 0 || index >= m_fromCombo->count())
            return {};
        const auto text = m_fromCombo->itemData(index, senderTextSignatureRole).toString();
        const auto html = m_fromCombo->itemData(index, senderHtmlSignatureRole).toString();
        if (m_snapshot.editorMode == javelin::jmap::submission::BodyEditorMode::RichText &&
            !html.isEmpty())
            return plainTextFromHtml(html);
        if (!text.isEmpty())
            return text;
        return html.isEmpty() ? QString{} : plainTextFromHtml(html);
    }

    QString ComposeTabWidget::signatureHtmlForIndex(const int index) const
    {
        if (index < 0 || index >= m_fromCombo->count())
            return {};
        const auto html = m_fromCombo->itemData(index, senderHtmlSignatureRole).toString();
        if (!html.isEmpty())
            return html;
        const auto text = m_fromCombo->itemData(index, senderTextSignatureRole).toString();
        return text.isEmpty() ? QString{} : htmlFromPlainText(text);
    }

    int ComposeTabWidget::defaultSignatureInsertionPosition() const
    {
        if (m_signatureInsertionPosition >= 0 &&
            m_signatureInsertionPosition <= m_richTextEdit->document()->characterCount() - 1)
            return m_signatureInsertionPosition;

        for (auto block = m_richTextEdit->document()->begin(); block.isValid();
             block = block.next())
        {
            const auto format = block.blockFormat();
            if (std::abs(format.leftMargin() - 40.0) < 0.01 &&
                std::abs(format.rightMargin() - 40.0) < 0.01)
                return block.position();
            if (block.text().contains(QStringLiteral("---------- Forwarded message ----------")))
                return block.position();
        }
        return std::max(0, m_richTextEdit->document()->characterCount() - 1);
    }

    void ComposeTabWidget::initializeSignatureTracking()
    {
        m_signatureTracked = false;
        m_signatureCustom = false;
        m_signatureExplicitlyRemoved = false;
        m_signatureInsertionPosition = -1;
        const auto signature = signaturePlainTextForIndex(m_fromCombo->currentIndex());
        if (signature.trimmed().isEmpty())
        {
            m_signatureInsertionPosition = defaultSignatureInsertionPosition();
            return;
        }
        auto cursor = m_richTextEdit->document()->find(signature);
        if (cursor.isNull())
        {
            m_signatureInsertionPosition = defaultSignatureInsertionPosition();
            return;
        }
        m_signatureCursor = cursor;
        m_signatureInsertionPosition = cursor.selectionStart();
        m_signatureTracked = true;
    }

    void ComposeTabWidget::replaceTrackedSignatureForIndex(const int index, const bool forceInsert)
    {
        if (!shouldReplaceTrackedSignature(m_signatureTracked, m_signatureCustom,
                                           m_signatureExplicitlyRemoved, forceInsert))
            return;

        const auto plain = signaturePlainTextForIndex(index);
        const auto html = signatureHtmlForIndex(index);
        const bool wasTracked = m_signatureTracked;
        const bool hadExplicitRemoval = m_signatureExplicitlyRemoved;
        const int start =
            wasTracked ? m_signatureCursor.selectionStart() : defaultSignatureInsertionPosition();
        m_signatureProgrammaticEdit = true;
        QTextCursor cursor{m_richTextEdit->document()};
        cursor.setPosition(start);
        if (wasTracked)
        {
            cursor.setPosition(m_signatureCursor.selectionEnd(), QTextCursor::KeepAnchor);
            cursor.removeSelectedText();
        }
        else if (forceInsert && !hadExplicitRemoval && (!plain.isEmpty() || !html.isEmpty()))
        {
            if (m_snapshot.editorMode == javelin::jmap::submission::BodyEditorMode::RichText)
                cursor.insertHtml(QStringLiteral("<p><br/></p>"));
            else
            {
                const auto body = m_richTextEdit->toPlainText();
                cursor.insertText(body.isEmpty() ? QStringLiteral("\n") : QStringLiteral("\n\n"));
            }
        }
        const int insertionStart = cursor.position();
        if (m_snapshot.editorMode == javelin::jmap::submission::BodyEditorMode::RichText)
        {
            if (!html.isEmpty())
                cursor.insertHtml(html);
        }
        else if (!plain.isEmpty())
            cursor.insertText(plain);
        const int insertionEnd = cursor.position();
        m_signatureProgrammaticEdit = false;

        m_signatureInsertionPosition = insertionStart;
        m_signatureTracked = insertionEnd > insertionStart;
        m_signatureCustom = false;
        m_signatureExplicitlyRemoved = false;
        if (m_signatureTracked)
        {
            m_signatureCursor = QTextCursor{m_richTextEdit->document()};
            m_signatureCursor.setPosition(insertionStart);
            m_signatureCursor.setPosition(insertionEnd, QTextCursor::KeepAnchor);
        }
        scheduleWorkingCopySave();
    }

    void ComposeTabWidget::removeTrackedSignature()
    {
        if (!m_signatureTracked)
        {
            m_signatureExplicitlyRemoved = true;
            return;
        }
        m_signatureInsertionPosition = m_signatureCursor.selectionStart();
        m_signatureProgrammaticEdit = true;
        auto cursor = m_signatureCursor;
        cursor.removeSelectedText();
        m_signatureProgrammaticEdit = false;
        m_signatureTracked = false;
        m_signatureCustom = false;
        m_signatureExplicitlyRemoved = true;
        scheduleWorkingCopySave();
    }

    void ComposeTabWidget::refreshSignatureMenu()
    {
        m_signatureMenu->clear();
        auto* useIdentity = m_signatureMenu->addAction(i18n("Use Identity Signature"));
        connect(useIdentity, &QAction::triggered, this,
                [this]
                {
                    m_signatureExplicitlyRemoved = false;
                    m_signatureCustom = false;
                    replaceTrackedSignatureForIndex(m_fromCombo->currentIndex(), true);
                });
        auto* noSignature = m_signatureMenu->addAction(i18n("No Signature for This Message"));
        connect(noSignature, &QAction::triggered, this, &ComposeTabWidget::removeTrackedSignature);

        const auto email = m_fromCombo->currentData(senderEmailRole).toString();
        if (!email.isEmpty())
        {
            auto* variants = m_signatureMenu->addMenu(i18n("Signature Variant"));
            for (int index = 0; index < m_fromCombo->count(); ++index)
            {
                if (m_fromCombo->itemData(index, senderEmailRole)
                        .toString()
                        .compare(email, Qt::CaseInsensitive) != 0)
                    continue;
                auto* action = variants->addAction(m_fromCombo->itemText(index));
                action->setCheckable(true);
                action->setChecked(index == m_fromCombo->currentIndex());
                connect(action, &QAction::triggered, this,
                        [this, index] { m_fromCombo->setCurrentIndex(index); });
            }
        }

        m_signatureMenu->addSeparator();
        const auto openIdentityManager = [this]
        {
            Q_EMIT manageIdentitiesRequested(
                m_fromCombo->currentData(senderAccountIdRole).toString(),
                m_fromCombo->currentData(senderIdentityIdRole).toString());
        };
        auto* editCurrent = m_signatureMenu->addAction(i18n("Edit Current Signature…"));
        editCurrent->setEnabled(m_fromCombo->currentIndex() >= 0);
        connect(editCurrent, &QAction::triggered, this, openIdentityManager);
        auto* manage = m_signatureMenu->addAction(i18n("Manage Identities and Signatures…"));
        connect(manage, &QAction::triggered, this, openIdentityManager);
    }

    void ComposeTabWidget::applySnapshotToUi()
    {
        m_syncingUi = true;
        const QSignalBlocker fromBlocker{m_fromCombo};
        const QSignalBlocker toBlocker{m_toEdit};
        const QSignalBlocker ccBlocker{m_ccEdit};
        const QSignalBlocker bccBlocker{m_bccEdit};
        const QSignalBlocker subjectBlocker{m_subjectEdit};
        const QSignalBlocker richBlocker{m_richTextEdit};
        const QSignalBlocker tabBlocker{m_editorTabs};

        int identityIndex = -1;
        for (int index = 0; index < m_fromCombo->count(); ++index)
        {
            if (m_fromCombo->itemData(index, senderAccountIdRole).toString().toStdString() ==
                    m_snapshot.accountId &&
                m_fromCombo->itemData(index, senderIdentityIdRole).toString().toStdString() ==
                    m_snapshot.identityId)
            {
                identityIndex = index;
                break;
            }
        }
        if (identityIndex >= 0)
        {
            m_fromCombo->setCurrentIndex(identityIndex);
        }
        m_previousIdentityIndex = m_fromCombo->currentIndex();

        m_toEdit->setText(formatAddresses(m_snapshot.to));
        m_ccEdit->setText(formatAddresses(m_snapshot.cc));
        m_bccEdit->setText(formatAddresses(m_snapshot.bcc));
        m_subjectEdit->setText(m_snapshot.subject.has_value()
                                   ? QString::fromStdString(*m_snapshot.subject)
                                   : QString{});

        if (m_snapshot.editorMode == javelin::jmap::submission::BodyEditorMode::RawHtml)
        {
            m_snapshot.editorMode = javelin::jmap::submission::BodyEditorMode::RichText;
        }
        const bool plainTextMode =
            m_snapshot.editorMode == javelin::jmap::submission::BodyEditorMode::PlainText;
        if (plainTextMode)
        {
            m_richTextEdit->setPlainText(QString::fromStdString(m_snapshot.plainTextBody));
            m_richTextEdit->forcePlainTextMarkup(false);
            m_richTextEdit->switchToPlainText();
        }
        else
        {
            setEditorHtml(QString::fromStdString(m_snapshot.htmlBody));
        }
        m_editorTabs->setTabVisible(previewTabIndex, !plainTextMode);
        m_editorTabs->setCurrentIndex(richEditorTabIndex);
        setOptionalRecipientVisible(m_ccRow, m_ccButton, !m_snapshot.cc.empty());
        setOptionalRecipientVisible(m_bccRow, m_bccButton, !m_snapshot.bcc.empty());
        populateAttachments();
        m_syncingUi = false;
        initializeSignatureTracking();
        QTextCursor cursor{m_richTextEdit->document()};
        cursor.setPosition(0);
        m_richTextEdit->setTextCursor(cursor);
    }

    void ComposeTabWidget::populateAttachments()
    {
        m_attachmentScrollArea->setVisible(!m_snapshot.attachments.empty());
        while (m_attachmentStripLayout->count() > 0)
        {
            auto* item = m_attachmentStripLayout->takeAt(0);
            if (auto* widget = item->widget())
            {
                widget->deleteLater();
            }
            delete item;
        }

        for (std::size_t index = 0; index < m_snapshot.attachments.size(); ++index)
        {
            auto* chip = new DraftAttachmentChip(
                m_snapshot.attachments[index],
                m_snapshot.editorMode != javelin::jmap::submission::BodyEditorMode::PlainText,
                [this, index] { removeAttachmentAt(index); }, [this, index](const bool embedded)
                { setAttachmentEmbedded(index, embedded); }, m_attachmentStrip);
            chip->setEnabled(!m_operationInFlight);
            m_attachmentStripLayout->addWidget(chip);
        }
        m_attachmentStripLayout->addStretch(1);
    }

    void ComposeTabWidget::refreshPreview()
    {
        if (m_snapshot.editorMode == javelin::jmap::submission::BodyEditorMode::PlainText)
        {
            m_previewView->clearDocument();
            return;
        }
        m_previewView->setDocumentHtml(stableEditorHtml().toStdString());
    }

    void ComposeTabWidget::syncSnapshotFromUi()
    {
        const auto selectedAccountId =
            m_fromCombo->currentData(senderAccountIdRole).toString().toStdString();
        const auto selectedIdentityId =
            m_fromCombo->currentData(senderIdentityIdRole).toString().toStdString();
        if (!selectedAccountId.empty() && !selectedIdentityId.empty())
        {
            const bool selectedAccountChanged = selectedAccountId != m_snapshot.accountId;
            m_snapshot.accountId = selectedAccountId;
            m_snapshot.identityId = selectedIdentityId;
            if (selectedAccountChanged)
            {
                m_snapshot.draftEmailId.reset();
                Q_EMIT accountChanged(QString::fromStdString(m_snapshot.accountId));
            }
        }
        m_snapshot.to = parseAddresses(m_toEdit->text());
        m_snapshot.cc = parseAddresses(m_ccEdit->text());
        m_snapshot.bcc = parseAddresses(m_bccEdit->text());
        m_snapshot.subject =
            m_subjectEdit->text().trimmed().isEmpty()
                ? std::nullopt
                : std::optional<std::string>{m_subjectEdit->text().trimmed().toStdString()};

        switch (m_snapshot.editorMode)
        {
        case javelin::jmap::submission::BodyEditorMode::RawHtml:
            m_snapshot.editorMode = javelin::jmap::submission::BodyEditorMode::RichText;
            [[fallthrough]];
        case javelin::jmap::submission::BodyEditorMode::RichText:
        {
            const auto html = stableEditorHtml();
            reconcileInlineAttachmentReferences(html);
            m_snapshot.htmlBody = html.toStdString();
            m_snapshot.plainTextBody = m_richTextEdit->toCleanPlainText().toStdString();
            break;
        }
        case javelin::jmap::submission::BodyEditorMode::PlainText:
            m_snapshot.plainTextBody = m_richTextEdit->toCleanPlainText().toStdString();
            m_snapshot.htmlBody.clear();
            break;
        }

        ++m_snapshot.revision;
        updateTabTitle();
    }

    void ComposeTabWidget::switchBodyFormat(const bool richText)
    {
        if (m_syncingUi)
        {
            return;
        }

        const bool plainTextMode = !richText;
        const bool restoreNativeSignature =
            m_signatureTracked && !m_signatureCustom && !m_signatureExplicitlyRemoved;
        const int selectedIdentityIndex = m_fromCombo->currentIndex();
        const auto removeNativeSignature = [this, restoreNativeSignature]
        {
            if (!restoreNativeSignature)
                return;
            m_signatureInsertionPosition = m_signatureCursor.selectionStart();
            m_signatureProgrammaticEdit = true;
            auto signatureCursor = m_signatureCursor;
            signatureCursor.removeSelectedText();
            m_signatureProgrammaticEdit = false;
            m_signatureTracked = false;
        };
        if (plainTextMode &&
            m_snapshot.editorMode != javelin::jmap::submission::BodyEditorMode::PlainText)
        {
            bool useMarkup = false;
            if (m_richTextEdit->composerControler()->isFormattingUsed())
            {
                QMessageBox warning{
                    QMessageBox::Warning, i18n("Convert to Plain Text"),
                    i18n("This message contains formatting. How should it be converted to plain "
                         "text?"),
                    QMessageBox::NoButton, this};
                QAbstractButton* loseFormatting = warning.addButton(
                    i18nc("@action:button", "Lose Formatting"), QMessageBox::DestructiveRole);
                QPushButton* addMarkup = warning.addButton(
                    i18nc("@action:button", "Add Markup Plain Text"), QMessageBox::AcceptRole);
                QAbstractButton* cancel = warning.addButton(QMessageBox::Cancel);
                warning.setDefaultButton(addMarkup);
                warning.exec();

                if (warning.clickedButton() == cancel || warning.clickedButton() == nullptr)
                {
                    Q_EMIT toolbarStateChanged();
                    return;
                }
                useMarkup = warning.clickedButton() == addMarkup;
                Q_UNUSED(loseFormatting);
            }

            removeNativeSignature();
            m_syncingUi = true;
            m_richTextEdit->forcePlainTextMarkup(useMarkup);
            m_richTextEdit->switchToPlainText();
            m_snapshot.editorMode = javelin::jmap::submission::BodyEditorMode::PlainText;
            m_syncingUi = false;
        }
        else if (richText &&
                 m_snapshot.editorMode == javelin::jmap::submission::BodyEditorMode::PlainText)
        {
            removeNativeSignature();
            m_syncingUi = true;
            m_richTextEdit->activateRichText();
            m_snapshot.editorMode = javelin::jmap::submission::BodyEditorMode::RichText;
            m_syncingUi = false;
        }
        if (restoreNativeSignature)
            replaceTrackedSignatureForIndex(selectedIdentityIndex, true);

        if (const auto error = ComposeUiPreferences::setRichTextDefault(m_settings, richText))
        {
            Q_EMIT statusMessageRequested(
                i18n("Could not remember the compose format: %1", error->detail), 10000);
        }
        m_editorTabs->setCurrentIndex(richEditorTabIndex);
        m_editorTabs->setTabVisible(previewTabIndex, !plainTextMode);

        Q_EMIT toolbarStateChanged();
        populateAttachments();
        updateEditorModeUi();
        syncSnapshotFromUi();
        refreshPreview();
        scheduleWorkingCopySave();
    }

    void ComposeTabWidget::setOptionalRecipientVisible(QWidget* row, QToolButton* button,
                                                       const bool visible)
    {
        const QSignalBlocker blocker{button};
        button->setChecked(visible);
        row->setVisible(visible);
    }

    void ComposeTabWidget::scheduleWorkingCopySave()
    {
        if (m_operationInFlight)
        {
            return;
        }

        m_autosaveTimer->start();
    }

    void ComposeTabWidget::persistWorkingCopy()
    {
        if (m_pendingInlineImageJobs != 0)
            return;

        syncSnapshotFromUi();
        if (const auto error = m_composeCommandPort.storeWorkingCopy(m_snapshot))
        {
            Q_EMIT statusMessageRequested(error->message, 10000);
            return;
        }
    }

    void ComposeTabWidget::setBusy(const bool busy)
    {
        m_operationInFlight = busy;
        m_fromCombo->setEnabled(!busy);
        m_signatureButton->setEnabled(!busy);
        m_toEdit->setEnabled(!busy);
        m_ccEdit->setEnabled(!busy);
        m_bccEdit->setEnabled(!busy);
        m_ccButton->setEnabled(!busy);
        m_bccButton->setEnabled(!busy);
        m_subjectEdit->setEnabled(!busy);
        m_richTextEdit->setEnabled(!busy);
        m_editorTabs->setEnabled(!busy);
        updateEditorModeUi();
        populateAttachments();
        Q_EMIT toolbarStateChanged();
    }

    void ComposeTabWidget::updateEditorModeUi()
    {
        const bool richMode =
            m_editorTabs->currentIndex() == richEditorTabIndex &&
            m_snapshot.editorMode == javelin::jmap::submission::BodyEditorMode::RichText;
        const bool actionsEnabled = !m_operationInFlight && richMode;
        m_formatToolbar->setVisible(m_snapshot.editorMode ==
                                    javelin::jmap::submission::BodyEditorMode::RichText);
        m_formatToolbar->setEnabled(!m_operationInFlight);
        m_richTextEdit->setEnableActions(actionsEnabled);
        m_codeAction->setEnabled(actionsEnabled);
        m_insertImageAction->setEnabled(actionsEnabled);
    }

    void ComposeTabWidget::updateTabTitle()
    {
        Q_EMIT titleChanged(tabTitle());
    }

    void ComposeTabWidget::addAttachments()
    {
        const auto filePaths = QFileDialog::getOpenFileNames(this, i18n("Attach Files"));
        addAttachmentPaths(filePaths);
    }

    void ComposeTabWidget::attachFiles()
    {
        if (!m_operationInFlight)
            addAttachments();
    }

    void ComposeTabWidget::saveDraft()
    {
        startSaveDraft(false);
    }

    void ComposeTabWidget::sendMessage()
    {
        startSend();
    }

    void ComposeTabWidget::addAttachmentPaths(const QStringList& filePaths)
    {
        for (const auto& filePath : filePaths)
        {
            const QFileInfo info{filePath};
            if (!info.exists() || !info.isFile())
            {
                continue;
            }

            m_snapshot.attachments.push_back(javelin::jmap::submission::DraftAttachment{
                .localFilePath = filePath.toStdString(),
                .displayName = info.fileName().toStdString(),
                .mediaType = detectedMediaType(filePath).toStdString(),
                .size = static_cast<std::uint64_t>(info.size()),
                .blobId = std::nullopt,
                .inlineDisposition = false,
                .contentId = std::nullopt,
                .contentHash = std::nullopt,
            });
        }

        populateAttachments();
        scheduleWorkingCopySave();
    }

    void ComposeTabWidget::addInlineImagePath(const QString& filePath)
    {
        const QFileInfo info{filePath};
        const QImage image{filePath};
        if (!info.exists() || !info.isFile() || image.isNull())
        {
            Q_EMIT statusMessageRequested(i18n("The selected image could not be loaded."), 7000);
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
        populateAttachments();
        scheduleWorkingCopySave();
    }

    void ComposeTabWidget::addPastedInlineImage(const QImage& image)
    {
        if (image.isNull())
        {
            return;
        }

        const auto directory = draftAssetDirectory(m_snapshot.composeSessionId);
        if (!QDir{}.mkpath(directory))
        {
            Q_EMIT statusMessageRequested(i18n("Could not create storage for the pasted image."),
                                          10000);
            return;
        }

        const auto fileName =
            QStringLiteral("pasted-%1.png").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        const auto filePath = QDir{directory}.filePath(fileName);
        if (!image.save(filePath, "PNG"))
        {
            Q_EMIT statusMessageRequested(i18n("Could not save the pasted image."), 10000);
            return;
        }
        addInlineImagePath(filePath);
    }

    void ComposeTabWidget::insertImage()
    {
        const auto insertionPosition = m_richTextEdit->textCursor().selectionStart();
        auto* document = m_richTextEdit->document();
        const auto documentRevision = document->revision();
        auto timing = std::make_shared<ImageInsertTiming>();
        timing->elapsed.start();

        qCInfo(logComposeImage).noquote()
            << "insert dialog opened"
            << "documentRevision" << documentRevision << "insertionPosition" << insertionPosition;

        timing->documentChangeConnection =
            connect(document, &QTextDocument::contentsChange, this,
                    [timing](const int position, const int removed, const int added)
                    {
                        const auto elapsedMilliseconds = timing->elapsed.elapsed();
                        const auto postAcceptMilliseconds =
                            timing->acceptedAtMilliseconds.has_value()
                                ? elapsedMilliseconds - *timing->acceptedAtMilliseconds
                                : -1;
                        qCInfo(logComposeImage).noquote()
                            << "editor document changed"
                            << "elapsedMs" << elapsedMilliseconds << "postAcceptMs"
                            << postAcceptMilliseconds << "position" << position << "removed"
                            << removed << "added" << added;
                        QObject::disconnect(timing->documentChangeConnection);
                    });

        QTimer::singleShot(
            0, this,
            [this, timing]
            {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (dialog == nullptr)
                {
                    qCWarning(logComposeImage)
                        << "insert dialog instrumentation could not find the active modal dialog";
                    return;
                }
                connect(
                    dialog, &QDialog::accepted, this,
                    [timing, dialog]
                    {
                        timing->acceptedAtMilliseconds = timing->elapsed.elapsed();
                        timing->sourceFilePath = imageDialogSourceFilePath(*dialog);
                        qCInfo(logComposeImage).noquote()
                            << "insert dialog accepted"
                            << "elapsedMs" << *timing->acceptedAtMilliseconds << "sourceFile"
                            << timing->sourceFilePath;
                    },
                    Qt::SingleShotConnection);
            });

        m_richTextEdit->composerControler()->slotAddImage();
        QObject::disconnect(timing->documentChangeConnection);

        const auto elapsedMilliseconds = timing->elapsed.elapsed();
        const auto postAcceptMilliseconds =
            timing->acceptedAtMilliseconds.has_value()
                ? elapsedMilliseconds - *timing->acceptedAtMilliseconds
                : -1;
        const bool documentChanged = document->revision() != documentRevision;
        qCInfo(logComposeImage).noquote()
            << "KDE image insertion returned"
            << "elapsedMs" << elapsedMilliseconds << "postAcceptMs" << postAcceptMilliseconds
            << "documentChanged" << documentChanged << "documentRevision" << document->revision();

        if (documentChanged)
        {
            adoptInsertedComposerImage(insertionPosition, timing->sourceFilePath);
        }
    }

    void ComposeTabWidget::adoptInsertedComposerImage(const int insertionPosition,
                                                      const QString& sourceFilePath)
    {
        QElapsedTimer dispatchElapsed;
        dispatchElapsed.start();
        auto* document = m_richTextEdit->document();
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
            qCWarning(logComposeImage).noquote()
                << "inserted image adoption failed to locate the image fragment"
                << "elapsedMs" << dispatchElapsed.elapsed() << "insertionPosition"
                << insertionPosition;
            Q_EMIT statusMessageRequested(i18n("The inserted image could not be tracked."), 10000);
            return;
        }

        auto imageFormat = insertedFragment.charFormat().toImageFormat();
        const auto originalResourceName = imageFormat.name();
        const auto image = qvariant_cast<QImage>(
            document->resource(QTextDocument::ImageResource, QUrl{originalResourceName}));
        if (image.isNull())
        {
            qCWarning(logComposeImage).noquote()
                << "inserted image adoption could not retrieve the document resource"
                << "elapsedMs" << dispatchElapsed.elapsed() << "resourceName"
                << originalResourceName;
            Q_EMIT statusMessageRequested(i18n("The inserted image could not be loaded."), 10000);
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
        ++m_pendingInlineImageJobs;
        Q_EMIT toolbarStateChanged();

        connect(watcher, &QFutureWatcher<PreparedInlineImage>::finished, this,
                [this, watcher, resourceName, contentId]
                {
                    const auto result = watcher->result();
                    watcher->deleteLater();

                    const bool stillReferenced =
                        documentContainsImageResource(*m_richTextEdit->document(), resourceName);
                    if (!result.succeeded())
                    {
                        qCWarning(logComposeImage).noquote()
                            << "inserted image preparation failed"
                            << "processingMs" << result.processingMilliseconds << "error"
                            << result.error;
                        if (stillReferenced)
                            removeImageResource(*m_richTextEdit->document(), resourceName);
                        if (m_deferredOperation == DeferredOperation::Send)
                            m_deferredOperation = DeferredOperation::None;
                        Q_EMIT statusMessageRequested(
                            i18n("The inserted image could not be prepared."), 10000);
                        finishInlineImagePreparation();
                        return;
                    }

                    if (!stillReferenced)
                    {
                        QFile::remove(result.filePath);
                        qCInfo(logComposeImage).noquote()
                            << "prepared image discarded because it was removed from the editor"
                            << "processingMs" << result.processingMilliseconds;
                        finishInlineImagePreparation();
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
                    populateAttachments();
                    qCInfo(logComposeImage).noquote()
                        << "inserted image preparation complete"
                        << "processingMs" << result.processingMilliseconds << "reencoded"
                        << result.reencoded << "encodedBytes" << result.size << "mediaType"
                        << result.mediaType << "attachmentCount" << m_snapshot.attachments.size();
                    finishInlineImagePreparation();
                });

        watcher->setFuture(QtConcurrent::run(
            [sourceFilePath, image, destinationDirectory, assetId]
            { return prepareInlineImage(sourceFilePath, image, destinationDirectory, assetId); }));
        qCInfo(logComposeImage).noquote()
            << "inserted image preparation dispatched"
            << "dispatchMs" << dispatchElapsed.elapsed() << "width" << image.width() << "height"
            << image.height() << "decodedBytes" << image.sizeInBytes() << "sourceFile"
            << sourceFilePath;
    }

    void ComposeTabWidget::finishInlineImagePreparation()
    {
        if (m_pendingInlineImageJobs == 0)
            return;

        --m_pendingInlineImageJobs;
        Q_EMIT toolbarStateChanged();
        if (m_pendingInlineImageJobs != 0)
            return;

        scheduleWorkingCopySave();
        const auto deferredOperation = std::exchange(m_deferredOperation, DeferredOperation::None);
        QTimer::singleShot(0, this,
                           [this, deferredOperation]
                           {
                               switch (deferredOperation)
                               {
                               case DeferredOperation::None:
                                   break;
                               case DeferredOperation::SaveDraft:
                                   startSaveDraft(false);
                                   break;
                               case DeferredOperation::SaveDraftAndClose:
                                   startSaveDraft(true);
                                   break;
                               case DeferredOperation::Send:
                                   startSend();
                                   break;
                               }
                           });
    }

    void ComposeTabWidget::removeAttachmentAt(const std::size_t index)
    {
        if (index >= m_snapshot.attachments.size())
        {
            return;
        }

        if (m_snapshot.attachments[index].contentId.has_value())
        {
            removeEmbeddedImageReference(*m_snapshot.attachments[index].contentId);
        }
        m_snapshot.attachments.erase(m_snapshot.attachments.begin() +
                                     static_cast<std::ptrdiff_t>(index));
        populateAttachments();
        refreshPreview();
        syncSnapshotFromUi();
        scheduleWorkingCopySave();
    }

    void ComposeTabWidget::setAttachmentEmbedded(const std::size_t index, const bool embedded)
    {
        if (index >= m_snapshot.attachments.size() ||
            m_snapshot.editorMode == javelin::jmap::submission::BodyEditorMode::PlainText ||
            embedded == m_snapshot.attachments[index].inlineDisposition)
        {
            return;
        }

        auto& attachment = m_snapshot.attachments[index];
        attachment.inlineDisposition = embedded;
        if (embedded)
        {
            if (!attachment.contentId.has_value())
            {
                attachment.contentId = newContentId();
            }
            insertEmbeddedImage(index);
        }
        else if (attachment.contentId.has_value())
        {
            removeEmbeddedImageReference(*attachment.contentId);
            attachment.contentId = std::nullopt;
        }

        populateAttachments();
        refreshPreview();
        syncSnapshotFromUi();
        scheduleWorkingCopySave();
    }

    void ComposeTabWidget::insertEmbeddedImage(const std::size_t index)
    {
        if (index >= m_snapshot.attachments.size())
        {
            return;
        }

        const auto& attachment = m_snapshot.attachments[index];
        if (!attachment.contentId.has_value())
        {
            return;
        }

        const QImage image{QString::fromStdString(attachment.localFilePath)};
        if (image.isNull())
        {
            return;
        }

        const auto resourceName = composerEditorResourceName(*attachment.contentId);
        const auto width = std::min(image.width(), 720);
        const auto height = image.width() > 0 ? image.height() * width / image.width() : -1;
        m_richTextEdit->composerControler()->composerImages()->addImageHelper(resourceName, image,
                                                                              width, height);
    }

    void ComposeTabWidget::removeEmbeddedImageReference(const std::string& contentId)
    {
        const auto cidUrl = composerContentIdUrl(contentId);
        const QRegularExpression imageTagPattern{
            QStringLiteral("<img\\b[^>]*\\bsrc\\s*=\\s*([\"'])%1\\1[^>]*>")
                .arg(QRegularExpression::escape(cidUrl)),
            QRegularExpression::CaseInsensitiveOption};

        auto html = stableEditorHtml();
        html.remove(imageTagPattern);
        setEditorHtml(html);
    }

    void ComposeTabWidget::setEditorHtml(const QString& html)
    {
        if (m_richTextEdit->textMode() == KPIMTextEdit::RichTextComposer::Plain)
        {
            m_richTextEdit->activateRichText();
        }
        m_richTextEdit->setTextOrHtml(
            htmlForQtDocument(editorHtmlForInlineAttachments(html, m_snapshot.attachments)));
        loadInlineImageResources();
    }

    void ComposeTabWidget::loadInlineImageResources()
    {
        auto* images = m_richTextEdit->composerControler()->composerImages();
        for (const auto& attachment : m_snapshot.attachments)
        {
            if (!attachment.inlineDisposition || !attachment.contentId.has_value() ||
                attachment.localFilePath.empty())
            {
                continue;
            }

            const QImage image{QString::fromStdString(attachment.localFilePath)};
            if (image.isNull())
            {
                continue;
            }
            const auto resourceName = composerEditorResourceName(*attachment.contentId);
            images->loadImage(image, resourceName, resourceName);
        }
    }

    QString ComposeTabWidget::stableEditorHtml()
    {
        return stableHtmlForInlineAttachments(m_richTextEdit->toCleanHtml(),
                                              m_snapshot.attachments);
    }

    void ComposeTabWidget::reconcileInlineAttachmentReferences(const QString& html)
    {
        if (reconcileInlineAttachments(m_snapshot.attachments, html))
        {
            populateAttachments();
        }
    }

    void ComposeTabWidget::startSaveDraft(const bool closeAfterSave)
    {
        if (m_operationInFlight)
        {
            return;
        }
        if (m_pendingInlineImageJobs != 0)
        {
            m_deferredOperation = closeAfterSave ? DeferredOperation::SaveDraftAndClose
                                                 : DeferredOperation::SaveDraft;
            Q_EMIT statusMessageRequested(i18n("Finishing image processing before saving…"), 10000);
            return;
        }

        syncSnapshotFromUi();
        QString errorMessage;
        const auto settings = liveSettings(m_settings, m_snapshot.accountId, &errorMessage);
        if (!settings.has_value())
        {
            Q_EMIT statusMessageRequested(errorMessage, 10000);
            Q_EMIT userInterventionRequired(errorMessage);
            return;
        }

        if (m_autosaveTimer->isActive())
        {
            m_autosaveTimer->stop();
        }
        if (const auto error = m_composeCommandPort.storeWorkingCopy(m_snapshot))
        {
            Q_EMIT statusMessageRequested(error->message, 10000);
            return;
        }

        setBusy(true);
        m_closeAfterSave = closeAfterSave;
        Q_EMIT statusMessageRequested(i18n("Saving draft..."), 5000);
        auto task = m_composeCommandPort.saveDraft(*settings, m_snapshot);
        QCoro::connect(
            std::move(task), this,
            [this](std::variant<javelin::jmap::submission::DraftSaveSummary,
                                javelin::jmap::OperationError>
                       result)
            {
                setBusy(false);
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    Q_EMIT statusMessageRequested(error->message, 10000);
                    m_closeAfterSave = false;
                    return;
                }

                const auto& summary = std::get<javelin::jmap::submission::DraftSaveSummary>(result);
                m_snapshot = summary.savedSnapshot;
                populateAttachments();
                if (const auto error = m_composeCommandPort.storeWorkingCopy(m_snapshot))
                {
                    Q_EMIT statusMessageRequested(error->message, 10000);
                    m_closeAfterSave = false;
                    return;
                }

                Q_EMIT statusMessageRequested(i18n("Draft saved."), 5000);
                if (m_closeAfterSave)
                {
                    m_closeAfterSave = false;
                    if (const auto error =
                            m_composeCommandPort.discard(m_snapshot.composeSessionId))
                    {
                        Q_EMIT statusMessageRequested(error->message, 10000);
                        return;
                    }
                    m_closeWithoutPrompt = true;
                    Q_EMIT closeRequested();
                }
            });
    }

    void ComposeTabWidget::startSend()
    {
        if (m_operationInFlight)
        {
            return;
        }
        if (!canSend())
        {
            Q_EMIT statusMessageRequested(
                i18n("Choose an available sender identity before sending."), 10000);
            return;
        }
        if (m_pendingInlineImageJobs != 0)
        {
            m_deferredOperation = DeferredOperation::Send;
            Q_EMIT statusMessageRequested(i18n("Finishing image processing before sending…"),
                                          10000);
            return;
        }

        syncSnapshotFromUi();
        QString errorMessage;
        const auto settings = liveSettings(m_settings, m_snapshot.accountId, &errorMessage);
        if (!settings.has_value())
        {
            Q_EMIT statusMessageRequested(errorMessage, 10000);
            Q_EMIT userInterventionRequired(errorMessage);
            return;
        }

        if (m_snapshot.to.empty())
        {
            Q_EMIT statusMessageRequested(i18n("Add at least one recipient before sending."), 7000);
            return;
        }

        if (!m_snapshot.subject.has_value())
        {
            QMessageBox messageBox{this};
            messageBox.setWindowTitle(i18n("Send Without Subject?"));
            messageBox.setText(i18n("This message has no subject. Send it anyway?"));
            QAbstractButton* sendAnywayButton =
                messageBox.addButton(i18n("Send Anyway"), QMessageBox::AcceptRole);
            messageBox.addButton(QMessageBox::Cancel);
            messageBox.exec();
            if (messageBox.clickedButton() != sendAnywayButton)
            {
                m_subjectEdit->setFocus();
                return;
            }
        }

        setBusy(true);
        Q_EMIT statusMessageRequested(i18n("Sending message..."), 5000);
        auto task = m_composeCommandPort.send(*settings, m_snapshot);
        QCoro::connect(
            std::move(task), this,
            [this](
                std::variant<javelin::jmap::submission::SendSummary, javelin::jmap::OperationError>
                    result)
            {
                setBusy(false);
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    Q_EMIT statusMessageRequested(error->message, 10000);
                    return;
                }

                const auto& summary = std::get<javelin::jmap::submission::SendSummary>(result);
                m_snapshot.draftEmailId = summary.draftEmailId;
                m_closeWithoutPrompt = true;
                Q_EMIT statusMessageRequested(
                    summary.scheduled ? i18n("Message scheduled.") : i18n("Message sent."), 7000);
                Q_EMIT closeRequested();
            });
    }

    void ComposeTabWidget::toggleCode()
    {
        QTextCharFormat format;
        if (m_codeAction->isChecked())
        {
            format.setFontFamilies(QStringList{QStringLiteral("monospace")});
        }
        else
        {
            format.clearProperty(QTextFormat::FontFamilies);
        }
        m_richTextEdit->mergeCurrentCharFormat(format);
    }

} // namespace javelin::gui::compose
