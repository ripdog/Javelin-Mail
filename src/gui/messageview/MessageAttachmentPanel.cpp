#include "gui/messageview/MessageAttachmentPanel.h"

#include "gui/shell/MessageFileUtils.h"

#include "gui/settings/GuiSettings.h"

#include <KLocalizedString>

#include <QApplication>
#include <QEvent>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMimeDatabase>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QStyle>
#include <QStyleOptionToolButton>
#include <QToolButton>

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

namespace javelin::gui::messageview
{
    namespace
    {
        [[nodiscard]] QString
        attachmentName(const javelin::jmap::cache::MessageAttachment& attachment)
        {
            return QString::fromStdString(attachment.name.value_or(attachment.partId));
        }

        [[nodiscard]] QIcon
        attachmentIcon(const javelin::jmap::cache::MessageAttachment& attachment)
        {
            const QFileInfo fileInfo(attachmentName(attachment));
            QFileIconProvider iconProvider;
            auto icon = iconProvider.icon(fileInfo);
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

        [[nodiscard]] std::vector<const javelin::jmap::cache::MessageAttachment*>
        visibleAttachments(const std::optional<javelin::jmap::cache::MessageViewSnapshot>& snapshot)
        {
            std::vector<const javelin::jmap::cache::MessageAttachment*> attachments;
            if (!snapshot.has_value())
                return attachments;

            attachments.reserve(snapshot->attachments.size());
            for (const auto& attachment : snapshot->attachments)
            {
                if (javelin::gui::shell::isDownloadableAttachment(attachment))
                    attachments.push_back(&attachment);
            }
            return attachments;
        }

        class AttachmentDragButton final : public QToolButton
        {
          public:
            explicit AttachmentDragButton(std::function<void(QWidget*)> dragAction,
                                          QWidget* parent = nullptr)
                : QToolButton(parent), m_dragAction(std::move(dragAction))
            {
            }

            void setFullText(QString text)
            {
                m_fullText = std::move(text);
                setAccessibleName(m_fullText);
                QToolButton::setText(m_fullText);
            }

          protected:
            void mousePressEvent(QMouseEvent* event) override
            {
                m_dragStarted = false;
                if (event->button() == Qt::LeftButton)
                    m_pressPosition = event->position().toPoint();
                QToolButton::mousePressEvent(event);
            }

            void mouseMoveEvent(QMouseEvent* event) override
            {
                if (!m_dragStarted && event->buttons().testFlag(Qt::LeftButton) &&
                    (event->position().toPoint() - m_pressPosition).manhattanLength() >=
                        QApplication::startDragDistance())
                {
                    m_dragStarted = true;
                    if (m_dragAction)
                        m_dragAction(this);
                    return;
                }
                QToolButton::mouseMoveEvent(event);
            }

            void mouseReleaseEvent(QMouseEvent* event) override
            {
                if (m_dragStarted)
                {
                    setDown(false);
                    event->accept();
                    return;
                }
                QToolButton::mouseReleaseEvent(event);
            }

            void resizeEvent(QResizeEvent* event) override
            {
                QToolButton::resizeEvent(event);
                updateElidedText();
            }

            void changeEvent(QEvent* event) override
            {
                QToolButton::changeEvent(event);
                if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
                    updateElidedText();
            }

          private:
            void updateElidedText()
            {
                if (m_fullText.isEmpty())
                    return;

                QStyleOptionToolButton option;
                initStyleOption(&option);
                const QRect buttonRect = style()->subControlRect(QStyle::CC_ToolButton, &option,
                                                                 QStyle::SC_ToolButton, this);
                int textWidth = buttonRect.width() - 16;
                if (!icon().isNull())
                {
                    const int spacing = std::max(
                        4, style()->pixelMetric(QStyle::PM_LayoutHorizontalSpacing, &option, this));
                    textWidth -= option.iconSize.width() + spacing;
                }
                QToolButton::setText(
                    fontMetrics().elidedText(m_fullText, Qt::ElideMiddle, std::max(0, textWidth)));
            }

            std::function<void(QWidget*)> m_dragAction;
            QString m_fullText;
            QPoint m_pressPosition;
            bool m_dragStarted = false;
        };

        class AttachmentTile final : public QFrame
        {
          public:
            AttachmentTile(const javelin::jmap::cache::MessageAttachment& attachment,
                           std::function<void()> openAction, std::function<void()> openWithAction,
                           std::function<void()> saveAction,
                           std::function<void(QWidget*)> dragAction, QString saveToolTip,
                           QWidget* parent)
                : QFrame(parent)
            {
                const auto fileName = attachmentName(attachment);
                setToolTip(fileName);
                setFrameStyle(QFrame::NoFrame);
                setObjectName(QStringLiteral("attachmentTile"));
                setMinimumWidth(minimumTileWidth);
                setStyleSheet(QStringLiteral("#attachmentTile {"
                                             " background: rgba(255, 255, 255, 0.06);"
                                             " border: 1px solid rgba(255, 255, 255, 0.08);"
                                             " border-radius: 6px;"
                                             "}"
                                             "#attachmentTile QToolButton {"
                                             " background: transparent;"
                                             " border: 0;"
                                             " border-radius: 0;"
                                             " padding: 6px 8px;"
                                             "}"
                                             "#attachmentTile QToolButton:hover {"
                                             " background: rgba(255, 255, 255, 0.06);"
                                             "}"
                                             "#attachmentTile QToolButton:pressed {"
                                             " background: rgba(255, 255, 255, 0.1);"
                                             "}"
                                             "#attachmentTile #saveAttachmentButton {"
                                             " border-left: 1px solid rgba(255, 255, 255, 0.12);"
                                             "}"));

                auto* layout = new QHBoxLayout(this);
                layout->setContentsMargins(0, 0, 0, 0);
                layout->setSpacing(0);

                auto* openButton = new AttachmentDragButton(std::move(dragAction), this);
                openButton->setObjectName(QStringLiteral("attachmentOpenButton"));
                openButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
                openButton->setIcon(attachmentIcon(attachment));
                openButton->setFullText(fileName);
                openButton->setToolTip(i18n("Open %1 in default application", fileName));
                openButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
                connect(openButton, &QToolButton::clicked, this,
                        [action = std::move(openAction)]
                        {
                            if (action)
                                action();
                        });
                auto* openMenu = new QMenu(openButton);
                auto* openWith = openMenu->addAction(i18nc("@action:inmenu", "Open With…"));
                connect(openWith, &QAction::triggered, this,
                        [action = std::move(openWithAction)]
                        {
                            if (action)
                                action();
                        });
                openButton->setMenu(openMenu);
                openButton->setPopupMode(QToolButton::MenuButtonPopup);

                auto* saveButton = new QToolButton(this);
                saveButton->setObjectName(QStringLiteral("saveAttachmentButton"));
                saveButton->setIcon(QIcon::fromTheme(QStringLiteral("edit-download")));
                saveButton->setAccessibleName(saveToolTip);
                saveButton->setToolTip(std::move(saveToolTip));
                saveButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
                connect(saveButton, &QToolButton::clicked, this,
                        [action = std::move(saveAction)]
                        {
                            if (action)
                                action();
                        });

                layout->addWidget(openButton, 1);
                layout->addWidget(saveButton);
                const int preferredWidth =
                    openButton->sizeHint().width() + saveButton->sizeHint().width();
                m_targetWidth = std::clamp(preferredWidth, minimumTileWidth, maximumTileWidth);
                setMaximumWidth(maximumTileWidth);
            }

            [[nodiscard]] int targetWidth() const
            {
                return m_targetWidth;
            }

          private:
            static constexpr int minimumTileWidth = 200;
            static constexpr int maximumTileWidth = 500;
            int m_targetWidth = minimumTileWidth;
        };
    } // namespace

