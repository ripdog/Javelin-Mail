#include "gui/messageview/MessageViewContainer.h"
#include "gui/IconUtils.h"
#include "gui/messageview/GoogleHtmlTranslator.h"
#include "gui/messageview/HtmlMessageView.h"
#include "gui/messageview/MessageViewPresentation.h"
#include "gui/messageview/PlainTextLinkifier.h"
#include "gui/settings/PreferencesDialog.h"
#include "jmap/cache/TranslationCacheRepository.h"
#include "jmap/contacts/ContactIdentityLookup.h"
#include "jmap/language/LanguageDetection.h"

#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFrame>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMimeDatabase>
#include <QMouseEvent>
#include <QProgressBar>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStringList>
#include <QStyle>
#include <QTextBrowser>
#include <QTextDocument>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <cctype>
#include <variant>
#include <vector>

namespace javelin::gui::messageview
{
    namespace
    {
        constexpr auto remoteContentGroup = "remoteContent";
        constexpr auto allowedSendersKey = "allowedSenders";
        constexpr auto allowedDomainsKey = "allowedDomains";
        constexpr auto translationGroup = "translation";
        constexpr auto autoTranslateSendersKey = "autoTranslateSenders";
        constexpr auto autoTranslateDomainsKey = "autoTranslateDomains";
        constexpr auto targetLanguage = "en";
        constexpr auto automaticSourceLanguage = "auto";

        [[nodiscard]] std::string defaultLanguageModelPath()
        {
#ifdef JAVELIN_FASTTEXT_LANGUAGE_MODEL_PATH
            return JAVELIN_FASTTEXT_LANGUAGE_MODEL_PATH;
#else
            return {};
#endif
        }

        [[nodiscard]] std::string
        detectionText(const javelin::jmap::cache::MessageViewSnapshot& snapshot)
        {
            if (snapshot.plainTextBody.has_value() && !snapshot.plainTextBody->isTruncated)
            {
                return snapshot.plainTextBody->value;
            }

            if (snapshot.htmlBody.has_value() && !snapshot.htmlBody->isTruncated)
            {
                QTextDocument document;
                document.setHtml(QString::fromStdString(snapshot.htmlBody->value));
                return document.toPlainText().toStdString();
            }

            return {};
        }

        [[nodiscard]] std::optional<javelin::jmap::language::LanguageDetectionResult>
        detectMessageLanguage(javelin::jmap::cache::MessageViewSnapshot snapshot)
        {
            const auto text = detectionText(snapshot);
            if (text.empty())
            {
                return std::nullopt;
            }

            javelin::jmap::language::LanguageDetectionService service{defaultLanguageModelPath()};
            return service.detect(text);
        }

        [[nodiscard]] QString
        attachmentName(const javelin::jmap::cache::MessageAttachment& attachment)
        {
            return QString::fromStdString(attachment.name.value_or(attachment.partId));
        }

        [[nodiscard]] std::optional<std::string>
        renderedBodyKey(const std::optional<javelin::jmap::cache::MessageViewSnapshot>& snapshot)
        {
            if (!snapshot.has_value())
            {
                return std::nullopt;
            }

            if (snapshot->htmlBody.has_value())
            {
                if (snapshot->htmlRenderDocument.has_value())
                {
                    return snapshot->htmlRenderDocument->html;
                }

                return snapshot->htmlBody->value;
            }

            if (snapshot->plainTextBody.has_value())
            {
                return snapshot->plainTextBody->value;
            }

            return std::nullopt;
        }

        [[nodiscard]] QString addressLabel(const javelin::jmap::domain::EmailAddress& address)
        {
            const auto email = QString::fromStdString(address.email);
            if (!address.name.has_value() || address.name->empty())
            {
                return email;
            }

            const auto name = QString::fromStdString(*address.name);
            return QStringLiteral("%1 <%2>").arg(name, email);
        }

        [[nodiscard]] QString
        addressListLabel(const std::vector<javelin::jmap::domain::EmailAddress>& addresses)
        {
            if (addresses.empty())
            {
                return QStringLiteral("(none)");
            }

            QStringList labels;
            labels.reserve(static_cast<qsizetype>(addresses.size()));
            for (const auto& address : addresses)
            {
                labels.push_back(addressLabel(address));
            }
            return labels.join(QStringLiteral(", "));
        }

        [[nodiscard]] QString formatReceivedDateTime(const std::string& receivedAt)
        {
            const auto rawValue = QString::fromStdString(receivedAt);
            auto dateTime = QDateTime::fromString(rawValue, Qt::ISODateWithMs);
            if (!dateTime.isValid())
            {
                dateTime = QDateTime::fromString(rawValue, Qt::ISODate);
            }
            if (!dateTime.isValid())
            {
                return rawValue;
            }

            return QLocale{}.toString(dateTime.toLocalTime(), QLocale::ShortFormat);
        }

        [[nodiscard]] QString languageName(const std::string& languageCode)
        {
            const auto locale = QLocale{QString::fromStdString(languageCode)};
            const auto name = QLocale::languageToString(locale.language());
            return name == QStringLiteral("C") ? QString::fromStdString(languageCode) : name;
        }

        void makeLabelSelectable(QLabel* label)
        {
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
            label->setCursor(Qt::IBeamCursor);
            label->setFocusPolicy(Qt::NoFocus);
            label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        }

        void makeMetadataLabelSelectable(QLabel* label)
        {
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
            label->setCursor(Qt::IBeamCursor);
            label->setFocusPolicy(Qt::NoFocus);
        }

        [[nodiscard]] QStringList remoteContentAllowList(const QLatin1StringView key)
        {
            QSettings settings;
            settings.beginGroup(QLatin1StringView{remoteContentGroup});
            auto values = settings.value(key).toStringList();
            settings.endGroup();
            values.removeAll(QString{});
            values.removeDuplicates();
            values.sort(Qt::CaseInsensitive);
            return values;
        }

        void saveRemoteContentAllowList(const QLatin1StringView key, QStringList values)
        {
            values.removeAll(QString{});
            values.removeDuplicates();
            values.sort(Qt::CaseInsensitive);

            QSettings settings;
            settings.beginGroup(QLatin1StringView{remoteContentGroup});
            settings.setValue(key, values);
            settings.endGroup();
            settings.sync();
        }

        void addRemoteContentAllowListValue(const QLatin1StringView key, QString value)
        {
            value = value.trimmed().toLower();
            if (value.isEmpty())
            {
                return;
            }

            auto values = remoteContentAllowList(key);
            if (!values.contains(value, Qt::CaseInsensitive))
            {
                values.push_back(value);
                saveRemoteContentAllowList(key, values);
            }
        }

        [[nodiscard]] QStringList settingsList(const QLatin1StringView group,
                                               const QLatin1StringView key)
        {
            QSettings settings;
            settings.beginGroup(group);
            auto values = settings.value(key).toStringList();
            settings.endGroup();
            values.removeAll(QString{});
            values.removeDuplicates();
            values.sort(Qt::CaseInsensitive);
            return values;
        }

        void saveSettingsList(const QLatin1StringView group, const QLatin1StringView key,
                              QStringList values)
        {
            values.removeAll(QString{});
            values.removeDuplicates();
            values.sort(Qt::CaseInsensitive);

            QSettings settings;
            settings.beginGroup(group);
            settings.setValue(key, values);
            settings.endGroup();
            settings.sync();
        }

        void setSettingsListValue(const QLatin1StringView group, const QLatin1StringView key,
                                  QString value, const bool enabled)
        {
            value = value.trimmed().toLower();
            if (value.isEmpty())
            {
                return;
            }

            auto values = settingsList(group, key);
            values.removeAll(value);
            if (enabled)
            {
                values.push_back(value);
            }
            saveSettingsList(group, key, values);
        }

        [[nodiscard]] bool settingsListContains(const QLatin1StringView group,
                                                const QLatin1StringView key, const QString& value)
        {
            return !value.isEmpty() &&
                   settingsList(group, key).contains(value, Qt::CaseInsensitive);
        }

