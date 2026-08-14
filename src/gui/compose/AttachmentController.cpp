#include "gui/compose/AttachmentController.h"

#include "jmap/submission/ComposeTypes.h"

#include <KLocalizedString>

#include <QApplication>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMimeDatabase>
#include <QRadioButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QStyle>
#include <QToolButton>
#include <QWidget>

#include <cstdint>
#include <utility>

namespace javelin::gui::compose
{
    namespace
    {
        [[nodiscard]] QString attachmentSizeLabel(const std::uint64_t size)
        {
            constexpr double kib = 1024.0;
            constexpr double mib = 1024.0 * kib;
            constexpr double gib = 1024.0 * mib;

            if (size >= static_cast<std::uint64_t>(gib))
                return i18nc("@item file size", "%1 GB",
                             QString::number(static_cast<double>(size) / gib, 'f', 1));
            if (size >= static_cast<std::uint64_t>(mib))
                return i18nc("@item file size", "%1 MB",
                             QString::number(static_cast<double>(size) / mib, 'f', 1));
            if (size >= static_cast<std::uint64_t>(kib))
                return i18nc("@item file size", "%1 KB",
                             QString::number(static_cast<double>(size) / kib, 'f', 1));
            return i18nc("@item file size", "%1 B", static_cast<qulonglong>(size));
        }

        [[nodiscard]] QString
        attachmentDisplayName(const javelin::jmap::submission::DraftAttachment& attachment)
        {
            if (!attachment.displayName.empty())
                return QString::fromStdString(attachment.displayName);
            if (!attachment.localFilePath.empty())
                return QFileInfo{QString::fromStdString(attachment.localFilePath)}.fileName();
            return i18n("Attachment");
        }

        [[nodiscard]] QString
        attachmentItemText(const javelin::jmap::submission::DraftAttachment& attachment)
        {
            const auto mediaType = attachment.mediaType.empty()
                                       ? i18nc("@item unknown attachment media type", "attachment")
                                       : QString::fromStdString(attachment.mediaType);
            return QStringLiteral("%1  •  %2  •  %3")
                .arg(attachmentDisplayName(attachment), mediaType,
                     attachmentSizeLabel(attachment.size));
        }

        [[nodiscard]] QIcon
        attachmentIcon(const javelin::jmap::submission::DraftAttachment& attachment)
        {
            QFileIconProvider iconProvider;
            auto icon = iconProvider.icon(QFileInfo{attachmentDisplayName(attachment)});
            if (!icon.isNull())
                return icon;

            const QMimeDatabase mimeDatabase;
            const auto mimeType =
                mimeDatabase.mimeTypeForName(QString::fromStdString(attachment.mediaType));
            icon = QIcon::fromTheme(mimeType.iconName());
            if (icon.isNull())
                icon = QIcon::fromTheme(mimeType.genericIconName());
            if (icon.isNull())
                icon = QIcon::fromTheme(QStringLiteral("mail-attachment"));
            if (icon.isNull())
                icon = QApplication::style()->standardIcon(QStyle::SP_FileIcon);
            return icon;
        }

        [[nodiscard]] bool
        isImageAttachment(const javelin::jmap::submission::DraftAttachment& attachment)
        {
            if (!attachment.mediaType.empty())
                return attachment.mediaType.rfind("image/", 0) == 0;
            if (attachment.localFilePath.empty())
                return false;
            QMimeDatabase mimeDatabase;
            return mimeDatabase
                .mimeTypeForFile(QString::fromStdString(attachment.localFilePath),
                                 QMimeDatabase::MatchContent)
                .name()
                .startsWith(QStringLiteral("image/"));
        }

        class DraftAttachmentChip final : public QWidget
        {
          public:
            DraftAttachmentChip(const javelin::jmap::submission::DraftAttachment& attachment,
                                const bool embeddingAllowed, std::function<void()> removeAction,
                                std::function<void(bool)> embedAction, QWidget* parent)
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
                                    m_embedAction(false);
                            });
                    connect(embedRadio, &QRadioButton::toggled, this,
                            [this](const bool checked)
                            {
                                if (checked && m_embedAction)
                                    m_embedAction(true);
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
                                m_removeAction();
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
    } // namespace

    AttachmentController::AttachmentController(QScrollArea& scrollArea, QWidget& strip,
                                               QHBoxLayout& layout,
                                               javelin::jmap::submission::DraftSnapshot& snapshot,
                                               std::function<void(std::size_t)> removeAction,
                                               std::function<void(std::size_t, bool)> embedAction)
        : m_scrollArea(scrollArea), m_strip(strip), m_layout(layout), m_snapshot(snapshot),
          m_removeAction(std::move(removeAction)), m_embedAction(std::move(embedAction))
    {
    }

    void AttachmentController::refresh(const bool busy)
    {
        m_scrollArea.setVisible(!m_snapshot.attachments.empty());
        while (m_layout.count() > 0)
        {
            auto* item = m_layout.takeAt(0);
            if (auto* widget = item->widget())
                widget->deleteLater();
            delete item;
        }

        const bool embeddingAllowed =
            m_snapshot.editorMode != javelin::jmap::submission::BodyEditorMode::PlainText;
        for (std::size_t index = 0; index < m_snapshot.attachments.size(); ++index)
        {
            auto* chip = new DraftAttachmentChip(
                m_snapshot.attachments[index], embeddingAllowed,
                [this, index]
                {
                    if (m_removeAction)
                        m_removeAction(index);
                },
                [this, index](const bool embedded)
                {
                    if (m_embedAction)
                        m_embedAction(index, embedded);
                },
                &m_strip);
            chip->setEnabled(!busy);
            m_layout.addWidget(chip);
        }
        m_layout.addStretch(1);
    }
} // namespace javelin::gui::compose