    MessageAttachmentPanel::MessageAttachmentPanel(
        javelin::gui::settings::GuiSettings& settings,
        std::reference_wrapper<const std::optional<std::string>> accountId,
        std::reference_wrapper<const std::optional<std::string>> emailId,
        std::reference_wrapper<const std::optional<javelin::jmap::cache::MessageViewSnapshot>>
            snapshot,
        QWidget* parent)
        : QWidget(parent), m_settings(settings), m_accountId(accountId), m_emailId(emailId),
          m_snapshot(snapshot)
    {
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);

        m_statusLabel = new QLabel(this);
        m_statusLabel->setWordWrap(false);
        m_statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_statusLabel->setCursor(Qt::IBeamCursor);
        m_statusLabel->setFocusPolicy(Qt::NoFocus);
        m_statusLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

        m_listWidget = new QWidget(this);
        m_listLayout = new QGridLayout(m_listWidget);
        m_listLayout->setContentsMargins(0, 0, 0, 0);
        m_listLayout->setHorizontalSpacing(6);
        m_listLayout->setVerticalSpacing(6);
        m_listLayout->setAlignment(Qt::AlignLeft | Qt::AlignTop);

        m_saveAllButton = new QToolButton(this);
        m_saveAllButton->setText(i18nc("@action:button", "Save All"));
        connect(m_saveAllButton, &QToolButton::clicked, this,
                [this]
                {
                    const auto& observedAccountId = m_accountId.get();
                    const auto& observedEmailId = m_emailId.get();
                    if (observedAccountId.has_value() && observedEmailId.has_value())
                    {
                        Q_EMIT saveAllAttachmentsRequested(
                            QString::fromStdString(*observedAccountId),
                            QString::fromStdString(*observedEmailId));
                    }
                });

        layout->addWidget(m_statusLabel);
        layout->addWidget(m_listWidget, 1);
        layout->addWidget(m_saveAllButton);
        refresh();
    }

    bool MessageAttachmentPanel::hasVisibleAttachments() const
    {
        return !visibleAttachments(m_snapshot.get()).empty();
    }

