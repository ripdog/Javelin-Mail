#include "gui/messageview/MessageViewContainer.h"
#include "app/MessageSubject.h"
#include "gui/FontUtils.h"
#include "gui/IconUtils.h"
#include "gui/accessibility/AccessibleFactory.h"
#include "gui/messageview/HtmlMessageView.h"
#include "gui/messageview/MessageAttachmentPanel.h"
#include "gui/messageview/MessageBannerCoordinator.h"
#include "gui/messageview/MessageTranslationController.h"
#include "gui/messageview/MessageViewPresentation.h"
#include "gui/messageview/PlainTextLinkifier.h"
#include "gui/messageview/RemoteContentController.h"
#include "gui/settings/GuiSettings.h"
#include "gui/translation/TranslationService.h"
#include "gui/widgets/IndeterminateProgressBar.h"
#include "jmap/contacts/ContactIdentityLookup.h"
#include "jmap/render/HtmlTextExtractor.h"

#include <QCoroTask>

#include <KLocalizedString>
#include <KMessageWidget>

#include <QAccessible>
#include <QAccessibleWidget>
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
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMimeDatabase>
#include <QMouseEvent>
#include <QPainter>
#include <QPoint>
#include <QPrintDialog>
#include <QPrinter>
#include <QProgressBar>
#include <QScrollArea>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStackedLayout>
#include <QStackedWidget>
#include <QStringList>
#include <QStyle>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <variant>
#include <vector>

namespace javelin::gui::messageview
{
    namespace
    {
        constexpr std::string_view RemoteContentBannerId{"remote-content"};
        constexpr std::string_view TranslationBannerId{"translation"};
        constexpr std::string_view JunkBannerId{"junk"};
        constexpr std::string_view UnsubscribeBannerId{"unsubscribe"};
        constexpr auto AccessibleDocumentTextProperty = "_javelin_accessible_document_text";

        class MessageViewBanner final : public KMessageWidget
        {
          public:
            using KMessageWidget::KMessageWidget;

          protected:
            void paintEvent(QPaintEvent*) override
            {
                // Reader conditions are persistent context, not alerts. Keep KMessageWidget's
                // semantics and controls while matching the surrounding application palette.
                QPainter painter(this);
                painter.setRenderHint(QPainter::Antialiasing);
                painter.setPen(QPen(palette().color(QPalette::Mid), 1));
                painter.setBrush(palette().color(QPalette::AlternateBase));
                painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
            }
        };

        class AccessibleMessageView final : public QAccessibleWidget,
                                            public QAccessibleTextInterface
        {
          public:
            explicit AccessibleMessageView(MessageViewContainer* view)
                : QAccessibleWidget(view, QAccessible::Document)
            {
            }

            [[nodiscard]] void* interface_cast(const QAccessible::InterfaceType type) override
            {
                if (type == QAccessible::TextInterface)
                    return static_cast<QAccessibleTextInterface*>(this);
                return QAccessibleWidget::interface_cast(type);
            }

            [[nodiscard]] QAccessible::State state() const override
            {
                auto result = QAccessibleWidget::state();
                result.focusable = true;
                result.focused = widget() != nullptr && widget()->hasFocus();
                result.readOnly = true;
                result.selectableText = true;
                return result;
            }

            void selection(const int selectionIndex, int* startOffset,
                           int* endOffset) const override
            {
                Q_UNUSED(selectionIndex);
                if (startOffset != nullptr)
                    *startOffset = -1;
                if (endOffset != nullptr)
                    *endOffset = -1;
            }

            [[nodiscard]] int selectionCount() const override
            {
                return 0;
            }

            void addSelection(const int startOffset, const int endOffset) override
            {
                Q_UNUSED(startOffset);
                Q_UNUSED(endOffset);
            }

            void removeSelection(const int selectionIndex) override
            {
                Q_UNUSED(selectionIndex);
            }

            void setSelection(const int selectionIndex, const int startOffset,
                              const int endOffset) override
            {
                Q_UNUSED(selectionIndex);
                Q_UNUSED(startOffset);
                Q_UNUSED(endOffset);
            }

            [[nodiscard]] int cursorPosition() const override
            {
                return m_cursorPosition;
            }

            void setCursorPosition(const int position) override
            {
                m_cursorPosition = std::clamp(position, 0, characterCount());
            }

            [[nodiscard]] QString text(const int startOffset, const int endOffset) const override
            {
                const auto contents = documentText();
                const auto length = static_cast<int>(contents.size());
                const auto start = std::clamp(startOffset, 0, length);
                const auto end = endOffset < 0 ? length : std::clamp(endOffset, start, length);
                return contents.mid(start, end - start);
            }

            [[nodiscard]] int characterCount() const override
            {
                return static_cast<int>(documentText().size());
            }

            [[nodiscard]] QRect characterRect(const int offset) const override
            {
                Q_UNUSED(offset);
                return {};
            }

            [[nodiscard]] int offsetAtPoint(const QPoint& point) const override
            {
                Q_UNUSED(point);
                return -1;
            }

            void scrollToSubstring(const int startIndex, const int endIndex) override
            {
                Q_UNUSED(startIndex);
                Q_UNUSED(endIndex);
            }

            [[nodiscard]] QString attributes(const int offset, int* startOffset,
                                             int* endOffset) const override
            {
                Q_UNUSED(offset);
                if (startOffset != nullptr)
                    *startOffset = 0;
                if (endOffset != nullptr)
                    *endOffset = characterCount();
                return {};
            }

          private:
            [[nodiscard]] QString documentText() const
            {
                return object() != nullptr
                           ? object()->property(AccessibleDocumentTextProperty).toString()
                           : QString{};
            }

            int m_cursorPosition = 0;
        };

        [[nodiscard]] QAccessibleInterface* messageViewAccessibleFactory(const QString& key,
                                                                         QObject* object)
        {
            auto* view = accessibility::factoryObject<MessageViewContainer>(key, object);
            return view != nullptr ? new AccessibleMessageView(view) : nullptr;
        }

        void ensureMessageViewAccessibilityFactoryInstalled()
        {
            static const bool installed = []
            {
                QAccessible::installFactory(messageViewAccessibleFactory);
                return true;
            }();
            Q_UNUSED(installed);
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
                return i18nc("@item no email addresses", "(none)");
            }

            QStringList labels;
            labels.reserve(static_cast<qsizetype>(addresses.size()));
            for (const auto& address : addresses)
            {
                labels.push_back(addressLabel(address));
            }
            return labels.join(QStringLiteral(", "));
        }

        [[nodiscard]] QString
        formatReceivedDateTime(const std::string& receivedAt,
                               const QLocale::FormatType format = QLocale::ShortFormat)
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