        [[nodiscard]] QIcon
        attachmentIcon(const javelin::jmap::cache::MessageAttachment& attachment)
        {
            const auto fileName = attachmentName(attachment);
            const QFileInfo fileInfo(fileName);
            QFileIconProvider iconProvider;
            auto icon = iconProvider.icon(fileInfo);
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

        class AttachmentTile : public QFrame
        {
          public:
            AttachmentTile(const javelin::jmap::cache::MessageAttachment& attachment,
                           std::function<void()> openAction, std::function<void()> saveAction,
                           QString saveToolTip, QWidget* parent = nullptr)
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

                auto* openButton = new QToolButton(this);
                openButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
                openButton->setIcon(attachmentIcon(attachment));
                openButton->setText(fileName);
                openButton->setToolTip(
                    QStringLiteral("Open %1 in default application").arg(fileName));
                openButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
                connect(openButton, &QToolButton::clicked, this,
                        [action = std::move(openAction)]
                        {
                            if (action)
                            {
                                action();
                            }
                        });

                auto* saveButton = new QToolButton(this);
                saveButton->setObjectName(QStringLiteral("saveAttachmentButton"));
                saveButton->setIcon(QIcon::fromTheme(QStringLiteral("edit-download")));
                saveButton->setToolTip(std::move(saveToolTip));
                saveButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
                connect(saveButton, &QToolButton::clicked, this,
                        [action = std::move(saveAction)]
                        {
                            if (action)
                            {
                                action();
                            }
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

        class MessagePreviewTile : public QFrame
        {
          public:
            MessagePreviewTile(const javelin::jmap::cache::MessageListItem& message,
                               std::function<void()> activateAction, QWidget* parent = nullptr)
                : QFrame(parent), m_activateAction(std::move(activateAction))
            {
                setCursor(Qt::PointingHandCursor);
                setFrameStyle(QFrame::NoFrame);
                setObjectName(QStringLiteral("messagePreviewTile"));
                setStyleSheet(QStringLiteral(
                    "#messagePreviewTile { background: transparent; border: none; }"
                    "#messagePreviewTile:hover { background: rgba(255, 255, 255, 0.06); }"));

                auto* layout = new QVBoxLayout(this);
                layout->setContentsMargins(8, 8, 8, 8);
                layout->setSpacing(4);

                const auto subject = message.subject.has_value()
                                         ? QString::fromStdString(*message.subject)
                                         : QStringLiteral("(no subject)");
                auto* subjectLabel = new QLabel(subject, this);
                subjectLabel->setWordWrap(true);
                auto subjectFont = subjectLabel->font();
                subjectFont.setBold(true);
                subjectFont.setPointSize(subjectFont.pointSize() + 1);
                subjectLabel->setFont(subjectFont);

                const auto preview = message.preview.has_value()
                                         ? QString::fromStdString(*message.preview)
                                         : QStringLiteral("(no preview available)");
                auto* previewLabel = new QLabel(preview, this);
                previewLabel->setWordWrap(true);
                previewLabel->setMaximumHeight(previewLabel->fontMetrics().lineSpacing() * 2 + 6);
                previewLabel->setStyleSheet(QStringLiteral("color: #c5cad3;"));

                layout->addWidget(subjectLabel);
                layout->addWidget(previewLabel);
            }

          protected:
            void mouseReleaseEvent(QMouseEvent* event) override
            {
                if (event->button() == Qt::LeftButton &&
                    rect().contains(event->position().toPoint()))
                {
                    if (m_activateAction)
                    {
                        m_activateAction();
                    }
                    event->accept();
                    return;
                }

                QFrame::mouseReleaseEvent(event);
            }

          private:
            std::function<void()> m_activateAction;
        };

        [[nodiscard]] std::vector<const javelin::jmap::cache::MessageAttachment*>
        visibleAttachments(const std::optional<javelin::jmap::cache::MessageViewSnapshot>& snapshot)
        {
            std::vector<const javelin::jmap::cache::MessageAttachment*> attachments;
            if (!snapshot.has_value())
            {
                return attachments;
            }

            attachments.reserve(snapshot->attachments.size());
            for (const auto& attachment : snapshot->attachments)
            {
                const bool isEmbeddedInline =
                    attachment.cid.has_value() && attachment.disposition.has_value() &&
                    std::ranges::equal(*attachment.disposition, std::string_view{"inline"},
                                       [](const char left, const char right)
                                       {
                                           return std::tolower(static_cast<unsigned char>(left)) ==
                                                  std::tolower(static_cast<unsigned char>(right));
                                       });
                if (!isEmbeddedInline)
                {
                    attachments.push_back(&attachment);
                }
            }

            return attachments;
        }

    } // namespace

    MessageViewContainer::MessageViewContainer(
        javelin::jmap::cache::TranslationCacheRepository& translationCacheRepository,
        javelin::jmap::contacts::ContactIdentityLookup& contactIdentityLookup, QWidget* parent)
        : QWidget(parent), m_translationCacheRepository(translationCacheRepository),
          m_contactIdentityLookup(contactIdentityLookup)
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(8, 8, 8, 0);
        layout->setSpacing(8);

        auto* headerWidget = new QWidget(this);
        auto* headerLayout = new QVBoxLayout(headerWidget);
        headerLayout->setContentsMargins(0, 0, 0, 0);
        headerLayout->setSpacing(4);

        m_titleLabel = new QLabel(this);
        m_titleLabel->setObjectName(QStringLiteral("messageViewTitle"));
        m_titleLabel->setWordWrap(true);
        makeLabelSelectable(m_titleLabel);

        auto titleFont = m_titleLabel->font();
        titleFont.setPointSize(titleFont.pointSize() + 4);
        titleFont.setBold(true);
        m_titleLabel->setFont(titleFont);

        m_detailLabel = new QLabel(this);
        m_detailLabel->setWordWrap(true);
        makeLabelSelectable(m_detailLabel);

        m_metadataWidget = new QWidget(headerWidget);
        auto* metadataLayout = new QGridLayout(m_metadataWidget);
        metadataLayout->setContentsMargins(0, 0, 0, 0);
        metadataLayout->setHorizontalSpacing(14);
        metadataLayout->setVerticalSpacing(2);

        m_fromLabel = new QLabel(m_metadataWidget);
        m_fromLabel->setWordWrap(false);
        m_fromLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        makeMetadataLabelSelectable(m_fromLabel);

        m_toLabel = new QLabel(m_metadataWidget);
        m_toLabel->setWordWrap(true);
        m_toLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        makeMetadataLabelSelectable(m_toLabel);

        m_receivedLabel = new QLabel(m_metadataWidget);
        m_receivedLabel->setAlignment(Qt::AlignRight | Qt::AlignTop);
        m_receivedLabel->setWordWrap(false);
        m_receivedLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        makeMetadataLabelSelectable(m_receivedLabel);

        metadataLayout->addWidget(m_fromLabel, 0, 0);
        metadataLayout->addWidget(m_receivedLabel, 0, 1);
        metadataLayout->addWidget(m_toLabel, 1, 0, 1, 2);
        metadataLayout->setColumnStretch(0, 1);
        metadataLayout->setColumnStretch(1, 0);
        m_metadataWidget->setVisible(false);

        headerLayout->addWidget(m_titleLabel);
        headerLayout->addWidget(m_detailLabel);
        headerLayout->addWidget(m_metadataWidget);

        m_bodyControlsWidget = new QWidget(this);
        auto* buttonLayout = new QHBoxLayout(m_bodyControlsWidget);
        buttonLayout->setContentsMargins(0, 0, 0, 0);
        buttonLayout->setSpacing(6);

        m_remoteContentIconLabel = new QLabel(m_bodyControlsWidget);
        m_remoteContentIconLabel->setPixmap(javelin::gui::themedSvgPixmap(
            QStringLiteral(":/icons/thunderbird-icons/remote-blocked.svg"),
            palette().color(QPalette::HighlightedText), 18));
        m_remoteContentIconLabel->setFixedSize(20, 20);
        m_remoteContentIconLabel->setAlignment(Qt::AlignCenter);

        m_remoteContentStatusLabel = new QLabel(m_bodyControlsWidget);
        m_remoteContentStatusLabel->setWordWrap(true);
        makeLabelSelectable(m_remoteContentStatusLabel);

        m_permitSenderRemoteContentButton = new QToolButton(m_bodyControlsWidget);
        m_permitSenderRemoteContentButton->setText(QStringLiteral("Always load sender"));
        connect(m_permitSenderRemoteContentButton, &QToolButton::clicked, this,
                &MessageViewContainer::permitRemoteContentForCurrentSender);

        m_permitDomainRemoteContentButton = new QToolButton(m_bodyControlsWidget);
        m_permitDomainRemoteContentButton->setText(QStringLiteral("Always load domain"));
        connect(m_permitDomainRemoteContentButton, &QToolButton::clicked, this,
                &MessageViewContainer::permitRemoteContentForCurrentDomain);

        m_remoteContentButton = new QToolButton(m_bodyControlsWidget);
        m_remoteContentButton->setText(QStringLiteral("Load remote content"));
        m_remoteContentButton->setCheckable(true);
        connect(m_remoteContentButton, &QToolButton::clicked, this,
                [this](const bool checked)
                {
                    m_htmlView->setRemoteContentEnabled(checked);
                    updateRemoteContentButton();
                });

        buttonLayout->addWidget(m_remoteContentIconLabel);
        buttonLayout->addWidget(m_remoteContentStatusLabel, 1);
        buttonLayout->addWidget(m_permitSenderRemoteContentButton);
        buttonLayout->addWidget(m_permitDomainRemoteContentButton);
        buttonLayout->addWidget(m_remoteContentButton);

        m_languageBannerWidget = new QWidget(this);
        auto* languageLayout = new QHBoxLayout(m_languageBannerWidget);
        languageLayout->setContentsMargins(8, 6, 8, 6);
        languageLayout->setSpacing(8);
        m_languageBannerWidget->setStyleSheet(QStringLiteral(
            "QWidget { background: #243044; border: 1px solid #3f5373; border-radius: 6px; }"
            "QLabel { border: 0; background: transparent; }"
            "QToolButton { border: 1px solid #60789f; border-radius: 4px; padding: 4px 8px; }"));

        m_languageStatusLabel = new QLabel(m_languageBannerWidget);
        m_languageStatusLabel->setWordWrap(true);
        makeLabelSelectable(m_languageStatusLabel);

        m_translateButton = new QToolButton(m_languageBannerWidget);
        m_translateButton->setText(QStringLiteral("Translate"));
        connect(m_translateButton, &QToolButton::clicked, this,
                &MessageViewContainer::translateCurrentMessage);

        m_translateOptionsButton = new QToolButton(m_languageBannerWidget);
        m_translateOptionsButton->setText(QStringLiteral("Options"));
        m_translateOptionsButton->setPopupMode(QToolButton::InstantPopup);
        m_translateOptionsButton->setToolTip(QStringLiteral("Translation options"));
        auto* translateMenu = new QMenu(m_translateOptionsButton);
        auto* autoSenderAction = translateMenu->addAction(QStringLiteral("Auto-translate sender"));
        autoSenderAction->setCheckable(true);
        autoSenderAction->setData(QStringLiteral("sender"));
        auto* autoDomainAction =
            translateMenu->addAction(QStringLiteral("Auto-translate sender domain"));
        autoDomainAction->setCheckable(true);
        autoDomainAction->setData(QStringLiteral("domain"));
        connect(autoSenderAction, &QAction::toggled, this,
                &MessageViewContainer::setAutoTranslateSender);
        connect(autoDomainAction, &QAction::toggled, this,
                &MessageViewContainer::setAutoTranslateDomain);
        m_translateOptionsButton->setMenu(translateMenu);

        languageLayout->addWidget(m_languageStatusLabel, 1);
        languageLayout->addWidget(m_translateButton);
        languageLayout->addWidget(m_translateOptionsButton);
        m_languageBannerWidget->setVisible(false);
        m_translator = new GoogleHtmlTranslator(this);

        m_bodyStack = new QStackedWidget(this);
        m_bodyStack->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

        m_placeholderPanel = new QWidget(this);
        auto* placeholderOuterLayout = new QVBoxLayout(m_placeholderPanel);
        placeholderOuterLayout->setContentsMargins(0, 12, 0, 12);
        placeholderOuterLayout->addStretch(1);

        auto* placeholderCard = new QWidget(m_placeholderPanel);
        placeholderCard->setObjectName(QStringLiteral("messagePlaceholderCard"));
        placeholderCard->setMinimumWidth(280);
        placeholderCard->setMaximumWidth(520);
        placeholderCard->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
        placeholderCard->setStyleSheet(QStringLiteral("#messagePlaceholderCard {"
                                                      " background: #1f2126;"
                                                      " border: 1px solid #393d46;"
                                                      " border-radius: 16px;"
                                                      "}"
                                                      "#messagePlaceholderCard QLabel {"
                                                      " color: #e6e9ef;"
                                                      "}"));

        auto* placeholderCardLayout = new QVBoxLayout(placeholderCard);
        placeholderCardLayout->setContentsMargins(24, 22, 24, 22);
        placeholderCardLayout->setSpacing(10);

        m_placeholderTitleLabel = new QLabel(placeholderCard);
        auto placeholderTitleFont = m_placeholderTitleLabel->font();
        placeholderTitleFont.setPointSize(placeholderTitleFont.pointSize() + 2);
        placeholderTitleFont.setBold(true);
        m_placeholderTitleLabel->setFont(placeholderTitleFont);
        m_placeholderTitleLabel->setWordWrap(true);
        makeLabelSelectable(m_placeholderTitleLabel);

        m_placeholderDetailLabel = new QLabel(placeholderCard);
        m_placeholderDetailLabel->setWordWrap(true);
        m_placeholderDetailLabel->setStyleSheet(QStringLiteral("color: #c5cad3;"));
        makeLabelSelectable(m_placeholderDetailLabel);

        m_loadingIndicator = new QProgressBar(placeholderCard);
        m_loadingIndicator->setRange(0, 0);
        m_loadingIndicator->setTextVisible(false);
        m_loadingIndicator->setFixedHeight(8);
        m_loadingIndicator->setVisible(false);
        m_loadingIndicator->setStyleSheet(QStringLiteral("QProgressBar {"
                                                         " background: #2a2d34;"
                                                         " border: 1px solid #393d46;"
                                                         " border-radius: 4px;"
                                                         "}"
                                                         "QProgressBar::chunk {"
                                                         " background: #7fb0ff;"
                                                         " border-radius: 4px;"
                                                         "}"));

        placeholderCardLayout->addWidget(m_placeholderTitleLabel);
        placeholderCardLayout->addWidget(m_placeholderDetailLabel);
        placeholderCardLayout->addWidget(m_loadingIndicator);

        placeholderOuterLayout->addWidget(placeholderCard, 0, Qt::AlignHCenter);
        placeholderOuterLayout->addStretch(1);

        m_plainTextView = new QTextBrowser(this);
        m_plainTextView->setReadOnly(true);
        m_plainTextView->setOpenLinks(false);
        m_plainTextView->setOpenExternalLinks(false);
        m_plainTextView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        connect(m_plainTextView, &QTextBrowser::anchorClicked, this,
                [](const QUrl& url) { QDesktopServices::openUrl(url); });
        connect(m_plainTextView, &QTextBrowser::highlighted, this,
                [this](const QUrl& url) { Q_EMIT hoveredLinkChanged(url.toString()); });

        m_multipleSelectionScrollArea = new QScrollArea(this);
        m_multipleSelectionScrollArea->setWidgetResizable(true);
        m_multipleSelectionScrollArea->setFrameShape(QFrame::NoFrame);
        m_multipleSelectionScrollArea->setSizePolicy(QSizePolicy::Expanding,
                                                     QSizePolicy::Expanding);
        m_multipleSelectionWidget = new QWidget(m_multipleSelectionScrollArea);
        m_multipleSelectionLayout = new QVBoxLayout(m_multipleSelectionWidget);
        m_multipleSelectionLayout->setContentsMargins(0, 0, 0, 0);
        m_multipleSelectionLayout->setSpacing(0);
        m_multipleSelectionScrollArea->setWidget(m_multipleSelectionWidget);

        m_htmlView = new HtmlMessageView(this);
        m_htmlView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        connect(m_htmlView, &HtmlMessageView::viewSourceRequested, this,
                &MessageViewContainer::viewSourceRequested);
        connect(m_htmlView, &HtmlMessageView::hoveredLinkChanged, this,
                &MessageViewContainer::hoveredLinkChanged);
        connect(m_htmlView, &HtmlMessageView::documentLoaded, this,
                [this](const QString& documentId)
                {
                    if (!m_accountId.has_value() || !m_emailId.has_value() ||
                        documentId != QString::fromStdString(*m_accountId + "\n" + *m_emailId))
                    {
                        return;
                    }
                    m_htmlDocumentLoaded = true;
                    m_loading = false;
                    updatePresentation(false);
                    if (m_activeView == ActiveView::Html)
                    {
                        maybeAutoTranslateCurrentMessage();
                    }
                });

        m_bodyStack->addWidget(m_placeholderPanel);
        m_bodyStack->addWidget(m_multipleSelectionScrollArea);
        m_bodyStack->addWidget(m_plainTextView);
        m_bodyStack->addWidget(m_htmlView);

        m_attachmentStatusLabel = new QLabel(this);
        m_attachmentStatusLabel->setWordWrap(false);
        m_attachmentStatusLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
        makeLabelSelectable(m_attachmentStatusLabel);
        m_attachmentStatusLabel->setVisible(false);

        m_attachmentHeaderWidget = new QWidget(this);
        auto* attachmentHeaderLayout = new QHBoxLayout(m_attachmentHeaderWidget);
        attachmentHeaderLayout->setContentsMargins(0, 0, 0, 0);
        attachmentHeaderLayout->setSpacing(4);

        m_attachmentExpanderButton = new QToolButton(m_attachmentHeaderWidget);
        m_attachmentExpanderButton->setAutoRaise(true);
        m_attachmentExpanderButton->setArrowType(Qt::RightArrow);
        connect(m_attachmentExpanderButton, &QToolButton::clicked, this,
                [this]
                {
                    m_attachmentsExpanded = !m_attachmentsExpanded;
                    updateAttachmentSection();
                });

        m_attachmentStatusLabel->setParent(m_attachmentHeaderWidget);

        m_saveAllAttachmentsButton = new QToolButton(m_attachmentHeaderWidget);
        m_saveAllAttachmentsButton->setText(QStringLiteral("Save All"));
        connect(m_saveAllAttachmentsButton, &QToolButton::clicked, this,
                [this]
                {
                    if (m_accountId.has_value() && m_emailId.has_value())
                    {
                        Q_EMIT saveAllAttachmentsRequested(QString::fromStdString(*m_accountId),
                                                           QString::fromStdString(*m_emailId));
                    }
                });

        m_attachmentListWidget = new QWidget(m_attachmentHeaderWidget);
        m_attachmentListLayout = new QGridLayout(m_attachmentListWidget);
        m_attachmentListLayout->setContentsMargins(0, 0, 0, 0);
        m_attachmentListLayout->setHorizontalSpacing(6);
        m_attachmentListLayout->setVerticalSpacing(6);
        m_attachmentListLayout->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        m_attachmentListWidget->setVisible(false);

        attachmentHeaderLayout->addWidget(m_attachmentStatusLabel);
        attachmentHeaderLayout->addWidget(m_attachmentListWidget, 1);
        attachmentHeaderLayout->addWidget(m_saveAllAttachmentsButton);
        m_attachmentHeaderWidget->setVisible(false);

        layout->addWidget(headerWidget);
        layout->addWidget(m_bodyControlsWidget);
        layout->addWidget(m_languageBannerWidget);
        layout->addWidget(m_bodyStack, 1);
        layout->addWidget(m_attachmentHeaderWidget);

        connect(&m_contactIdentityLookup,
                &javelin::jmap::contacts::ContactIdentityLookup::contactDataChanged, this,
                [this]
                {
                    if (m_snapshot.has_value())
                        m_fromLabel->setText(
                            QStringLiteral("From: %1").arg(contactAwareSenderLabel()));
                });
        updatePresentation();
    }

    MessageViewContainer::~MessageViewContainer() = default;

    void
    MessageViewContainer::setSelection(javelin::jmap::cache::MessageViewService& messageViewService,
                                       std::optional<std::string> accountId,
                                       std::optional<std::string> mailboxId,
                                       std::optional<std::string> emailId)
    {
        if (m_accountId == accountId && m_mailboxId == mailboxId && m_emailId == emailId)
        {
            return;
        }

        m_accountId = std::move(accountId);
        m_mailboxId = std::move(mailboxId);
        m_emailId = std::move(emailId);
        m_multipleMessages.clear();
        m_attachmentsExpanded = false;
        m_translationInProgress = false;
        m_messageTranslated = false;
        m_originalPlainText.clear();
        m_translationError.clear();
        m_autoTranslateAttempted = false;
        m_translationWasAutomatic = false;
        m_languageDetectionStarted = false;
        m_htmlDocumentLoaded = false;
        m_loading = m_emailId.has_value();
        m_errorMessage.clear();

        m_snapshot = std::nullopt;
        if (m_accountId.has_value() && m_emailId.has_value())
        {
            const auto result = messageViewService.load(*m_accountId, *m_emailId);
            if (const auto* snapshot =
                    std::get_if<std::optional<javelin::jmap::cache::MessageViewSnapshot>>(&result))
            {
                m_snapshot = *snapshot;
            }
        }

        updatePresentation();
    }

    void MessageViewContainer::setMultipleSelection(
        std::optional<std::string> accountId, std::optional<std::string> mailboxId,
        std::vector<javelin::jmap::cache::MessageListItem> messages)
    {
        m_accountId = std::move(accountId);
        m_mailboxId = std::move(mailboxId);
        m_emailId = std::nullopt;
        m_multipleMessages = std::move(messages);
        m_attachmentsExpanded = false;
        m_translationInProgress = false;
        m_messageTranslated = false;
        m_originalPlainText.clear();
        m_translationError.clear();
        m_autoTranslateAttempted = false;
        m_translationWasAutomatic = false;
        m_languageDetectionStarted = false;
        m_htmlDocumentLoaded = false;
        m_loading = false;
        m_errorMessage.clear();
        m_snapshot = std::nullopt;
        updatePresentation();
    }

    void MessageViewContainer::refresh(javelin::jmap::cache::MessageViewService& messageViewService)
    {
        const auto previousRenderedBody = renderedBodyKey(m_snapshot);
        m_errorMessage.clear();
        m_translationInProgress = false;
        m_messageTranslated = false;
        m_originalPlainText.clear();
        m_translationError.clear();
        m_autoTranslateAttempted = false;
        m_translationWasAutomatic = false;
        m_languageDetectionStarted = false;
        m_snapshot = std::nullopt;
        if (m_accountId.has_value() && m_emailId.has_value())
        {
            const auto result = messageViewService.load(*m_accountId, *m_emailId);
            if (const auto* snapshot =
                    std::get_if<std::optional<javelin::jmap::cache::MessageViewSnapshot>>(&result))
            {
                m_snapshot = *snapshot;
            }
        }

        updatePresentation(previousRenderedBody != renderedBodyKey(m_snapshot));
    }

    void MessageViewContainer::setActiveView(const ActiveView view)
    {
        m_activeView = view;
        updateSenderRemoteContentPermit();
        updateRemoteContentButton();
        updateLanguageBanner();

        switch (m_activeView)
        {
        case ActiveView::Placeholder:
            m_bodyStack->setCurrentWidget(m_placeholderPanel);
            break;
        case ActiveView::Multiple:
            m_bodyStack->setCurrentWidget(m_multipleSelectionScrollArea);
            break;
        case ActiveView::PlainText:
            m_bodyStack->setCurrentWidget(m_plainTextView);
            break;
        case ActiveView::Html:
            m_bodyStack->setCurrentWidget(m_htmlView);
            break;
        }
    }

    void MessageViewContainer::setLoadingState(const bool loading, const QString& detailText)
    {
        m_loading = loading;
        if (loading)
        {
            m_errorMessage.clear();
        }
        if (!detailText.isEmpty())
        {
            m_placeholderDetailLabel->setText(detailText);
        }
        updatePresentation();
    }

    void MessageViewContainer::setErrorState(const QString& errorMessage)
    {
        m_loading = false;
        m_errorMessage = errorMessage;
        updatePresentation();
    }

    bool MessageViewContainer::hasContentSnapshot() const
    {
        return m_snapshot.has_value();
    }

    bool MessageViewContainer::hasReadableBody() const
    {
        return m_snapshot.has_value() &&
               (m_snapshot->htmlBody.has_value() || m_snapshot->plainTextBody.has_value());
    }

    void MessageViewContainer::updateSenderRemoteContentPermit()
    {
        const auto sender = currentSenderAddress();
        const auto domain = currentSenderDomain();
        const bool permittedSender =
            !sender.isEmpty() && remoteContentAllowList(QLatin1StringView{allowedSendersKey})
                                     .contains(sender, Qt::CaseInsensitive);
        const bool permittedDomain =
            !domain.isEmpty() && remoteContentAllowList(QLatin1StringView{allowedDomainsKey})
                                     .contains(domain, Qt::CaseInsensitive);
        const bool shouldAllow = permittedSender || permittedDomain;
        if (m_htmlView->remoteContentEnabled() != shouldAllow)
        {
            m_htmlView->setRemoteContentEnabled(shouldAllow);
        }
    }

    void MessageViewContainer::updateRemoteContentButton()
    {
        const bool hasBlockedRemoteContent =
            m_snapshot.has_value() && m_snapshot->htmlRenderDocument.has_value() &&
            m_snapshot->htmlRenderDocument->blockedRemoteResourceCount > 0;
        const bool remoteContentAllowed =
            hasBlockedRemoteContent && m_htmlView->remoteContentEnabled();
        const bool showRemoteContentControls = hasBlockedRemoteContent && !remoteContentAllowed;
        m_bodyControlsWidget->setVisible(showRemoteContentControls);
        m_remoteContentIconLabel->setVisible(showRemoteContentControls);
        m_remoteContentStatusLabel->setVisible(showRemoteContentControls);
        m_permitSenderRemoteContentButton->setVisible(showRemoteContentControls);
        m_permitDomainRemoteContentButton->setVisible(showRemoteContentControls);
        m_permitSenderRemoteContentButton->setEnabled(hasBlockedRemoteContent &&
                                                      !currentSenderAddress().isEmpty());
        m_permitDomainRemoteContentButton->setEnabled(hasBlockedRemoteContent &&
                                                      !currentSenderDomain().isEmpty());
        m_remoteContentButton->setVisible(showRemoteContentControls);
        m_remoteContentButton->setEnabled(hasBlockedRemoteContent);
        m_remoteContentButton->setChecked(hasBlockedRemoteContent &&
                                          m_htmlView->remoteContentEnabled());
        if (hasBlockedRemoteContent)
        {
            m_remoteContentStatusLabel->setText(
                QStringLiteral("Blocked remote resources: %1")
                    .arg(static_cast<qulonglong>(
                        m_snapshot->htmlRenderDocument->blockedRemoteResourceCount)));
        }
        else
        {
            m_remoteContentStatusLabel->clear();
        }
        m_permitSenderRemoteContentButton->setToolTip(
            currentSenderAddress().isEmpty()
                ? QStringLiteral("No sender address is available")
                : QStringLiteral("Always load remote content from this sender"));
        m_permitDomainRemoteContentButton->setToolTip(
            currentSenderDomain().isEmpty()
                ? QStringLiteral("No sender domain is available")
                : QStringLiteral("Always load remote content from this sender domain"));
        m_remoteContentButton->setText(m_htmlView->remoteContentEnabled()
                                           ? QStringLiteral("Hide remote content")
                                           : QStringLiteral("Load remote content"));
    }

    void MessageViewContainer::updateLanguageBanner()
    {
        const bool canTranslateView =
            m_activeView == ActiveView::PlainText || m_activeView == ActiveView::Html;
        const bool hasLanguageOffer = m_snapshot.has_value() &&
                                      m_snapshot->languageDetection.has_value() &&
                                      m_snapshot->shouldOfferTranslation;
        const bool shouldShow =
            canTranslateView && (hasLanguageOffer || m_translationInProgress ||
                                 m_messageTranslated || !m_translationError.isEmpty());
        m_languageBannerWidget->setVisible(shouldShow);
        m_translateOptionsButton->setVisible(shouldShow);
        updateTranslateOptionsMenu();
        if (!shouldShow)
        {
            m_languageStatusLabel->clear();
            return;
        }

        m_translateButton->setEnabled(!m_translationInProgress);
        m_translateButton->setText(m_messageTranslated ? QStringLiteral("Show original")
                                                       : QStringLiteral("Translate"));
        m_translateButton->setToolTip(m_messageTranslated
                                          ? QStringLiteral("Restore the original message text")
                                          : QStringLiteral("Translate this message to English"));

        if (m_translationInProgress)
        {
            m_languageStatusLabel->setText(QStringLiteral("Translating message..."));
            return;
        }

        if (!m_translationError.isEmpty())
        {
            m_languageStatusLabel->setText(m_translationError);
            return;
        }

        if (m_messageTranslated)
        {
            m_languageStatusLabel->setText(m_translationWasAutomatic
                                               ? QStringLiteral("Auto-translated to English.")
                                               : QStringLiteral("Message translated to English."));
            return;
        }

        const auto& detection = *m_snapshot->languageDetection;
        m_languageStatusLabel->setText(QStringLiteral("This message appears to be in %1.")
                                           .arg(languageName(detection.languageCode)));
    }

    void MessageViewContainer::updateTranslateOptionsMenu()
    {
        if (m_translateOptionsButton == nullptr || m_translateOptionsButton->menu() == nullptr)
        {
            return;
        }

        const auto sender = currentSenderAddress();
        const auto domain = currentSenderDomain();
        for (auto* action : m_translateOptionsButton->menu()->actions())
        {
            const auto kind = action->data().toString();
            QSignalBlocker blocker{action};
            if (kind == QStringLiteral("sender"))
            {
                action->setEnabled(!sender.isEmpty());
                action->setChecked(settingsListContains(QLatin1StringView{translationGroup},
                                                        QLatin1StringView{autoTranslateSendersKey},
                                                        sender));
            }
            else if (kind == QStringLiteral("domain"))
            {
                action->setEnabled(!domain.isEmpty());
                action->setChecked(settingsListContains(QLatin1StringView{translationGroup},
                                                        QLatin1StringView{autoTranslateDomainsKey},
                                                        domain));
            }
        }
    }

    void MessageViewContainer::setAutoTranslateSender(const bool enabled)
    {
        const auto sender = currentSenderAddress();
        setSettingsListValue(QLatin1StringView{translationGroup},
                             QLatin1StringView{autoTranslateSendersKey}, sender, enabled);
        if (enabled)
        {
            setSettingsListValue(QLatin1StringView{translationGroup},
                                 QLatin1StringView{autoTranslateDomainsKey}, currentSenderDomain(),
                                 false);
            m_autoTranslateAttempted = false;
        }
        updateTranslateOptionsMenu();
        maybeAutoTranslateCurrentMessage();
    }

    void MessageViewContainer::setAutoTranslateDomain(const bool enabled)
    {
        const auto domain = currentSenderDomain();
        setSettingsListValue(QLatin1StringView{translationGroup},
                             QLatin1StringView{autoTranslateDomainsKey}, domain, enabled);
        if (enabled)
        {
            setSettingsListValue(QLatin1StringView{translationGroup},
                                 QLatin1StringView{autoTranslateSendersKey}, currentSenderAddress(),
                                 false);
            m_autoTranslateAttempted = false;
        }
        updateTranslateOptionsMenu();
        maybeAutoTranslateCurrentMessage();
    }

    void MessageViewContainer::maybeAutoTranslateCurrentMessage()
    {
        if (m_autoTranslateAttempted || m_messageTranslated || m_translationInProgress ||
            !m_snapshot.has_value() || !m_snapshot->shouldOfferTranslation)
        {
            return;
        }
        if (m_activeView == ActiveView::Html && !m_htmlDocumentLoaded)
        {
            return;
        }

        const auto sender = currentSenderAddress();
        const auto domain = currentSenderDomain();
        const bool allowNetwork =
            settingsListContains(QLatin1StringView{translationGroup},
                                 QLatin1StringView{autoTranslateSendersKey}, sender) ||
            settingsListContains(QLatin1StringView{translationGroup},
                                 QLatin1StringView{autoTranslateDomainsKey}, domain);

        m_autoTranslateAttempted = true;
        translateCurrentMessageFromCacheOrNetwork(true, allowNetwork);
    }

    void MessageViewContainer::translateCurrentMessage()
    {
        if (m_messageTranslated)
        {
            restoreCurrentTranslation();
            return;
        }

        translateCurrentMessageFromCacheOrNetwork(false, true);
    }

    void MessageViewContainer::translateCurrentMessageFromCacheOrNetwork(const bool automatic,
                                                                         const bool allowNetwork)
    {
        if (!m_snapshot.has_value() || m_translationInProgress)
        {
            return;
        }

        const auto selectedEmailId = m_emailId;
        m_translationInProgress = true;
        m_translationError.clear();
        m_translationWasAutomatic = automatic;
        updateLanguageBanner();

        const auto applyTranslatedChunks =
            [this](const GoogleHtmlTranslator::TranslationChunks& chunks) -> bool
        {
            if (m_activeView == ActiveView::PlainText)
            {
                if (chunks.empty() || chunks.front().empty())
                {
                    return false;
                }
                m_originalPlainText = m_plainTextView->toPlainText();
                m_plainTextView->setHtml(linkifyPlainText(chunks.front().front()));
                return true;
            }

            if (m_activeView == ActiveView::Html)
            {
                m_htmlView->applyTranslationChunks(chunks);
                return true;
            }

            return false;
        };

        const auto cacheTranslatedChunks =
            [this](const GoogleHtmlTranslator::TranslationChunks& sourceChunks,
                   const GoogleHtmlTranslator::TranslationChunks& translatedChunks)
        {
            for (qsizetype i = 0; i < sourceChunks.size() && i < translatedChunks.size(); ++i)
            {
                const auto& sourceChunk = sourceChunks[i];
                const auto& translatedChunk = translatedChunks[i];
                for (qsizetype j = 0; j < sourceChunk.size() && j < translatedChunk.size(); ++j)
                {
                    (void)m_translationCacheRepository.upsert({
                        .sourceLanguage = QStringLiteral("auto"),
                        .targetLanguage = QStringLiteral("en"),
                        .inputText = sourceChunk[j],
                        .translatedText = translatedChunk[j],
                    });
                }
            }
        };

        const auto translateChunks =
            [this, selectedEmailId, allowNetwork, applyTranslatedChunks,
             cacheTranslatedChunks](GoogleHtmlTranslator::TranslationChunks chunks)
        {
            GoogleHtmlTranslator::TranslationChunks cachedChunks;
            cachedChunks.reserve(chunks.size());
            bool cacheComplete = true;
            for (const auto& chunk : chunks)
            {
                QStringList cachedChunk;
                cachedChunk.reserve(chunk.size());
                for (const auto& text : chunk)
                {
                    const auto cached = m_translationCacheRepository.find(
                        QStringLiteral("auto"), QStringLiteral("en"), text);
                    if (const auto* value = std::get_if<std::optional<QString>>(&cached);
                        value != nullptr && value->has_value())
                    {
                        cachedChunk.push_back(**value);
                    }
                    else
                    {
                        cacheComplete = false;
                        break;
                    }
                }
                if (!cacheComplete)
                {
                    break;
                }
                cachedChunks.push_back(std::move(cachedChunk));
            }

            if (cacheComplete)
            {
                m_translationInProgress = false;
                if (!applyTranslatedChunks(cachedChunks))
                {
                    m_translationError =
                        QStringLiteral("Translation failed: no translated text was returned.");
                    updateLanguageBanner();
                    return;
                }
                m_messageTranslated = true;
                updateLanguageBanner();
                return;
            }

            if (!allowNetwork)
            {
                m_translationInProgress = false;
                m_translationWasAutomatic = false;
                updateLanguageBanner();
                return;
            }

            const auto sourceChunks = chunks;
            m_translator->translate(
                std::move(chunks), QStringLiteral("auto"), QStringLiteral("en"),
                [this, selectedEmailId, sourceChunks, applyTranslatedChunks,
                 cacheTranslatedChunks](GoogleHtmlTranslator::Result result)
                {
                    if (m_emailId != selectedEmailId)
                    {
                        return;
                    }

                    m_translationInProgress = false;
                    if (const auto* error = std::get_if<QString>(&result))
                    {
                        m_translationError = QStringLiteral("Translation failed: %1").arg(*error);
                        updateLanguageBanner();
                        return;
                    }

                    const auto& translatedChunks =
                        std::get<GoogleHtmlTranslator::TranslationChunks>(result);
                    if (!applyTranslatedChunks(translatedChunks))
                    {
                        m_translationError =
                            QStringLiteral("Translation failed: no translated text was returned.");
                        updateLanguageBanner();
                        return;
                    }
                    cacheTranslatedChunks(sourceChunks, translatedChunks);

                    m_messageTranslated = true;
                    updateLanguageBanner();
                });
        };

        if (m_activeView == ActiveView::PlainText)
        {
            GoogleHtmlTranslator::TranslationChunks chunks;
            chunks.push_back(QStringList{m_plainTextView->toPlainText()});
            translateChunks(std::move(chunks));
            return;
        }

        if (m_activeView == ActiveView::Html)
        {
            m_htmlView->collectTranslationChunks(
                [this, selectedEmailId, translateChunks](QVector<QStringList> chunks)
                {
                    if (m_emailId != selectedEmailId)
                    {
                        return;
                    }
                    if (chunks.empty())
                    {
                        m_translationInProgress = false;
                        m_translationError =
                            QStringLiteral("Translation failed: no message text was found.");
                        updateLanguageBanner();
                        return;
                    }
                    translateChunks(std::move(chunks));
                });
        }
    }

    void MessageViewContainer::restoreCurrentTranslation()
    {
        if (m_activeView == ActiveView::PlainText && !m_originalPlainText.isEmpty())
        {
            m_plainTextView->setHtml(linkifyPlainText(m_originalPlainText));
        }
        else if (m_activeView == ActiveView::Html)
        {
            m_htmlView->restoreOriginalText();
        }

        m_translationInProgress = false;
        m_messageTranslated = false;
        m_originalPlainText.clear();
        m_translationError.clear();
        updateLanguageBanner();
    }

    void MessageViewContainer::startLanguageDetection()
    {
        if (m_languageDetectionStarted || !m_snapshot.has_value())
        {
            return;
        }

        m_languageDetectionStarted = true;
        const auto selectedEmailId = m_emailId;
        auto* watcher =
            new QFutureWatcher<std::optional<javelin::jmap::language::LanguageDetectionResult>>{
                this};
        connect(watcher,
                &QFutureWatcher<
                    std::optional<javelin::jmap::language::LanguageDetectionResult>>::finished,
                this,
                [this, watcher, selectedEmailId]
                {
                    const auto detection = watcher->result();
                    watcher->deleteLater();
                    if (m_emailId != selectedEmailId || !m_snapshot.has_value())
                    {
                        return;
                    }

                    m_snapshot->languageDetection = detection;
                    m_snapshot->shouldOfferTranslation =
                        detection.has_value() &&
                        javelin::jmap::language::shouldOfferTranslation(*detection);
                    updateLanguageBanner();
                    maybeAutoTranslateCurrentMessage();
                });
        watcher->setFuture(QtConcurrent::run([snapshot = *m_snapshot]
                                             { return detectMessageLanguage(snapshot); }));
    }

    void MessageViewContainer::updatePresentation(const bool reloadBody)
    {
        if (reloadBody)
        {
            m_plainTextView->clear();
            if (!m_snapshot.has_value() || !m_snapshot->htmlBody.has_value())
            {
                m_htmlView->clearDocument();
            }
            m_translationInProgress = false;
            m_messageTranslated = false;
            m_originalPlainText.clear();
            m_translationError.clear();
            m_autoTranslateAttempted = false;
            m_translationWasAutomatic = false;
            m_languageDetectionStarted = false;
            m_htmlDocumentLoaded = false;
        }
        m_attachmentStatusLabel->clear();
        rebuildAttachmentRows();
        rebuildMultipleSelectionRows();
        updateAttachmentSection();
        updateRemoteContentButton();
        updateLanguageBanner();
        m_loadingIndicator->setVisible(false);

        const auto presentation =
            messageViewPresentation(m_accountId.has_value(), m_mailboxId.has_value(),
                                    m_emailId.has_value(), m_multipleMessages.size());

        if (presentation == MessageViewPresentation::NoAccount)
        {
            m_detailLabel->setVisible(true);
            m_metadataWidget->setVisible(false);
            m_titleLabel->setText(QStringLiteral("Choose an account"));
            m_detailLabel->setText(QStringLiteral("Select an account to browse your mail."));
            m_placeholderTitleLabel->setText(QStringLiteral("Ready when you are"));
            m_placeholderDetailLabel->setText(
                QStringLiteral("Message details will appear here after you choose an account, "
                               "mailbox, and message."));
            setActiveView(ActiveView::Placeholder);
            return;
        }

        if (presentation == MessageViewPresentation::MultipleSelection)
        {
            m_detailLabel->setVisible(true);
            m_metadataWidget->setVisible(false);
            m_titleLabel->setText(QStringLiteral("%1 messages selected")
                                      .arg(static_cast<qulonglong>(m_multipleMessages.size())));
            m_detailLabel->clear();
            m_bodyControlsWidget->setVisible(false);
            setActiveView(ActiveView::Multiple);
            return;
        }

        if (presentation == MessageViewPresentation::NoMailbox)
        {
            m_detailLabel->setVisible(true);
            m_metadataWidget->setVisible(false);
            m_titleLabel->setText(QStringLiteral("Choose a mailbox"));
            m_detailLabel->setText(
                QStringLiteral("Select a mailbox in the left pane to populate the message list."));
            m_placeholderTitleLabel->setText(QStringLiteral("Choose a mailbox"));
            m_placeholderDetailLabel->setText(
                QStringLiteral("Choose a mailbox to see its messages here."));
            setActiveView(ActiveView::Placeholder);
            return;
        }

        if (presentation == MessageViewPresentation::NoMessage)
        {
            m_detailLabel->setVisible(true);
            m_metadataWidget->setVisible(false);
            m_titleLabel->setText(QStringLiteral("Choose a message"));
            m_detailLabel->setText(
                QStringLiteral("Select a message in the center pane to open it here."));
            m_placeholderTitleLabel->setText(QStringLiteral("Choose a message"));
            m_placeholderDetailLabel->setText(QStringLiteral("Select a message to read it here."));
            setActiveView(ActiveView::Placeholder);
            return;
        }

        if (!m_snapshot.has_value())
        {
            m_detailLabel->setVisible(true);
            m_metadataWidget->setVisible(false);
            if (!m_errorMessage.isEmpty())
            {
                m_titleLabel->setText(QStringLiteral("Could not load message"));
                m_detailLabel->setText(m_errorMessage);
                m_placeholderTitleLabel->setText(QStringLiteral("Message retrieval failed"));
                m_placeholderDetailLabel->setText(m_errorMessage);
            }
            else if (m_loading)
            {
                m_titleLabel->setText(QStringLiteral("Loading message"));
                m_detailLabel->setText(QStringLiteral("Downloading the selected message now."));
                m_placeholderTitleLabel->setText(QStringLiteral("Loading message"));
                if (m_placeholderDetailLabel->text().isEmpty())
                {
                    m_placeholderDetailLabel->setText(
                        QStringLiteral("Downloading the selected message now."));
                }
                m_loadingIndicator->setVisible(true);
            }
            else
            {
                m_titleLabel->setText(QStringLiteral("Message is unavailable"));
                m_detailLabel->setText(
                    QStringLiteral("This message is not available on this device yet."));
                m_placeholderTitleLabel->setText(QStringLiteral("Message unavailable"));
                m_placeholderDetailLabel->setText(QStringLiteral(
                    "Try refreshing the mailbox or reopening the message in a moment."));
            }
            setActiveView(ActiveView::Placeholder);
            return;
        }

        const auto subject = m_snapshot->email.subject.has_value()
                                 ? QString::fromStdString(*m_snapshot->email.subject)
                                 : QStringLiteral("(no subject)");

        m_titleLabel->setText(subject);
        m_detailLabel->clear();
        m_detailLabel->setVisible(false);
        m_metadataWidget->setVisible(true);
        m_fromLabel->setText(QStringLiteral("From: %1").arg(contactAwareSenderLabel()));
        m_toLabel->setText(QStringLiteral("To: %1").arg(addressListLabel(m_snapshot->email.to)));
        m_receivedLabel->setText(QStringLiteral("Received: %1")
                                     .arg(formatReceivedDateTime(m_snapshot->email.receivedAt)));

        if (reloadBody && m_snapshot->plainTextBody.has_value())
        {
            m_plainTextView->setHtml(
                linkifyPlainText(QString::fromStdString(m_snapshot->plainTextBody->value)));
        }

        if (reloadBody && m_snapshot->htmlBody.has_value())
        {
            const auto renderDocument =
                m_snapshot->htmlRenderDocument.has_value()
                    ? QString::fromStdString(m_snapshot->htmlRenderDocument->html)
                    : QString::fromStdString(m_snapshot->htmlBody->value);
            m_htmlDocumentLoaded = false;
            m_loading = true;
            m_htmlView->setDocumentHtml(renderDocument.toStdString(),
                                        *m_accountId + "\n" + *m_emailId);
        }
        updateSenderRemoteContentPermit();

        m_attachmentStatusLabel->setText(attachmentStatusText());
        rebuildAttachmentRows();
        updateAttachmentSection();
        updateRemoteContentButton();

        if (m_snapshot->htmlBody.has_value())
        {
            if (m_htmlDocumentLoaded)
            {
                setActiveView(ActiveView::Html);
                startLanguageDetection();
            }
            else
            {
                m_placeholderTitleLabel->setText(QStringLiteral("Loading message"));
                m_placeholderDetailLabel->setText(
                    QStringLiteral("Preparing the selected message for display."));
                m_loadingIndicator->setVisible(true);
                setActiveView(ActiveView::Placeholder);
            }
        }
        else if (m_snapshot->plainTextBody.has_value())
        {
            m_loading = false;
            setActiveView(ActiveView::PlainText);
            startLanguageDetection();
        }
        else
        {
            if (m_loading)
            {
                m_titleLabel->setText(QStringLiteral("Loading message"));
                m_detailLabel->setText(QStringLiteral("Downloading the selected message now."));
                m_placeholderTitleLabel->setText(QStringLiteral("Loading message"));
                m_placeholderDetailLabel->setText(
                    QStringLiteral("Downloading the selected message now."));
                m_loadingIndicator->setVisible(true);
            }
            else
            {
                m_placeholderTitleLabel->setText(QStringLiteral("Nothing to display"));
                m_placeholderDetailLabel->setText(QStringLiteral(
                    "This message does not currently have a readable body available."));
            }
            setActiveView(ActiveView::Placeholder);
        }
    }

    void MessageViewContainer::permitRemoteContentForCurrentSender()
    {
        addRemoteContentAllowListValue(QLatin1StringView{allowedSendersKey},
                                       currentSenderAddress());
        updateSenderRemoteContentPermit();
        updateRemoteContentButton();
    }

    void MessageViewContainer::permitRemoteContentForCurrentDomain()
    {
        addRemoteContentAllowListValue(QLatin1StringView{allowedDomainsKey}, currentSenderDomain());
        updateSenderRemoteContentPermit();
        updateRemoteContentButton();
    }

    QString MessageViewContainer::contactAwareSenderLabel() const
    {
        if (!m_snapshot.has_value() || m_snapshot->email.from.empty() || !m_accountId.has_value())
        {
            return m_snapshot.has_value() ? addressListLabel(m_snapshot->email.from) : QString{};
        }
        QStringList labels;
        for (const auto& sender : m_snapshot->email.from)
        {
            const auto resolved = m_contactIdentityLookup.resolve(*m_accountId, sender.email);
            const auto* identity =
                std::get_if<std::optional<javelin::jmap::contacts::ContactIdentity>>(&resolved);
            if (identity == nullptr || !identity->has_value())
            {
                labels.push_back(addressListLabel({sender}));
                continue;
            }
            QString name = QString::fromStdString((*identity)->displayName);
            if ((*identity)->organization.has_value() &&
                *(*identity)->organization != (*identity)->displayName)
            {
                name +=
                    QStringLiteral(" — %1").arg(QString::fromStdString(*(*identity)->organization));
            }
            labels.push_back(
                QStringLiteral("%1 <%2>").arg(name, QString::fromStdString(sender.email)));
        }
        return labels.join(QStringLiteral(", "));
    }

    QString MessageViewContainer::currentSenderAddress() const
    {
        if (!m_snapshot.has_value() || m_snapshot->email.from.empty())
        {
            return {};
        }
        return QString::fromStdString(m_snapshot->email.from.front().email).trimmed().toLower();
    }

    QString MessageViewContainer::currentSenderDomain() const
    {
        const auto sender = currentSenderAddress();
        const auto atIndex = sender.lastIndexOf(QLatin1Char('@'));
        if (atIndex < 0 || atIndex + 1 >= sender.size())
        {
            return {};
        }
        return sender.sliced(atIndex + 1).trimmed().toLower();
    }

    void MessageViewContainer::rebuildAttachmentRows()
    {
        while (QLayoutItem* item = m_attachmentListLayout->takeAt(0))
        {
            if (QWidget* widget = item->widget())
            {
                widget->deleteLater();
            }
            delete item;
        }

        const auto attachments = visibleAttachments(m_snapshot);
        const bool hasAttachments = !attachments.empty();
        m_attachmentListWidget->setVisible(hasAttachments);
        m_attachmentsCollapsed = false;
        if (!hasAttachments || !m_accountId.has_value() || !m_emailId.has_value())
        {
            return;
        }

        constexpr int tileSpacing = 6;
        std::vector<AttachmentTile*> tiles;
        tiles.reserve(attachments.size());
        const auto attachmentSaveSettings =
            javelin::gui::settings::PreferencesDialog::loadAttachmentSaveSettings();

        for (std::size_t index = 0; index < attachments.size(); ++index)
        {
            const auto* attachment = attachments.at(index);
            const auto fileName = attachmentName(*attachment);
            const auto saveToolTip =
                attachmentSaveSettings.alwaysAsk
                    ? QStringLiteral("Save %1 to selected location").arg(fileName)
                    : QStringLiteral("Save %1 to %2")
                          .arg(fileName, attachmentSaveSettings.directory);
            auto* tile = new AttachmentTile(
                *attachment,
                [this, partId = QString::fromStdString(attachment->partId)]
                {
                    Q_EMIT openAttachmentRequested(QString::fromStdString(*m_accountId),
                                                   QString::fromStdString(*m_emailId), partId);
                },
                [this, partId = QString::fromStdString(attachment->partId)]
                {
                    Q_EMIT saveAttachmentRequested(QString::fromStdString(*m_accountId),
                                                   QString::fromStdString(*m_emailId), partId);
                },
                saveToolTip, m_attachmentListWidget);
            tiles.push_back(tile);
        }

        const int availableWidth =
            std::max(m_attachmentListWidget->contentsRect().width(),
                     m_attachmentHeaderWidget->contentsRect().width() -
                         m_attachmentStatusLabel->sizeHint().width() -
                         m_saveAllAttachmentsButton->sizeHint().width() - 12);
        int row = 0;
        int column = 0;
        int rowWidth = 0;
        int maxColumnCount = 0;
        for (auto* tile : tiles)
        {
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
            m_attachmentListLayout->addWidget(tile, row, column, Qt::AlignLeft);
            rowWidth = column == 0 ? tileWidth : rowWidth + tileSpacing + tileWidth;
            ++column;
        }
        maxColumnCount = std::max(maxColumnCount, column);

        for (int stretchColumn = 0; stretchColumn < maxColumnCount; ++stretchColumn)
        {
            m_attachmentListLayout->setColumnStretch(stretchColumn, 0);
        }
        m_attachmentListLayout->setColumnStretch(maxColumnCount, 1);
    }

    void MessageViewContainer::rebuildMultipleSelectionRows()
    {
        while (QLayoutItem* item = m_multipleSelectionLayout->takeAt(0))
        {
            if (QWidget* widget = item->widget())
            {
                widget->deleteLater();
            }
            delete item;
        }

        for (std::size_t index = 0; index < m_multipleMessages.size(); ++index)
        {
            const auto& message = m_multipleMessages[index];
            auto* tile = new MessagePreviewTile(
                message, [this, emailId = QString::fromStdString(message.emailId)]
                { Q_EMIT messageActivated(emailId); }, m_multipleSelectionWidget);
            m_multipleSelectionLayout->addWidget(tile);

            if (index + 1 < m_multipleMessages.size())
            {
                auto* separator = new QFrame(m_multipleSelectionWidget);
                separator->setFrameShape(QFrame::HLine);
                separator->setFrameShadow(QFrame::Plain);
                separator->setStyleSheet(QStringLiteral("color: #393d46;"));
                m_multipleSelectionLayout->addWidget(separator);
            }
        }

        m_multipleSelectionLayout->addStretch(1);
    }

    QString MessageViewContainer::attachmentStatusText() const
    {
        if (!m_snapshot.has_value())
        {
            return {};
        }

        const auto attachments = visibleAttachments(m_snapshot);
        if (!attachments.empty())
        {
            const auto attachmentCount = static_cast<qulonglong>(attachments.size());
            return QStringLiteral("%1 %2")
                .arg(attachmentCount)
                .arg(attachmentCount == 1 ? QStringLiteral("attachment")
                                          : QStringLiteral("attachments"));
        }

        if (m_snapshot->htmlRenderDocument.has_value() &&
            m_snapshot->htmlRenderDocument->inlineResourceCount > 0)
        {
            return QStringLiteral("Inline resources: %1")
                .arg(static_cast<qulonglong>(m_snapshot->htmlRenderDocument->inlineResourceCount));
        }

        return {};
    }

    void MessageViewContainer::updateAttachmentSection()
    {
        const bool hasAttachments = !visibleAttachments(m_snapshot).empty();
        m_attachmentHeaderWidget->setVisible(hasAttachments);
        m_attachmentStatusLabel->setVisible(hasAttachments);
        m_attachmentExpanderButton->setVisible(false);
        m_saveAllAttachmentsButton->setVisible(hasAttachments);
        m_saveAllAttachmentsButton->setEnabled(hasAttachments);
        m_attachmentListWidget->setVisible(hasAttachments);
    }

    void MessageViewContainer::resizeEvent(QResizeEvent* event)
    {
        QWidget::resizeEvent(event);
        if (!visibleAttachments(m_snapshot).empty())
        {
            rebuildAttachmentRows();
            updateAttachmentSection();
        }
    }

} // namespace javelin::gui::messageview