    QString MessageAttachmentPanel::statusText() const
    {
        const auto& snapshot = m_snapshot.get();
        if (!snapshot.has_value())
            return {};
        const auto attachments = visibleAttachments(snapshot);
        if (!attachments.empty())
            return i18np("%1 attachment", "%1 attachments", attachments.size());
        if (snapshot->htmlRenderDocument.has_value() &&
            snapshot->htmlRenderDocument->inlineResourceCount > 0)
        {
            return i18np("Inline resource: %1", "Inline resources: %1",
                         snapshot->htmlRenderDocument->inlineResourceCount);
        }
        return {};
    }

    void MessageAttachmentPanel::refresh()
    {
        const bool hasAttachments = hasVisibleAttachments();
        setVisible(hasAttachments);
        m_statusLabel->setVisible(hasAttachments);
        m_statusLabel->setText(statusText());
        m_saveAllButton->setVisible(hasAttachments);
        m_saveAllButton->setEnabled(hasAttachments);
        m_listWidget->setVisible(hasAttachments);
        rebuildRows();
    }

    void MessageAttachmentPanel::rebuildRows()
    {
        while (QLayoutItem* item = m_listLayout->takeAt(0))
            delete item;
        for (auto* tile : m_attachmentTiles)
            delete tile;
        m_attachmentTiles.clear();

        const auto& accountId = m_accountId.get();
        const auto& emailId = m_emailId.get();
        const auto attachments = visibleAttachments(m_snapshot.get());
        if (attachments.empty() || !accountId.has_value() || !emailId.has_value())
            return;

        m_attachmentTiles.reserve(attachments.size());
        const auto attachmentSaveSettings = m_settings.attachmentSaveSettings();
        for (const auto* attachment : attachments)
        {
            const auto fileName = attachmentName(*attachment);
            const auto saveToolTip =
                attachmentSaveSettings.alwaysAsk
                    ? i18n("Save %1 to selected location", fileName)
                    : i18n("Save %1 to %2", fileName, attachmentSaveSettings.directory);
            const auto partId = QString::fromStdString(attachment->partId);
            m_attachmentTiles.push_back(new AttachmentTile(
                *attachment,
                [this, partId]
                {
                    Q_EMIT openAttachmentRequested(QString::fromStdString(*m_accountId.get()),
                                                   QString::fromStdString(*m_emailId.get()),
                                                   partId);
                },
                [this, partId]
                {
                    Q_EMIT openAttachmentWithRequested(QString::fromStdString(*m_accountId.get()),
                                                       QString::fromStdString(*m_emailId.get()),
                                                       partId);
                },
                [this, partId]
                {
                    Q_EMIT saveAttachmentRequested(QString::fromStdString(*m_accountId.get()),
                                                   QString::fromStdString(*m_emailId.get()),
                                                   partId);
                },
                [this, partId](QWidget* source)
                {
                    Q_EMIT dragAttachmentRequested(QString::fromStdString(*m_accountId.get()),
                                                   QString::fromStdString(*m_emailId.get()), partId,
                                                   source);
                },
                saveToolTip, m_listWidget));
        }
        reflowRows();
    }

    void MessageAttachmentPanel::reflowRows()
    {
        while (QLayoutItem* item = m_listLayout->takeAt(0))
            delete item;
        if (m_attachmentTiles.empty())
            return;

        constexpr int tileSpacing = 6;
        const int availableWidth =
            std::max(1, std::max(m_listWidget->contentsRect().width(),
                                 contentsRect().width() - m_statusLabel->sizeHint().width() -
                                     m_saveAllButton->sizeHint().width() - 12));

        for (std::size_t column = 0; column <= m_attachmentTiles.size(); ++column)
            m_listLayout->setColumnStretch(static_cast<int>(column), 0);

        int row = 0;
        int column = 0;
        int rowWidth = 0;
        int maxColumnCount = 0;
        for (auto* widget : m_attachmentTiles)
        {
            auto* tile = static_cast<AttachmentTile*>(widget);
            const int tileWidth = std::min(tile->targetWidth(), availableWidth);
            const int nextWidth = column == 0 ? tileWidth : rowWidth + tileSpacing + tileWidth;
            if (column > 0 && nextWidth > availableWidth)
            {
                maxColumnCount = std::max(maxColumnCount, column);
                ++row;
                column = 0;
                rowWidth = 0;
            }
            tile->setFixedWidth(tileWidth);
            m_listLayout->addWidget(tile, row, column, Qt::AlignLeft);
            rowWidth = column == 0 ? tileWidth : rowWidth + tileSpacing + tileWidth;
            ++column;
        }
        maxColumnCount = std::max(maxColumnCount, column);
        m_listLayout->setColumnStretch(maxColumnCount, 1);
    }

    void MessageAttachmentPanel::resizeEvent(QResizeEvent* event)
    {
        QWidget::resizeEvent(event);
        if (hasVisibleAttachments() && event->size().width() != event->oldSize().width())
            reflowRows();
    }
} // namespace javelin::gui::messageview