            return QLocale{}.toString(dateTime.toLocalTime(), format);
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

                const auto subject = javelin::app::subjectForDisplay(message.subject);
                auto* subjectLabel = new QLabel(subject, this);
                subjectLabel->setWordWrap(true);
                auto subjectFont = javelin::gui::fontWithSizeDelta(subjectLabel->font(), 1);
                subjectFont.setBold(true);
                subjectLabel->setFont(subjectFont);

                const auto preview = message.preview.has_value()
                                         ? QString::fromStdString(*message.preview)
                                         : i18n("(no preview available)");
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

    } // namespace

    MessageViewContainer::MessageViewContainer(
        javelin::gui::settings::GuiSettings& settings,
        javelin::gui::translation::TranslationService& translationService,
        javelin::jmap::contacts::ContactIdentityLookup& contactIdentityLookup, QWidget* parent)
        : QWidget(parent), m_settings(settings), m_translationService(translationService),
          m_contactIdentityLookup(contactIdentityLookup)
    {
        ensureMessageViewAccessibilityFactoryInstalled();
        setFocusPolicy(Qt::StrongFocus);
        setAccessibleName(i18nc("@info accessible message view", "Message view"));

        m_bannerCoordinator =
            std::make_unique<MessageBannerCoordinator>(m_settings, m_accountId, m_emailId);
        m_remoteContentController = std::make_unique<RemoteContentController>(m_settings);
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

        auto titleFont = javelin::gui::fontWithSizeDelta(m_titleLabel->font(), 4);
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

        const auto makeBanner = [this](const KMessageWidget::MessageType type, const QIcon& icon)
        {
            auto* banner = new MessageViewBanner(this);
            banner->setMessageType(type);
            banner->setIcon(icon);
            banner->setWordWrap(false);
            banner->setVisible(false);
            return banner;
        };

        m_remoteContentBanner =
            makeBanner(KMessageWidget::Information,
                       javelin::gui::themedSvgIcon(
                           QStringLiteral(":/icons/thunderbird-icons/remote-blocked.svg"),
                           palette().color(QPalette::Active, QPalette::Text)));
        m_permitSenderRemoteContentAction = new QAction(i18n("Always load sender"), this);
        m_remoteContentBanner->addAction(m_permitSenderRemoteContentAction);
        connect(m_permitSenderRemoteContentAction, &QAction::triggered, this,
                &MessageViewContainer::permitRemoteContentForCurrentSender);
        m_permitDomainRemoteContentAction = new QAction(i18n("Always load domain"), this);
        m_remoteContentBanner->addAction(m_permitDomainRemoteContentAction);
        connect(m_permitDomainRemoteContentAction, &QAction::triggered, this,
                &MessageViewContainer::permitRemoteContentForCurrentDomain);
        m_remoteContentAction = new QAction(i18n("Load remote content"), this);
        m_remoteContentAction->setCheckable(true);
        m_remoteContentBanner->addAction(m_remoteContentAction);
        connect(m_remoteContentAction, &QAction::triggered, this,
                [this](const bool checked)
                {
                    m_htmlView->setRemoteContentEnabled(checked);
                    updateRemoteContentButton();
                });
        connect(m_remoteContentBanner, &KMessageWidget::hideAnimationFinished, this,
                [this]
                {
                    dismissMessageBanner(RemoteContentBannerId);
                    updateRemoteContentButton();
                });

        m_junkBanner = makeBanner(
            KMessageWidget::Warning,
            javelin::gui::themedSvgIcon(QStringLiteral(":/icons/thunderbird-icons/spam.svg"),
                                        palette().color(QPalette::Active, QPalette::Text)));
        m_notJunkAction = new QAction(i18nc("@action:button", "Not Junk"), this);
        m_junkBanner->addAction(m_notJunkAction);
        connect(m_notJunkAction, &QAction::triggered, this,
                [this]
                {
                    if (!m_accountId.has_value() || !m_emailId.has_value())
                    {
                        return;
                    }
                    Q_EMIT notJunkRequested(
                        QString::fromStdString(*m_accountId),
                        m_mailboxId.has_value() ? QString::fromStdString(*m_mailboxId) : QString{},
                        QString::fromStdString(*m_emailId));
                });
        connect(m_junkBanner, &KMessageWidget::hideAnimationFinished, this,
                [this]
                {
                    dismissMessageBanner(JunkBannerId);
                    updateJunkBanner();
                });

        m_unsubscribeBanner = makeBanner(KMessageWidget::Information,
                                         QIcon::fromTheme(QStringLiteral("news-unsubscribe")));
        m_unsubscribeBanner->setTextFormat(Qt::RichText);
        connect(m_unsubscribeBanner, &KMessageWidget::linkActivated, this, [](const QString& url)
                { QDesktopServices::openUrl(QUrl::fromEncoded(url.toUtf8())); });
        connect(m_unsubscribeBanner, &KMessageWidget::linkHovered, this,
                [this](const QString& url) { Q_EMIT hoveredLinkChanged(url); });
        connect(m_unsubscribeBanner, &KMessageWidget::hideAnimationFinished, this,
                [this]
                {
                    dismissMessageBanner(UnsubscribeBannerId);
                    updateUnsubscribeBanner();
                });

        m_translationBanner =
            makeBanner(KMessageWidget::Information,
                       QIcon::fromTheme(QStringLiteral("preferences-desktop-locale")));
        m_translateAction = new QAction(i18nc("@action:button", "Translate"), this);
        m_translationBanner->addAction(m_translateAction);
        connect(m_translateAction, &QAction::triggered, this,
                [this]() { translateCurrentMessage(); });
        m_translateOptionsAction = new QAction(i18nc("@action:button", "Options"), this);
        m_translateOptionsAction->setToolTip(i18n("Translation options"));
        m_translateOptionsMenu = new QMenu(m_translationBanner);
        auto* autoSenderAction = m_translateOptionsMenu->addAction(i18n("Auto-translate sender"));
        autoSenderAction->setCheckable(true);
        autoSenderAction->setData(QStringLiteral("sender"));
        auto* autoDomainAction =
            m_translateOptionsMenu->addAction(i18n("Auto-translate sender domain"));
        autoDomainAction->setCheckable(true);
        autoDomainAction->setData(QStringLiteral("domain"));
        connect(autoSenderAction, &QAction::toggled, this,
                &MessageViewContainer::setAutoTranslateSender);
        connect(autoDomainAction, &QAction::toggled, this,
                &MessageViewContainer::setAutoTranslateDomain);
        connect(m_translateOptionsAction, &QAction::triggered, this,
                [this]
                {
                    const auto x =
                        m_translationBanner->layoutDirection() == Qt::RightToLeft
                            ? 0
                            : std::max(0, m_translationBanner->width() -
                                              m_translateOptionsMenu->sizeHint().width());
                    m_translateOptionsMenu->popup(
                        m_translationBanner->mapToGlobal(QPoint{x, m_translationBanner->height()}));
                });
        m_translationBanner->addAction(m_translateOptionsAction);
        connect(m_translationBanner, &KMessageWidget::hideAnimationFinished, this,
                [this]
                {
                    dismissMessageBanner(TranslationBannerId);
                    updateLanguageBanner();
                });

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
        auto placeholderTitleFont =
            javelin::gui::fontWithSizeDelta(m_placeholderTitleLabel->font(), 2);
        placeholderTitleFont.setBold(true);
        m_placeholderTitleLabel->setFont(placeholderTitleFont);
        m_placeholderTitleLabel->setWordWrap(true);
        makeLabelSelectable(m_placeholderTitleLabel);

        m_placeholderDetailLabel = new QLabel(placeholderCard);
        m_placeholderDetailLabel->setWordWrap(true);
        m_placeholderDetailLabel->setStyleSheet(QStringLiteral("color: #c5cad3;"));
        makeLabelSelectable(m_placeholderDetailLabel);

        m_loadingIndicator = new javelin::gui::widgets::IndeterminateProgressBar(placeholderCard);
        m_loadingIndicator->setAccessibleName(
            i18nc("@info accessible progress", "Loading message"));
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

        m_htmlView = new HtmlMessageView(m_settings.messageAppearanceSettings(), this);
        m_htmlView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        m_bodyPresenter = std::make_unique<MessageBodyPresenter>(*m_bodyStack, *m_placeholderPanel,
                                                                 *m_multipleSelectionScrollArea,
                                                                 *m_plainTextView, *m_htmlView);
        m_translationController = std::make_unique<MessageTranslationController>(
            m_translationService, *m_bodyPresenter, *m_plainTextView, *m_htmlView, m_snapshot,
            m_emailId, [this] { return currentSenderAddress(); },
            [this] { return currentSenderDomain(); });
        connect(m_translationController.get(), &MessageTranslationController::stateChanged, this,
                [this]
                {
                    updateLanguageBanner();
                    updateAccessibleDocument();
                });
        connect(m_htmlView, &HtmlMessageView::viewSourceRequested, this,
                &MessageViewContainer::viewSourceRequested);
        connect(m_htmlView, &HtmlMessageView::findRequested, this,
                &MessageViewContainer::showFindBar);
        connect(m_htmlView, &HtmlMessageView::zoomInRequested, this, &MessageViewContainer::zoomIn);
        connect(m_htmlView, &HtmlMessageView::zoomOutRequested, this,
                &MessageViewContainer::zoomOut);
        connect(m_htmlView, &HtmlMessageView::resetZoomRequested, this,
                &MessageViewContainer::resetZoom);
        connect(m_htmlView, &HtmlMessageView::printRequested, this,
                &MessageViewContainer::printMessage);
        connect(m_htmlView, &HtmlMessageView::hoveredLinkChanged, this,
                &MessageViewContainer::hoveredLinkChanged);
        connect(
            m_htmlView, &HtmlMessageView::documentLoaded, this,
            [this](const QString& documentId)
            {
                if (!m_bodyPresenter->acceptHtmlDocumentLoaded(documentId, m_accountId, m_emailId))
                    return;
                m_loading = false;
                updatePresentation(false);
                if (m_bodyPresenter->activeView() == ActiveView::Html)
                    maybeAutoTranslateCurrentMessage();
            });

        m_bodyStack->addWidget(m_placeholderPanel);
        m_bodyStack->addWidget(m_multipleSelectionScrollArea);
        m_bodyStack->addWidget(m_plainTextView);
        m_bodyStack->addWidget(m_htmlView);

        m_attachmentPanel =
            new MessageAttachmentPanel(m_settings, m_accountId, m_emailId, m_snapshot, this);
        connect(m_attachmentPanel, &MessageAttachmentPanel::saveAttachmentRequested, this,
                &MessageViewContainer::saveAttachmentRequested);
        connect(m_attachmentPanel, &MessageAttachmentPanel::openAttachmentRequested, this,
                &MessageViewContainer::openAttachmentRequested);
        connect(m_attachmentPanel, &MessageAttachmentPanel::saveAllAttachmentsRequested, this,
                &MessageViewContainer::saveAllAttachmentsRequested);

        m_findBarContainer = new QWidget(this);
        m_findBarContainer->setObjectName(QStringLiteral("messageFindBar"));
        auto* findBarStack = new QStackedLayout(m_findBarContainer);
        findBarStack->setContentsMargins(0, 0, 0, 0);
        findBarStack->setStackingMode(QStackedLayout::StackAll);

        m_findBar = new MessageViewBanner(m_findBarContainer);
        m_findBar->setMessageType(KMessageWidget::Information);
        m_findBar->setPosition(KMessageWidget::Footer);
        m_findBar->setText(QString{});
        m_findBar->setIcon(QIcon{});
        m_findBar->setCloseButtonVisible(false);
        findBarStack->addWidget(m_findBar);

        auto* findControls = new QWidget(m_findBarContainer);
        findControls->setAttribute(Qt::WA_StyledBackground, false);
        auto* findLayout = new QHBoxLayout(findControls);
        findLayout->setContentsMargins(12, 6, 8, 6);
        findLayout->setSpacing(6);
        auto* findLabel = new QLabel(i18nc("@label", "Find:"), findControls);
        m_findEdit = new QLineEdit(findControls);
        m_findEdit->setObjectName(QStringLiteral("messageFindEdit"));
        m_findEdit->setClearButtonEnabled(true);
        m_findEdit->setPlaceholderText(i18n("Find in message"));
        m_findEdit->setAccessibleName(i18n("Find in message"));
        m_findEdit->installEventFilter(this);
        m_findResultLabel = new QLabel(findControls);
        m_findResultLabel->setMinimumWidth(m_findResultLabel->fontMetrics().horizontalAdvance(
            i18nc("@info find result count", "999 of 999")));
        m_findResultLabel->setAlignment(Qt::AlignCenter);

        m_findPreviousButton = new QToolButton(findControls);
        m_findPreviousButton->setAutoRaise(true);
        m_findPreviousButton->setIcon(QIcon::fromTheme(QStringLiteral("go-up")));
        m_findPreviousButton->setToolTip(i18n("Previous match"));
        m_findPreviousButton->setAccessibleName(i18n("Previous match"));
        m_findNextButton = new QToolButton(findControls);
        m_findNextButton->setAutoRaise(true);
        m_findNextButton->setIcon(QIcon::fromTheme(QStringLiteral("go-down")));
        m_findNextButton->setToolTip(i18n("Next match"));
        m_findNextButton->setAccessibleName(i18n("Next match"));
        auto* closeFindButton = new QToolButton(findControls);
        closeFindButton->setAutoRaise(true);
        closeFindButton->setIcon(QIcon::fromTheme(QStringLiteral("window-close")));
        closeFindButton->setToolTip(i18n("Close find bar"));
        closeFindButton->setAccessibleName(i18n("Close find bar"));

        findLayout->addWidget(findLabel);
        findLayout->addWidget(m_findEdit, 1);
        findLayout->addWidget(m_findResultLabel);
        findLayout->addWidget(m_findPreviousButton);
        findLayout->addWidget(m_findNextButton);
        findLayout->addWidget(closeFindButton);
        findBarStack->addWidget(findControls);
        findControls->raise();
        m_findBarContainer->setVisible(false);

        connect(m_findEdit, &QLineEdit::textChanged, this,
                [this]
                {
                    m_plainTextFindQuery.clear();
                    m_plainTextFindIndex = -1;
                    runFind(false);
                });
        connect(m_findEdit, &QLineEdit::returnPressed, this,
                [this]
                {
                    if (QGuiApplication::keyboardModifiers().testFlag(Qt::ShiftModifier))
                    {
                        findPrevious();
                    }
                    else
                    {
                        findNext();
                    }
                });
        connect(m_findPreviousButton, &QToolButton::clicked, this,
                &MessageViewContainer::findPrevious);
        connect(m_findNextButton, &QToolButton::clicked, this, &MessageViewContainer::findNext);
        connect(closeFindButton, &QToolButton::clicked, this,
                &MessageViewContainer::dismissFindBar);
        auto* dismissFindShortcut = new QShortcut(QKeySequence{Qt::Key_Escape}, m_findBarContainer);
        dismissFindShortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(dismissFindShortcut, &QShortcut::activated, this,
                &MessageViewContainer::dismissFindBar);

        layout->addWidget(headerWidget);
        layout->addWidget(m_remoteContentBanner);
        layout->addWidget(m_translationBanner);
        layout->addWidget(m_junkBanner);
        layout->addWidget(m_unsubscribeBanner);
        layout->addWidget(m_bodyStack, 1);
        layout->addWidget(m_findBarContainer);
        layout->addWidget(m_attachmentPanel);

        connect(&m_contactIdentityLookup,
                &javelin::jmap::contacts::ContactIdentityLookup::contactDataChanged, this,
                [this]
                {
                    if (m_snapshot.has_value())
                    {
                        m_fromLabel->setText(
                            i18nc("@label email sender", "From: %1", contactAwareSenderLabel()));
                        updateAccessibleDocument();
                    }
                });
        updatePresentation();
    }

    MessageViewContainer::~MessageViewContainer() = default;

    void MessageViewContainer::focusMessageBody()
    {
        if (m_bodyPresenter == nullptr)
            return;

        switch (m_bodyPresenter->activeView())
        {
        case ActiveView::Html:
            m_htmlView->setFocus(Qt::ShortcutFocusReason);
            break;
        case ActiveView::PlainText:
            m_plainTextView->setFocus(Qt::ShortcutFocusReason);
            break;
        case ActiveView::Multiple:
            m_multipleSelectionScrollArea->setFocus(Qt::ShortcutFocusReason);
            break;
        case ActiveView::Placeholder:
            m_bodyStack->setFocus(Qt::ShortcutFocusReason);
            break;
        }
    }

    bool MessageViewContainer::readerActionsAvailable() const
    {
        if (m_bodyPresenter == nullptr || !m_snapshot.has_value())
        {
            return false;
        }
        const auto view = m_bodyPresenter->activeView();
        return view == ActiveView::Html || view == ActiveView::PlainText;
    }

    void MessageViewContainer::showFindBar()
    {
        if (!readerActionsAvailable())
        {
            return;
        }
        m_findBarContainer->setVisible(true);
        m_findEdit->setFocus(Qt::ShortcutFocusReason);
        m_findEdit->selectAll();
        if (!m_findEdit->text().isEmpty())
        {
            runFind(false);
        }
    }

    void MessageViewContainer::dismissFindBar()
    {
        clearFindHighlights();
        m_findBarContainer->setVisible(false);
        focusMessageBody();
    }

    void MessageViewContainer::findNext()
    {
        runFind(false);
    }

    void MessageViewContainer::findPrevious()
    {
        runFind(true);
    }

    void MessageViewContainer::runFind(const bool backwards)
    {
        if (!readerActionsAvailable() || m_findEdit == nullptr)
        {
            updateFindResult(0, 0);
            return;
        }

        const QString query = m_findEdit->text();
        if (query.isEmpty())
        {
            clearFindHighlights();
            return;
        }

        if (m_bodyPresenter->activeView() == ActiveView::Html)
        {
            m_htmlView->findText(query, backwards,
                                 [this, query](const int activeMatch, const int matchCount)
                                 {
                                     if (m_findEdit->text() == query &&
                                         m_bodyPresenter->activeView() == ActiveView::Html)
                                     {
                                         updateFindResult(activeMatch, matchCount);
                                     }
                                 });
            return;
        }

        const bool newQuery = m_plainTextFindQuery != query;
        std::vector<QTextCursor> matches;
        QTextCursor cursor{m_plainTextView->document()};
        while (true)
        {
            cursor = m_plainTextView->document()->find(query, cursor);
            if (cursor.isNull())
            {
                break;
            }
            matches.push_back(cursor);
        }

        if (matches.empty())
        {
            m_plainTextFindQuery = query;
            m_plainTextFindIndex = -1;
            m_plainTextView->setExtraSelections({});
            updateFindResult(0, 0);
            return;
        }

        const int matchCount = static_cast<int>(matches.size());
        if (newQuery || m_plainTextFindIndex < 0 || m_plainTextFindIndex >= matchCount)
        {
            m_plainTextFindIndex = backwards ? matchCount - 1 : 0;
        }
        else if (backwards)
        {
            m_plainTextFindIndex = (m_plainTextFindIndex + matchCount - 1) % matchCount;
        }
        else
        {
            m_plainTextFindIndex = (m_plainTextFindIndex + 1) % matchCount;
        }
        m_plainTextFindQuery = query;

        QList<QTextEdit::ExtraSelection> highlights;
        highlights.reserve(matchCount);
        for (const auto& match : matches)
        {
            QTextEdit::ExtraSelection selection;
            selection.cursor = match;
            selection.format.setBackground(palette().brush(QPalette::AlternateBase));
            highlights.push_back(std::move(selection));
        }
        m_plainTextView->setExtraSelections(highlights);
        m_plainTextView->setTextCursor(matches[static_cast<std::size_t>(m_plainTextFindIndex)]);
        m_plainTextView->ensureCursorVisible();
        updateFindResult(m_plainTextFindIndex + 1, matchCount);
    }

    void MessageViewContainer::clearFindHighlights()
    {
        m_htmlView->clearFindHighlights();
        m_plainTextView->setExtraSelections({});
        auto cursor = m_plainTextView->textCursor();
        cursor.clearSelection();
        m_plainTextView->setTextCursor(cursor);
        m_plainTextFindQuery.clear();
        m_plainTextFindIndex = -1;
        updateFindResult(0, 0);
    }

    void MessageViewContainer::updateFindResult(const int activeMatch, const int matchCount)
    {
        const bool hasQuery = m_findEdit != nullptr && !m_findEdit->text().isEmpty();
        if (!hasQuery)
        {
            m_findResultLabel->clear();
        }
        else if (matchCount <= 0)
        {
            m_findResultLabel->setText(i18nc("@info find result", "No matches"));
        }
        else
        {
            m_findResultLabel->setText(
                i18nc("@info find result count", "%1 of %2", activeMatch, matchCount));
        }
        const bool canNavigate = hasQuery && matchCount > 0;
        m_findPreviousButton->setEnabled(canNavigate);
        m_findNextButton->setEnabled(canNavigate);
    }

    void MessageViewContainer::applyZoom()
    {
        m_htmlView->setZoomFactor(std::pow(1.1, static_cast<double>(m_zoomSteps)));
    }

    void MessageViewContainer::zoomIn()
    {
        if (!readerActionsAvailable() || m_zoomSteps >= 15)
        {
            return;
        }
        ++m_zoomSteps;
        m_plainTextView->zoomIn(1);
        applyZoom();
    }

    void MessageViewContainer::zoomOut()
    {
        if (!readerActionsAvailable() || m_zoomSteps <= -8)
        {
            return;
        }
        --m_zoomSteps;
        m_plainTextView->zoomOut(1);
        applyZoom();
    }

    void MessageViewContainer::resetZoom()
    {
        if (!readerActionsAvailable())
        {
            return;
        }
        if (m_zoomSteps > 0)
        {
            m_plainTextView->zoomOut(m_zoomSteps);
        }
        else if (m_zoomSteps < 0)
        {
            m_plainTextView->zoomIn(-m_zoomSteps);
        }
        m_zoomSteps = 0;
        applyZoom();
    }

    void MessageViewContainer::printMessage()
    {
        if (!readerActionsAvailable())
        {
            return;
        }
        if (m_bodyPresenter->activeView() == ActiveView::Html)
        {
            m_htmlView->printDocument(m_titleLabel->text());
            return;
        }

        QPrinter printer{QPrinter::HighResolution};
        printer.setDocName(m_titleLabel->text());
        QPrintDialog dialog{&printer, this};
        if (dialog.exec() == QDialog::Accepted)
        {
            m_plainTextView->print(&printer);
        }
    }

    bool MessageViewContainer::eventFilter(QObject* watched, QEvent* event)
    {
        if (watched == m_findEdit && event->type() == QEvent::KeyPress &&
            static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape)
        {
            dismissFindBar();
            return true;
        }
        return QWidget::eventFilter(watched, event);
    }

    void MessageViewContainer::changeEvent(QEvent* event)
    {
        QWidget::changeEvent(event);
        if (event->type() != QEvent::PaletteChange &&
            event->type() != QEvent::ApplicationPaletteChange)
        {
            return;
        }

        m_remoteContentBanner->setIcon(javelin::gui::themedSvgIcon(
            QStringLiteral(":/icons/thunderbird-icons/remote-blocked.svg"),
            palette().color(QPalette::Active, QPalette::Text)));
        m_junkBanner->setIcon(
            javelin::gui::themedSvgIcon(QStringLiteral(":/icons/thunderbird-icons/spam.svg"),
                                        palette().color(QPalette::Active, QPalette::Text)));
        m_unsubscribeBanner->setIcon(QIcon::fromTheme(QStringLiteral("news-unsubscribe")));
        m_translationBanner->setIcon(
            QIcon::fromTheme(QStringLiteral("preferences-desktop-locale")));
    }

    void MessageViewContainer::translationSettingsChanged()
    {
        m_translationController->settingsChanged();
    }

    void MessageViewContainer::setSelection(
        javelin::jmap::cache::MessageViewReader& messageViewReader,
        std::optional<std::string> accountId, std::optional<std::string> mailboxId,
        std::optional<std::string> emailId, std::optional<std::string> junkMailboxId)
    {
        if (m_accountId == accountId && m_mailboxId == mailboxId && m_emailId == emailId &&
            m_junkMailboxId == junkMailboxId)
        {
            return;
        }

        m_htmlView->clearDocument();
        m_accountId = std::move(accountId);
        m_mailboxId = std::move(mailboxId);
        m_emailId = std::move(emailId);
        m_junkMailboxId = std::move(junkMailboxId);
        m_multipleMessages.clear();
        if (!m_emailId.has_value())
        {
            clearFindHighlights();
            m_findBarContainer->setVisible(false);
        }
        m_translationController->reset();
        ++m_snapshotLoadToken;
        m_loading = m_emailId.has_value();
        m_errorMessage.clear();

        m_snapshot = std::nullopt;
        updatePresentation();
        startSnapshotLoad(messageViewReader, true);
    }

    void MessageViewContainer::setMultipleSelection(
        std::optional<std::string> accountId, std::optional<std::string> mailboxId,
        std::vector<javelin::jmap::cache::MessageListItem> messages)
    {
        m_accountId = std::move(accountId);
        m_mailboxId = std::move(mailboxId);
        m_emailId = std::nullopt;
        m_junkMailboxId = std::nullopt;
        m_multipleMessages = std::move(messages);
        clearFindHighlights();
        m_findBarContainer->setVisible(false);
        m_translationController->reset();
        ++m_snapshotLoadToken;
        m_loading = false;
        m_errorMessage.clear();
        m_snapshot = std::nullopt;
        updatePresentation();
    }

    void MessageViewContainer::refresh(javelin::jmap::cache::MessageViewReader& messageViewReader)
    {
        m_errorMessage.clear();
        m_translationController->reset();
        m_loading = m_emailId.has_value();
        updatePresentation(false);
        startSnapshotLoad(messageViewReader, false);
    }

    void MessageViewContainer::startSnapshotLoad(
        javelin::jmap::cache::MessageViewReader& messageViewReader,
        const bool requestContentIfMissing)
    {
        if (!m_accountId.has_value() || !m_emailId.has_value())
            return;

        const auto accountId = *m_accountId;
        const auto emailId = *m_emailId;
        const auto token = ++m_snapshotLoadToken;
        auto* watcher = new QFutureWatcher<javelin::jmap::cache::MessageViewResult>{this};
        connect(
            watcher, &QFutureWatcher<javelin::jmap::cache::MessageViewResult>::finished, this,
            [this, watcher, accountId, emailId, token, requestContentIfMissing]
            {
                auto result = watcher->result();
                watcher->deleteLater();
                if (token != m_snapshotLoadToken || m_accountId != accountId ||
                    m_emailId != emailId)
                    return;

                if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
                {
                    m_loading = false;
                    m_errorMessage = error->message;
                    updatePresentation();
                    return;
                }

                auto snapshot = std::get<std::optional<javelin::jmap::cache::MessageViewSnapshot>>(
                    std::move(result));
                if (!snapshot.has_value())
                {
                    if (requestContentIfMissing)
                    {
                        Q_EMIT contentRequired(QString::fromStdString(accountId),
                                               QString::fromStdString(emailId));
                    }
                    else
                    {
                        m_loading = false;
                        m_errorMessage = i18n("The cached message content could not be loaded.");
                        updatePresentation();
                    }
                    return;
                }

                const auto previousRenderedBody = renderedBodyKey(m_snapshot);
                m_snapshot = std::move(snapshot);
                m_loading = false;
                m_errorMessage.clear();
                updatePresentation(previousRenderedBody != renderedBodyKey(m_snapshot));
            });
        watcher->setFuture(messageViewReader.loadAsync(accountId, emailId));
    }

    void MessageViewContainer::setActiveView(const ActiveView view)
    {
        m_bodyPresenter->setActiveView(view);
        const bool actionsAreAvailable = readerActionsAvailable();
        if (m_readerActionsAvailable != actionsAreAvailable)
        {
            m_readerActionsAvailable = actionsAreAvailable;
            Q_EMIT readerActionsAvailabilityChanged(actionsAreAvailable);
        }
        if (m_findBarContainer != nullptr && m_findBarContainer->isVisible() && actionsAreAvailable)
        {
            m_plainTextFindQuery.clear();
            m_plainTextFindIndex = -1;
            runFind(false);
        }
        else if (!actionsAreAvailable)
        {
            updateFindResult(0, 0);
        }
        updateSenderRemoteContentPermit();
        updateRemoteContentButton();
        updateJunkBanner();
        updateUnsubscribeBanner();
        updateLanguageBanner();
        updateAccessibleDocument();
    }

    void MessageViewContainer::updateAccessibleDocument()
    {
        const auto oldName = accessibleName();
        const auto oldText = property(AccessibleDocumentTextProperty).toString();
        auto name = m_titleLabel != nullptr ? m_titleLabel->text().simplified() : QString{};
        if (name.isEmpty())
            name = i18nc("@info accessible message view", "Message view");

        QStringList parts;
        const auto append = [&parts](QString value)
        {
            value = value.trimmed();
            if (!value.isEmpty() && !parts.contains(value))
                parts.push_back(std::move(value));
        };
        append(name);
        if (m_metadataWidget != nullptr && m_metadataWidget->isVisible())
        {
            append(m_fromLabel->text());
            append(m_toLabel->text());
            if (m_snapshot.has_value())
            {
                append(i18nc(
                    "@label email received date", "Received: %1",
                    formatReceivedDateTime(m_snapshot->email.receivedAt, QLocale::LongFormat)));
            }
            else
            {
                append(m_receivedLabel->text());
            }
        }
        else if (m_detailLabel != nullptr && m_detailLabel->isVisible())
        {
            append(m_detailLabel->text());
        }

        if (m_snapshot.has_value())
        {
            QString bodyText;
            if (m_translationController->messageTranslated() &&
                !m_translationController->translatedText().isEmpty())
            {
                bodyText = m_translationController->translatedText();
            }
            else if (m_bodyPresenter->activeView() == ActiveView::PlainText &&
                     m_plainTextView != nullptr)
            {
                bodyText = m_plainTextView->toPlainText();
            }
            else if (m_snapshot->plainTextBody.has_value())
            {
                bodyText = QString::fromStdString(m_snapshot->plainTextBody->value);
            }
            else if (m_snapshot->htmlBody.has_value())
            {
                bodyText = javelin::jmap::render::plainTextFromHtml(
                    QString::fromStdString(m_snapshot->htmlBody->value));
            }
            append(std::move(bodyText));
        }
        else if (m_placeholderTitleLabel != nullptr && m_placeholderTitleLabel->isVisible())
        {
            append(m_placeholderTitleLabel->text());
            if (m_placeholderDetailLabel != nullptr && m_placeholderDetailLabel->isVisible())
                append(m_placeholderDetailLabel->text());
        }

        const auto documentText = parts.join(QLatin1Char{'\n'});
        setAccessibleName(name);
        setAccessibleDescription({});
        setProperty(AccessibleDocumentTextProperty, documentText);

        if (!QAccessible::isActive())
            return;
        if (oldName != name && hasFocus())
        {
            QAccessibleEvent event{this, QAccessible::NameChanged};
            QAccessible::updateAccessibility(&event);
        }
        if (oldText != documentText && hasFocus())
        {
            QAccessibleTextUpdateEvent event{this, 0, oldText, documentText};
            QAccessible::updateAccessibility(&event);
        }
    }

    void MessageViewContainer::setErrorState(const QString& errorMessage)
    {
        m_loading = false;
        m_errorMessage = errorMessage;
        updatePresentation();
    }

    void MessageViewContainer::appearanceSettingsChanged()
    {
        m_htmlView->setAppearanceSettings(m_settings.messageAppearanceSettings());
    }

    void MessageViewContainer::updateSenderRemoteContentPermit()
    {
        m_remoteContentController->applySavedPermit(m_snapshot, *m_htmlView);
    }

    void MessageViewContainer::updateRemoteContentButton()
    {
        const bool hasBlockedRemoteContent =
            m_snapshot.has_value() && m_snapshot->htmlRenderDocument.has_value() &&
            m_snapshot->htmlRenderDocument->blockedRemoteResourceCount > 0;
        const bool remoteContentAllowed =
            hasBlockedRemoteContent && m_htmlView->remoteContentEnabled();
        const bool showRemoteContentControls = hasBlockedRemoteContent && !remoteContentAllowed &&
                                               !messageBannerDismissed(RemoteContentBannerId);
        m_remoteContentBanner->setVisible(showRemoteContentControls);
        m_permitSenderRemoteContentAction->setEnabled(hasBlockedRemoteContent &&
                                                      !currentSenderAddress().isEmpty());
        m_permitDomainRemoteContentAction->setEnabled(hasBlockedRemoteContent &&
                                                      !currentSenderDomain().isEmpty());
        m_remoteContentAction->setEnabled(hasBlockedRemoteContent);
        m_remoteContentAction->setChecked(hasBlockedRemoteContent &&
                                          m_htmlView->remoteContentEnabled());
        if (hasBlockedRemoteContent)
        {
            m_remoteContentBanner->setText(
                i18np("Blocked remote resource: %1", "Blocked remote resources: %1",
                      m_snapshot->htmlRenderDocument->blockedRemoteResourceCount));
        }
        else
        {
            m_remoteContentBanner->setText({});
        }
        m_permitSenderRemoteContentAction->setToolTip(
            currentSenderAddress().isEmpty() ? i18n("No sender address is available")
                                             : i18n("Always load remote content from this sender"));
        m_permitDomainRemoteContentAction->setToolTip(
            currentSenderDomain().isEmpty()
                ? i18n("No sender domain is available")
                : i18n("Always load remote content from this sender domain"));
        m_remoteContentAction->setText(m_htmlView->remoteContentEnabled()
                                           ? i18n("Hide remote content")
                                           : i18n("Load remote content"));
    }

    bool MessageViewContainer::messageBannerDismissed(const std::string_view bannerId) const
    {
        return m_bannerCoordinator->dismissed(bannerId);
    }

    void MessageViewContainer::dismissMessageBanner(const std::string_view bannerId)
    {
        m_bannerCoordinator->dismiss(bannerId);
    }

    QString MessageViewContainer::serverDisplayName() const
    {
        return m_bannerCoordinator->serverDisplayName();
    }

    void MessageViewContainer::updateJunkBanner()
    {
        const bool isJunk = m_snapshot.has_value() && m_junkMailboxId.has_value() &&
                            std::ranges::contains(m_snapshot->email.mailboxIds, *m_junkMailboxId);
        const bool shouldShow = isJunk && !messageBannerDismissed(JunkBannerId);
        m_junkBanner->setVisible(shouldShow);
        if (!shouldShow)
        {
            m_junkBanner->setText({});
            return;
        }

        m_junkBanner->setText(i18n("%1 marked this email as Junk.", serverDisplayName()));
    }

    void MessageViewContainer::updateUnsubscribeBanner()
    {
        const bool hasUnsubscribeUrl = m_snapshot.has_value() &&
                                       m_snapshot->unsubscribeUrl.has_value() &&
                                       !m_snapshot->unsubscribeUrl->empty();
        const bool shouldShow = hasUnsubscribeUrl && !messageBannerDismissed(UnsubscribeBannerId);
        m_unsubscribeBanner->setVisible(shouldShow);
        if (!shouldShow)
        {
            m_unsubscribeBanner->setText({});
            return;
        }

        const auto url = QString::fromStdString(*m_snapshot->unsubscribeUrl).toHtmlEscaped();
        m_unsubscribeBanner->setText(
            QStringLiteral("<table width=\"100%\" cellspacing=\"0\" cellpadding=\"0\"><tr>"
                           "<td>%1</td><td align=\"right\"><a href=\"%2\">%3</a></td>"
                           "</tr></table>")
                .arg(i18n("This message is from a mailing list.").toHtmlEscaped(), url,
                     i18nc("@action:link", "Unsubscribe").toHtmlEscaped()));
    }

    void MessageViewContainer::updateLanguageBanner()
    {
        const auto& detection = m_translationController->languageDetection();
        const bool hasLanguageOffer = m_snapshot.has_value() && detection.has_value() &&
                                      m_translationController->shouldOfferTranslation();
        const bool shouldShow = m_translationController->canTranslateActiveView() &&
                                !messageBannerDismissed(TranslationBannerId) &&
                                (hasLanguageOffer || m_translationController->inProgress() ||
                                 m_translationController->messageTranslated() ||
                                 !m_translationController->error().isEmpty());
        m_translationBanner->setVisible(shouldShow);
        updateTranslateOptionsMenu();
        if (!shouldShow)
        {
            m_translationBanner->setText({});
            return;
        }

        const auto targetName = languageName(m_translationService.targetLanguage().toStdString());
        m_translateAction->setEnabled(!m_translationController->inProgress());
        m_translateAction->setText(m_translationController->messageTranslated()
                                       ? i18n("Show original")
                                       : i18nc("@action:button", "Translate"));
        m_translateAction->setToolTip(m_translationController->messageTranslated()
                                          ? i18n("Restore the original message text")
                                          : i18n("Translate this message to %1", targetName));

        if (m_translationController->inProgress())
        {
            m_translationBanner->setText(m_translationController->progressText().isEmpty()
                                             ? i18n("Translating message…")
                                             : m_translationController->progressText());
            return;
        }
        if (!m_translationController->error().isEmpty())
        {
            m_translationBanner->setText(m_translationController->error());
            return;
        }
        if (m_translationController->messageTranslated())
        {
            m_translationBanner->setText(m_translationController->translationWasAutomatic()
                                             ? i18n("Auto-translated to %1.", targetName)
                                             : i18n("Message translated to %1.", targetName));
            return;
        }

        m_translationBanner->setText(
            i18n("This message appears to be in %1.", languageName(detection->languageCode)));
    }

    void MessageViewContainer::updateTranslateOptionsMenu()
    {
        if (m_translateOptionsMenu == nullptr)
        {
            return;
        }

        const auto sender = currentSenderAddress();
        const auto domain = currentSenderDomain();
        for (auto* action : m_translateOptionsMenu->actions())
        {
            const auto kind = action->data().toString();
            QSignalBlocker blocker{action};
            if (kind == QStringLiteral("sender"))
            {
                action->setEnabled(m_translationService.isEnabled() && !sender.isEmpty());
                action->setChecked(m_translationController->senderRuleEnabled());
            }
            else if (kind == QStringLiteral("domain"))
            {
                action->setEnabled(m_translationService.isEnabled() && !domain.isEmpty());
                action->setChecked(m_translationController->domainRuleEnabled());
            }
        }
    }

    void MessageViewContainer::setAutoTranslateSender(const bool enabled)
    {
        m_translationController->setAutoTranslateSender(enabled);
    }

    void MessageViewContainer::setAutoTranslateDomain(const bool enabled)
    {
        m_translationController->setAutoTranslateDomain(enabled);
    }

    void MessageViewContainer::maybeAutoTranslateCurrentMessage()
    {
        m_translationController->maybeAutoTranslate();
    }

    void MessageViewContainer::translateCurrentMessage()
    {
        m_translationController->translateCurrentMessage();
    }

    void MessageViewContainer::restoreCurrentTranslation()
    {
        m_translationController->restoreCurrentTranslation();
    }

    void MessageViewContainer::startLanguageDetection()
    {
        m_translationController->startLanguageDetection();
    }

    void MessageViewContainer::updatePresentation(const bool reloadBody)
    {
        if (reloadBody)
        {
            m_bodyPresenter->prepareForReload(m_snapshot);
            m_translationController->reset();
        }
        m_attachmentPanel->refresh();
        rebuildMultipleSelectionRows();
        updateRemoteContentButton();
        updateJunkBanner();
        updateUnsubscribeBanner();
        updateLanguageBanner();
        m_loadingIndicator->setVisible(false);
        m_placeholderDetailLabel->setVisible(true);

        const auto presentation =
            messageViewPresentation(m_accountId.has_value(), m_mailboxId.has_value(),
                                    m_emailId.has_value(), m_multipleMessages.size());

        if (presentation == MessageViewPresentation::NoAccount)
        {
            m_detailLabel->setVisible(true);
            m_metadataWidget->setVisible(false);
            m_titleLabel->setText(i18n("Choose an account"));
            m_detailLabel->setText(i18n("Select an account to browse your mail."));
            m_placeholderTitleLabel->setText(i18n("Ready when you are"));
            m_placeholderDetailLabel->setText(
                i18n("Message details will appear here after you choose an account, mailbox, and "
                     "message."));
            setActiveView(ActiveView::Placeholder);
            return;
        }

        if (presentation == MessageViewPresentation::MultipleSelection)
        {
            m_detailLabel->setVisible(true);
            m_metadataWidget->setVisible(false);
            m_titleLabel->setText(
                i18np("%1 message selected", "%1 messages selected", m_multipleMessages.size()));
            m_detailLabel->clear();
            m_remoteContentBanner->setVisible(false);
            setActiveView(ActiveView::Multiple);
            return;
        }

        if (presentation == MessageViewPresentation::NoMailbox)
        {
            m_detailLabel->setVisible(true);
            m_metadataWidget->setVisible(false);
            m_titleLabel->setText(i18n("Choose a mailbox"));
            m_detailLabel->setText(
                i18n("Select a mailbox in the left pane to populate the message list."));
            m_placeholderTitleLabel->setText(i18n("Choose a mailbox"));
            m_placeholderDetailLabel->setText(i18n("Choose a mailbox to see its messages here."));
            setActiveView(ActiveView::Placeholder);
            return;
        }

        if (presentation == MessageViewPresentation::NoMessage)
        {
            m_detailLabel->setVisible(true);
            m_metadataWidget->setVisible(false);
            m_titleLabel->setText(i18n("Choose a message"));
            m_detailLabel->setText(i18n("Select a message in the center pane to open it here."));
            m_placeholderTitleLabel->setText(i18n("Choose a message"));
            m_placeholderDetailLabel->setText(i18n("Select a message to read it here."));
            setActiveView(ActiveView::Placeholder);
            return;
        }

        if (!m_snapshot.has_value())
        {
            m_detailLabel->setVisible(true);
            m_metadataWidget->setVisible(false);
            if (!m_errorMessage.isEmpty())
            {
                m_titleLabel->setText(i18n("Could not load message"));
                m_detailLabel->setText(m_errorMessage);
                m_placeholderTitleLabel->setText(i18n("Message retrieval failed"));
                m_placeholderDetailLabel->setText(m_errorMessage);
            }
            else if (m_loading)
            {
                m_titleLabel->setText(i18n("Loading message"));
                m_detailLabel->clear();
                m_detailLabel->setVisible(false);
                m_placeholderTitleLabel->setText(i18n("Loading message"));
                m_placeholderDetailLabel->clear();
                m_placeholderDetailLabel->setVisible(false);
                m_loadingIndicator->setVisible(true);
            }
            else
            {
                m_titleLabel->setText(i18n("Message is unavailable"));
                m_detailLabel->setText(i18n("This message is not available on this device yet."));
                m_placeholderTitleLabel->setText(i18n("Message unavailable"));
                m_placeholderDetailLabel->setText(
                    i18n("Try refreshing the mailbox or reopening the message in a moment."));
            }
            setActiveView(ActiveView::Placeholder);
            return;
        }

        const auto subject = javelin::app::subjectForDisplay(m_snapshot->email.subject);

        m_titleLabel->setText(subject);
        m_detailLabel->clear();
        m_detailLabel->setVisible(false);
        m_metadataWidget->setVisible(true);
        m_fromLabel->setText(i18nc("@label email sender", "From: %1", contactAwareSenderLabel()));
        m_toLabel->setText(
            i18nc("@label email recipients", "To: %1", addressListLabel(m_snapshot->email.to)));
        m_receivedLabel->setText(i18nc("@label email received date", "Received: %1",
                                       formatReceivedDateTime(m_snapshot->email.receivedAt)));

        if (m_bodyPresenter->renderMessageBody(*m_snapshot, m_accountId, m_emailId, reloadBody))
            m_loading = true;
        updateSenderRemoteContentPermit();

        m_attachmentPanel->refresh();
        updateRemoteContentButton();

        if (m_snapshot->htmlBody.has_value())
        {
            setActiveView(ActiveView::Html);
            if (m_bodyPresenter->htmlDocumentLoaded())
                startLanguageDetection();
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
                m_titleLabel->setText(i18n("Loading message"));
                m_detailLabel->clear();
                m_detailLabel->setVisible(false);
                m_placeholderTitleLabel->setText(i18n("Loading message"));
                m_placeholderDetailLabel->clear();
                m_placeholderDetailLabel->setVisible(false);
                m_loadingIndicator->setVisible(true);
            }
            else
            {
                m_placeholderTitleLabel->setText(i18n("Nothing to display"));
                m_placeholderDetailLabel->setText(
                    i18n("This message does not currently have a readable body available."));
            }
            setActiveView(ActiveView::Placeholder);
        }
    }

    void MessageViewContainer::permitRemoteContentForCurrentSender()
    {
        m_remoteContentController->permitSender(m_snapshot);
        updateSenderRemoteContentPermit();
        updateRemoteContentButton();
    }

    void MessageViewContainer::permitRemoteContentForCurrentDomain()
    {
        m_remoteContentController->permitDomain(m_snapshot);
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
        return m_remoteContentController->senderAddress(m_snapshot);
    }

    QString MessageViewContainer::currentSenderDomain() const
    {
        return m_remoteContentController->senderDomain(m_snapshot);
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

} // namespace javelin::gui::messageview
