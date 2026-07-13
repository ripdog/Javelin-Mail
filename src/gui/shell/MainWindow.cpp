#include "gui/shell/MainWindow.h"

#include "app/ComposeService.h"
#include "app/LongPollCoordinator.h"
#include "gui/IconUtils.h"
#include "gui/calendar/EventDialog.h"
#include "gui/calendar/MonthCalendarWidget.h"
#include "gui/compose/ComposeTabWidget.h"
#include "gui/contacts/ContactsManagerWidget.h"
#include "gui/logging/LogViewerDialog.h"
#include "gui/mailboxes/MailboxIconUtils.h"
#include "gui/mailboxes/MailboxTreeDelegate.h"
#include "gui/mailboxes/MailboxTreeModel.h"
#include "gui/messages/MessageListDelegate.h"
#include "gui/messages/MessageListModel.h"
#include "gui/messageview/MessageViewContainer.h"
#include "gui/search/AdvancedSearchDialog.h"
#include "gui/settings/PreferencesDialog.h"
#include "gui/shell/LayeredStatusBar.h"
#include "gui/shell/MessageFileUtils.h"
#include "jmap/cache/AccountRepository.h"
#include "jmap/cache/ContactRepository.h"
#include "jmap/cache/IdentityRepository.h"
#include "jmap/cache/MessageViewService.h"
#include "jmap/cache/QueryService.h"
#include "jmap/calendar/CalendarService.h"
#include "jmap/contacts/ContactIdentityLookup.h"
#include "jmap/contacts/ContactService.h"
#include "jmap/query/QueryDiff.h"

#include <QCoroTask>

#include <KActionCollection>
#include <KStandardAction>

#include <QAbstractButton>
#include <QAction>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDebug>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDrag>
#include <QElapsedTimer>
#include <QEvent>
#include <QFileDialog>
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QItemSelection>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QLoggingCategory>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabBar>
#include <QTimeZone>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QtConcurrentRun>

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <unordered_set>

namespace javelin::gui::shell
{
    Q_LOGGING_CATEGORY(logGuiMailbox, "gui.mailbox")
    Q_LOGGING_CATEGORY(logUserOperations, "user.operations")
    void MainWindow::presentError(const javelin::jmap::LiveRefreshError& error,
                                  const QString& title)
    {
        m_statusBar->showMessage(error.message, 10000);
        if (error.requiresUserIntervention)
        {
            QMessageBox::critical(this, title, error.message);
        }
    }

    void MainWindow::presentUserInterventionError(const QString& message)
    {
        presentError({.message = message, .requiresUserIntervention = true});
    }

    namespace
    {
        constexpr auto windowGroup = "mainWindow";
        constexpr auto geometryKey = "geometry";
        constexpr auto splitterKey = "splitterState";
        constexpr auto activeTabIndexKey = "activeTabIndex";
        constexpr auto emailListSortPropertyKey = "emailListSortProperty";
        constexpr auto emailListSortDirectionKey = "emailListSortDirection";
        constexpr auto tabsKey = "tabs";

        class CompactDragListView final : public QListView
        {
          public:
            using QListView::QListView;

          protected:
            void startDrag(const Qt::DropActions supportedActions) override
            {
                auto indexes = selectionModel()->selectedRows();
                if (indexes.isEmpty() && currentIndex().isValid())
                {
                    indexes.push_back(currentIndex());
                }

                auto* dragMimeData = model()->mimeData(indexes);
                if (dragMimeData == nullptr)
                {
                    return;
                }

                const QString label = indexes.size() == 1
                                          ? QStringLiteral("1 selected")
                                          : QStringLiteral("%1 selected").arg(indexes.size());
                const QFontMetrics metrics{font()};
                const QSize badgeSize{metrics.horizontalAdvance(label) + 48,
                                      std::max(34, metrics.height() + 14)};
                const qreal scale = devicePixelRatioF();
                QPixmap badge{badgeSize * scale};
                badge.setDevicePixelRatio(scale);
                badge.fill(Qt::transparent);

                QPainter painter{&badge};
                painter.setRenderHint(QPainter::Antialiasing);
                painter.setPen(Qt::NoPen);
                painter.setBrush(palette().highlight());
                painter.drawRoundedRect(QRect{QPoint{}, badgeSize}.adjusted(1, 1, -1, -1), 8, 8);

                const QRect envelopeRect{12, (badgeSize.height() - 14) / 2, 20, 14};
                QPen envelopePen{palette().highlightedText(), 1.5};
                painter.setPen(envelopePen);
                painter.setBrush(Qt::NoBrush);
                painter.drawRoundedRect(envelopeRect, 2, 2);
                painter.drawLine(envelopeRect.topLeft(), envelopeRect.center());
                painter.drawLine(envelopeRect.topRight(), envelopeRect.center());

                painter.setPen(palette().highlightedText().color());
                painter.setFont(font());
                painter.drawText(QRect{40, 0, badgeSize.width() - 48, badgeSize.height()},
                                 Qt::AlignVCenter | Qt::AlignLeft, label);

                auto* drag = new QDrag{this};
                drag->setMimeData(dragMimeData);
                drag->setPixmap(badge);
                drag->setHotSpot(QPoint{-10, -10});
                static_cast<void>(drag->exec(supportedActions, defaultDropAction()));
            }
        };

        /// Returns the account ID for the currently selected mailbox tree index.
        /// Works for both account-level nodes and mailbox nodes.
        [[nodiscard]] std::optional<std::string> currentAccountId(const QTreeView& mailboxView)
        {
            const auto current = mailboxView.currentIndex();
            if (!current.isValid())
            {
                return std::nullopt;
            }

            const auto accountId =
                current.data(javelin::gui::mailboxes::MailboxTreeModel::AccountIdRole).toString();
            return accountId.isEmpty() ? std::optional<std::string>{std::nullopt}
                                       : std::optional<std::string>{accountId.toStdString()};
        }

        [[nodiscard]] std::optional<std::string> currentMailboxId(const QTreeView& mailboxView)
        {
            const auto currentIndex = mailboxView.currentIndex();
            if (!currentIndex.isValid())
            {
                return std::nullopt;
            }

            const auto mailboxId =
                currentIndex.data(javelin::gui::mailboxes::MailboxTreeModel::MailboxIdRole)
                    .toString();
            // Empty mailbox id means an account-level node is selected.
            return mailboxId.isEmpty() ? std::optional<std::string>{std::nullopt}
                                       : std::optional<std::string>{mailboxId.toStdString()};
        }

        [[nodiscard]] std::optional<std::string> currentMailboxRole(const QTreeView& mailboxView)
        {
            const auto currentIndex = mailboxView.currentIndex();
            if (!currentIndex.isValid())
            {
                return std::nullopt;
            }

            const auto role =
                currentIndex.data(javelin::gui::mailboxes::MailboxTreeModel::MailboxRoleRole)
                    .toString();
            return role.isEmpty() ? std::optional<std::string>{std::nullopt}
                                  : std::optional<std::string>{role.toStdString()};
        }

        [[nodiscard]] std::optional<std::string> currentEmailId(const QListView& messageView)
        {
            const auto currentIndex = messageView.currentIndex();
            if (!currentIndex.isValid())
            {
                return std::nullopt;
            }

            const auto emailId =
                currentIndex.data(javelin::gui::messages::MessageListModel::EmailIdRole).toString();
            return emailId.isEmpty() ? std::optional<std::string>{std::nullopt}
                                     : std::optional<std::string>{emailId.toStdString()};
        }

        [[nodiscard]] std::optional<std::string> currentThreadId(const QListView& messageView)
        {
            const auto currentIndex = messageView.currentIndex();
            if (!currentIndex.isValid())
            {
                return std::nullopt;
            }

            const auto threadId =
                currentIndex.data(javelin::gui::messages::MessageListModel::ThreadIdRole)
                    .toString();
            return threadId.isEmpty() ? std::optional<std::string>{std::nullopt}
                                      : std::optional<std::string>{threadId.toStdString()};
        }

        [[nodiscard]] std::optional<int> currentMessageRow(const QListView& messageView)
        {
            const auto currentIndex = messageView.currentIndex();
            if (!currentIndex.isValid())
            {
                return std::nullopt;
            }

            return currentIndex.row();
        }

        [[nodiscard]] bool indexIsUnread(const QModelIndex& index)
        {
            return index.isValid() &&
                   index.data(javelin::gui::messages::MessageListModel::IsUnreadRole).toBool();
        }

        [[nodiscard]] QModelIndex findIndexByRole(const QAbstractItemModel& model, const int role,
                                                  const QString& value,
                                                  const QModelIndex& parent = {})
        {
            const int rowCount = model.rowCount(parent);
            for (int row = 0; row < rowCount; ++row)
            {
                const QModelIndex index = model.index(row, 0, parent);
                if (!index.isValid())
                {
                    continue;
                }

                if (index.data(role).toString() == value)
                {
                    return index;
                }

                const QModelIndex childMatch = findIndexByRole(model, role, value, index);
                if (childMatch.isValid())
                {
                    return childMatch;
                }
            }

            return {};
        }

        [[nodiscard]] QModelIndex
        findMailboxIndexForSelection(const QAbstractItemModel& model, const QString& accountId,
                                     const std::optional<QString>& mailboxId,
                                     const QModelIndex& parent = {})
        {
            const int rowCount = model.rowCount(parent);
            for (int row = 0; row < rowCount; ++row)
            {
                const QModelIndex index = model.index(row, 0, parent);
                if (!index.isValid())
                {
                    continue;
                }

                const QString indexAccountId =
                    index.data(javelin::gui::mailboxes::MailboxTreeModel::AccountIdRole).toString();
                const QString indexMailboxId =
                    index.data(javelin::gui::mailboxes::MailboxTreeModel::MailboxIdRole).toString();
                const bool accountMatches = indexAccountId == accountId;
                const bool mailboxMatches =
                    mailboxId.has_value() ? indexMailboxId == *mailboxId : indexMailboxId.isEmpty();
                if (accountMatches && mailboxMatches)
                {
                    return index;
                }

                const QModelIndex childMatch =
                    findMailboxIndexForSelection(model, accountId, mailboxId, index);
                if (childMatch.isValid())
                {
                    return childMatch;
                }
            }

            return {};
        }

        [[nodiscard]] javelin::app::AccountConnectionSettings
        toAccountConnectionSettings(const javelin::gui::settings::ConnectionSettings& settings)
        {
            return javelin::app::AccountConnectionSettings{
                .sessionUrl = settings.sessionUrl.toStdString(),
                .loginEmail = settings.loginEmail.toStdString(),
                .apiKey = settings.apiKey.toStdString(),
            };
        }

        [[nodiscard]] std::optional<javelin::jmap::cache::MailboxTreeItem>
        findMailboxByRole(javelin::jmap::cache::QueryService& queryService,
                          const std::string_view accountId, const std::string_view role)
        {
            const auto result = queryService.listMailboxTree(accountId);
            const auto* mailboxes =
                std::get_if<std::vector<javelin::jmap::cache::MailboxTreeItem>>(&result);
            if (mailboxes == nullptr)
            {
                return std::nullopt;
            }

            for (const auto& mailbox : *mailboxes)
            {
                if (mailbox.role == std::optional<std::string>{std::string{role}})
                {
                    return mailbox;
                }
            }

            return std::nullopt;
        }

        [[nodiscard]] QJsonObject
        serializeEmailAddress(const javelin::jmap::domain::EmailAddress& address)
        {
            QJsonObject object;
            if (address.name.has_value())
            {
                object.insert(QStringLiteral("name"), QString::fromStdString(*address.name));
            }
            object.insert(QStringLiteral("email"), QString::fromStdString(address.email));
            return object;
        }

        [[nodiscard]] std::optional<javelin::jmap::domain::EmailAddress>
        deserializeEmailAddress(const QJsonValue& value)
        {
            if (!value.isObject())
            {
                return std::nullopt;
            }

            const auto object = value.toObject();
            const auto email = object.value(QStringLiteral("email")).toString();
            if (email.isEmpty())
            {
                return std::nullopt;
            }

            const auto name = object.value(QStringLiteral("name")).toString();
            return javelin::jmap::domain::EmailAddress{
                .name =
                    name.isEmpty() ? std::nullopt : std::optional<std::string>{name.toStdString()},
                .email = email.toStdString(),
            };
        }

        [[nodiscard]] QJsonObject
        serializeMessageListItem(const javelin::jmap::cache::MessageListItem& item)
        {
            QJsonObject object;
            object.insert(QStringLiteral("emailId"), QString::fromStdString(item.emailId));
            object.insert(QStringLiteral("threadId"), QString::fromStdString(item.threadId));
            if (item.subject.has_value())
            {
                object.insert(QStringLiteral("subject"), QString::fromStdString(*item.subject));
            }
            if (item.preview.has_value())
            {
                object.insert(QStringLiteral("preview"), QString::fromStdString(*item.preview));
            }
            object.insert(QStringLiteral("receivedAt"), QString::fromStdString(item.receivedAt));
            if (item.sentAt.has_value())
            {
                object.insert(QStringLiteral("sentAt"), QString::fromStdString(*item.sentAt));
            }
            object.insert(QStringLiteral("threadMessageCount"),
                          static_cast<qint64>(item.threadMessageCount));
            object.insert(QStringLiteral("hasAttachment"), item.hasAttachment);
            object.insert(QStringLiteral("isUnread"), item.isUnread);
            object.insert(QStringLiteral("isFlagged"), item.isFlagged);
            if (item.from.has_value())
            {
                object.insert(QStringLiteral("from"), serializeEmailAddress(*item.from));
            }
            return object;
        }

        [[nodiscard]] std::optional<javelin::jmap::cache::MessageListItem>
        deserializeMessageListItem(const QJsonValue& value)
        {
            if (!value.isObject())
            {
                return std::nullopt;
            }

            const auto object = value.toObject();
            const auto emailId = object.value(QStringLiteral("emailId")).toString();
            const auto threadId = object.value(QStringLiteral("threadId")).toString();
            const auto receivedAt = object.value(QStringLiteral("receivedAt")).toString();
            if (emailId.isEmpty() || threadId.isEmpty() || receivedAt.isEmpty())
            {
                return std::nullopt;
            }

            return javelin::jmap::cache::MessageListItem{
                .emailId = emailId.toStdString(),
                .threadId = threadId.toStdString(),
                .subject = object.value(QStringLiteral("subject")).isUndefined()
                               ? std::nullopt
                               : std::optional<std::string>{object.value(QStringLiteral("subject"))
                                                                .toString()
                                                                .toStdString()},
                .preview = object.value(QStringLiteral("preview")).isUndefined()
                               ? std::nullopt
                               : std::optional<std::string>{object.value(QStringLiteral("preview"))
                                                                .toString()
                                                                .toStdString()},
                .receivedAt = receivedAt.toStdString(),
                .sentAt = object.value(QStringLiteral("sentAt")).isUndefined()
                              ? std::nullopt
                              : std::optional<std::string>{object.value(QStringLiteral("sentAt"))
                                                               .toString()
                                                               .toStdString()},
                .threadMessageCount = static_cast<std::uint64_t>(
                    object.value(QStringLiteral("threadMessageCount")).toInteger(1)),
                .hasAttachment = object.value(QStringLiteral("hasAttachment")).toBool(false),
                .isUnread = object.value(QStringLiteral("isUnread")).toBool(false),
                .isFlagged = object.value(QStringLiteral("isFlagged")).toBool(false),
                .from = deserializeEmailAddress(object.value(QStringLiteral("from"))),
            };
        }

        [[nodiscard]] std::optional<std::string> optionalStringSetting(const QSettings& settings,
                                                                       const QString& key)
        {
            const auto value = settings.value(key).toString();
            return value.isEmpty() ? std::nullopt : std::optional<std::string>{value.toStdString()};
        }

        [[nodiscard]] javelin::jmap::search::EmailSearchCriteria
        searchCriteriaFromSettings(const QSettings& settings)
        {
            auto text = optionalStringSetting(settings, QStringLiteral("searchText"));
            const auto query = optionalStringSetting(settings, QStringLiteral("query"));
            if (!text.has_value())
            {
                text = query;
            }

            return javelin::jmap::search::EmailSearchCriteria{
                .text = std::move(text),
                .with = optionalStringSetting(settings, QStringLiteral("searchWith")),
                .from = optionalStringSetting(settings, QStringLiteral("searchFrom")),
                .to = optionalStringSetting(settings, QStringLiteral("searchTo")),
                .cc = optionalStringSetting(settings, QStringLiteral("searchCc")),
                .bcc = optionalStringSetting(settings, QStringLiteral("searchBcc")),
                .subject = optionalStringSetting(settings, QStringLiteral("searchSubject")),
                .body = optionalStringSetting(settings, QStringLiteral("searchBody")),
            };
        }

        [[nodiscard]] std::vector<javelin::jmap::cache::MessageListItem>
        cachedSearchItems(const QSettings& settings)
        {
            const auto cachedItems =
                QJsonDocument::fromJson(settings.value(QStringLiteral("cachedItems")).toByteArray())
                    .array();

            std::vector<javelin::jmap::cache::MessageListItem> items;
            items.reserve(static_cast<std::size_t>(cachedItems.size()));
            for (const auto& itemValue : cachedItems)
            {
                if (const auto item = deserializeMessageListItem(itemValue))
                {
                    items.push_back(*item);
                }
            }
            return items;
        }

        void writeCommonTabSettings(QSettings& settings, const std::string& accountId,
                                    const QString& title, const std::size_t offset,
                                    const std::optional<std::string>& threadId,
                                    const std::optional<std::string>& emailId)
        {
            settings.setValue(QStringLiteral("accountId"), QString::fromStdString(accountId));
            settings.setValue(QStringLiteral("title"), title);
            settings.setValue(QStringLiteral("offset"), static_cast<qulonglong>(offset));
            settings.setValue(QStringLiteral("threadId"),
                              threadId.has_value() ? QString::fromStdString(*threadId) : QString{});
            settings.setValue(QStringLiteral("emailId"),
                              emailId.has_value() ? QString::fromStdString(*emailId) : QString{});
        }

        void writeOptionalSearchField(QSettings& settings, const QString& key,
                                      const std::optional<std::string>& value)
        {
            if (value.has_value())
            {
                settings.setValue(key, QString::fromStdString(*value));
            }
            else
            {
                settings.remove(key);
            }
        }

        [[nodiscard]] javelin::jmap::query::EmailListSortProperty
        sortPropertyFromSetting(const QString& value)
        {
            if (value == QStringLiteral("sentAt"))
            {
                return javelin::jmap::query::EmailListSortProperty::SentAt;
            }
            if (value == QStringLiteral("from"))
            {
                return javelin::jmap::query::EmailListSortProperty::From;
            }
            if (value == QStringLiteral("to"))
            {
                return javelin::jmap::query::EmailListSortProperty::To;
            }
            if (value == QStringLiteral("subject"))
            {
                return javelin::jmap::query::EmailListSortProperty::Subject;
            }
            if (value == QStringLiteral("size"))
            {
                return javelin::jmap::query::EmailListSortProperty::Size;
            }

            return javelin::jmap::query::EmailListSortProperty::ReceivedAt;
        }

        [[nodiscard]] QString
        sortPropertySetting(const javelin::jmap::query::EmailListSortProperty property)
        {
            return QString::fromStdString(javelin::jmap::query::propertyName(property));
        }

        [[nodiscard]] javelin::jmap::query::EmailListSortDirection
        sortDirectionFromSetting(const QString& value)
        {
            if (value == QStringLiteral("ascending"))
            {
                return javelin::jmap::query::EmailListSortDirection::Ascending;
            }

            return javelin::jmap::query::EmailListSortDirection::Descending;
        }

        [[nodiscard]] QString
        sortDirectionSetting(const javelin::jmap::query::EmailListSortDirection direction)
        {
            return direction == javelin::jmap::query::EmailListSortDirection::Ascending
                       ? QStringLiteral("ascending")
                       : QStringLiteral("descending");
        }

        [[nodiscard]] QString
        sortPropertyLabel(const javelin::jmap::query::EmailListSortProperty property)
        {
            switch (property)
            {
            case javelin::jmap::query::EmailListSortProperty::ReceivedAt:
                return QStringLiteral("Date received");
            case javelin::jmap::query::EmailListSortProperty::SentAt:
                return QStringLiteral("Date sent");
            case javelin::jmap::query::EmailListSortProperty::From:
                return QStringLiteral("From");
            case javelin::jmap::query::EmailListSortProperty::To:
                return QStringLiteral("To");
            case javelin::jmap::query::EmailListSortProperty::Subject:
                return QStringLiteral("Subject");
            case javelin::jmap::query::EmailListSortProperty::Size:
                return QStringLiteral("Size");
            }

            return QStringLiteral("Date received");
        }

        [[nodiscard]] QString
        sortDirectionLabel(const javelin::jmap::query::EmailListSortDirection direction)
        {
            return direction == javelin::jmap::query::EmailListSortDirection::Ascending
                       ? QStringLiteral("ascending")
                       : QStringLiteral("descending");
        }

    } // namespace

    MainWindow::MainWindow(
        javelin::jmap::cache::AccountRepository& accountRepository,
        javelin::jmap::cache::ContactRepository& contactRepository,
        javelin::jmap::contacts::ContactService& contactService,
        javelin::jmap::calendar::CalendarService& calendarService,
        javelin::jmap::contacts::ContactIdentityLookup& contactIdentityLookup,
        javelin::jmap::cache::IdentityRepository& identityRepository,
        javelin::jmap::cache::MessageViewService& messageViewService,
        javelin::jmap::cache::QueryService& queryService,
        javelin::jmap::cache::TranslationCacheRepository& translationCacheRepository,
        javelin::app::ComposeService& composeService,
        javelin::app::MailApplicationService& mailService, QWidget* parent)
        : KXmlGuiWindow(parent), m_accountRepository(accountRepository),
          m_contactRepository(contactRepository), m_contactService(contactService),
          m_calendarService(calendarService), m_contactIdentityLookup(contactIdentityLookup),
          m_identityRepository(identityRepository), m_messageViewService(messageViewService),
          m_queryService(queryService), m_translationCacheRepository(translationCacheRepository),
          m_composeService(composeService), m_mailService(mailService)
    {
        m_statusBar = new LayeredStatusBar(this);
        setStatusBar(m_statusBar);
        setupUi();
        connect(&m_mailService, &javelin::app::MailApplicationService::sessionCapabilitiesChanged,
                this, [this](const QString&) { reloadAccounts(); });
        createActions();
        setupGUI(KXmlGuiWindow::ToolBar | KXmlGuiWindow::Keys | KXmlGuiWindow::Save |
                     KXmlGuiWindow::Create,
                 QStringLiteral("javelinmailui.rc"));
        setToolBarVisible(QStringLiteral("mainToolBar"), true);
        connectSelection();
        connect(&m_mailService, &javelin::app::MailApplicationService::accountStatusChanged, this,
                [this](const QString& accountId, const auto status)
                {
                    using Model = javelin::gui::mailboxes::MailboxTreeModel;
                    Model::ConnectionStatus modelStatus = Model::ConnectionStatus::Disconnected;
                    if (status == javelin::app::AccountSyncCoordinator::Status::Connecting)
                    {
                        modelStatus = Model::ConnectionStatus::Connecting;
                    }
                    else if (status == javelin::app::AccountSyncCoordinator::Status::Connected)
                    {
                        modelStatus = Model::ConnectionStatus::Connected;
                    }
                    m_mailboxModel->setConnectionStatus(accountId, modelStatus);
                });
        connect(&m_mailService, &javelin::app::MailApplicationService::cacheCommitted, this,
                [this](const javelin::app::MailCacheChange& change)
                {
                    {
                        QSignalBlocker mailboxSelectionBlocker{m_mailboxView->selectionModel()};
                        m_mailboxModel->refresh();
                        m_mailboxView->expandAll();
                    }

                    for (const auto& window : change.searchWindows)
                    {
                        loadSearchTabFromCache(change.accountId.toStdString(),
                                               window.queryKey.toStdString(), window.offset, true);
                    }

                    std::unordered_set<std::string> queryWindowMailboxIds;
                    for (const auto& window : change.queryWindows)
                    {
                        const auto mailboxId = window.mailboxId.toStdString();
                        queryWindowMailboxIds.insert(mailboxId);
                        loadMailboxTabFromCache(change.accountId.toStdString(), mailboxId, true,
                                                window.offset);
                    }

                    for (const auto& mailboxId : change.mailboxIds)
                    {
                        const auto mailbox = mailboxId.toStdString();
                        if (!queryWindowMailboxIds.contains(mailbox))
                        {
                            loadMailboxTabFromCache(change.accountId.toStdString(), mailbox, true);
                        }
                        if (change.hasNewMail && activeTabIsMailbox() &&
                            activeAccountId() ==
                                std::optional<std::string>{change.accountId.toStdString()} &&
                            activeMailboxId() == std::optional<std::string>{mailbox} &&
                            m_messageModel->rowCount() > 0 &&
                            m_messageView->verticalScrollBar()->value() == 0)
                        {
                            m_messageView->scrollTo(m_messageModel->index(0, 0));
                        }
                    }
                });
        restorePersistentState();

        auto* stateSaveTimer = new QTimer(this);
        stateSaveTimer->setInterval(std::chrono::minutes{1});
        connect(stateSaveTimer, &QTimer::timeout, this, &MainWindow::savePersistentState);
        stateSaveTimer->start();
    }

    void MainWindow::openMessageFromNotification(const QString& accountId, const QString& mailboxId,
                                                 const QString& threadId, const QString& emailId)
    {
        const auto account = accountId.toStdString();
        const auto mailbox = mailboxId.toStdString();
        const auto thread = threadId.isEmpty() ? std::optional<std::string>{std::nullopt}
                                               : std::optional<std::string>{threadId.toStdString()};
        const auto email = emailId.isEmpty() ? std::optional<std::string>{std::nullopt}
                                             : std::optional<std::string>{emailId.toStdString()};

        reloadAccounts();
        QString mailboxTitle = mailboxId;
        std::optional<std::string> mailboxRole;
        const auto mailboxIndex = findMailboxIndexForSelection(*m_mailboxModel, accountId,
                                                               std::optional<QString>{mailboxId});
        if (mailboxIndex.isValid())
        {
            mailboxTitle = mailboxIndex.data(Qt::DisplayRole).toString();
            const auto role =
                mailboxIndex.data(javelin::gui::mailboxes::MailboxTreeModel::MailboxRoleRole)
                    .toString();
            if (!role.isEmpty())
            {
                mailboxRole = role.toStdString();
            }
        }

        openOrActivateMailboxTab(account, mailbox, mailboxTitle, mailboxRole, false);
        if (auto* tab = activeTab())
        {
            if (auto* mailboxTab = std::get_if<MailboxTabState>(&tab->content))
            {
                mailboxTab->selection.threadId = thread;
                mailboxTab->selection.emailId = email;
            }
        }
        loadMailboxTabFromCache(account, mailbox, true);
        const QModelIndex notificationIndex = restoreMessageSelection(thread, email);
        if (notificationIndex.isValid())
        {
            m_messageView->setCurrentIndex(notificationIndex);
            m_messageView->scrollTo(notificationIndex);
            syncActiveTabSelectionFromViews();
        }
        if (email.has_value())
        {
            m_messageViewContainer->setSelection(m_messageViewService,
                                                 std::optional<std::string>{account},
                                                 std::optional<std::string>{mailbox}, email);
            refreshSelectedMessageContent(account, *email);
        }
        else
        {
            refreshSelectionFromModels();
        }
    }

    void MainWindow::createActions()
    {
        const auto iconColor = palette().color(QPalette::Text);
        auto thunderbirdIcon = [iconColor](const QString& resourcePath)
        { return javelin::gui::themedSvgIcon(resourcePath, iconColor); };

        m_refreshAction = new QAction(
            thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/cloud-download.svg")),
            QStringLiteral("Refresh From Server"), this);
        m_refreshAction->setShortcut(QKeySequence::Refresh);
        connect(m_refreshAction, &QAction::triggered, this, &MainWindow::refreshFromServer);
        actionCollection()->addAction(QStringLiteral("refresh_from_server"), m_refreshAction);
        actionCollection()->setDefaultShortcut(m_refreshAction, QKeySequence::Refresh);

        m_quitAction = new QAction(QIcon::fromTheme(QStringLiteral("application-exit")),
                                   QStringLiteral("&Quit"), this);
        m_quitAction->setShortcut(QKeySequence::Quit);
        connect(m_quitAction, &QAction::triggered, qApp, &QCoreApplication::quit);
        actionCollection()->addAction(QStringLiteral("quit_application"), m_quitAction);
        actionCollection()->setDefaultShortcut(m_quitAction, QKeySequence::Quit);

        m_preferencesAction =
            KStandardAction::preferences(this, &MainWindow::openPreferences, actionCollection());
        m_preferencesAction->setIcon(
            thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/settings.svg")));

        m_contactsAction = new QAction(QIcon::fromTheme(QStringLiteral("view-pim-contacts")),
                                       QStringLiteral("Contacts"), this);
        connect(m_contactsAction, &QAction::triggered, this, &MainWindow::openContacts);
        actionCollection()->addAction(QStringLiteral("open_contacts"), m_contactsAction);
        const auto contactAccounts = m_contactRepository.listAccounts();
        m_contactsAction->setEnabled(
            std::holds_alternative<std::vector<javelin::jmap::cache::ContactAccount>>(
                contactAccounts) &&
            !std::get<std::vector<javelin::jmap::cache::ContactAccount>>(contactAccounts).empty());

        m_calendarAction = new QAction(QIcon::fromTheme(QStringLiteral("view-calendar-month")),
                                       QStringLiteral("Calendar"), this);
        connect(m_calendarAction, &QAction::triggered, this, &MainWindow::openCalendar);
        actionCollection()->addAction(QStringLiteral("open_calendar"), m_calendarAction);
        const auto calendarAccounts = m_calendarService.accounts();
        m_calendarAction->setEnabled(
            std::holds_alternative<std::vector<javelin::jmap::cache::CalendarAccount>>(
                calendarAccounts) &&
            !std::get<std::vector<javelin::jmap::cache::CalendarAccount>>(calendarAccounts)
                 .empty());

        m_newMessageAction =
            new QAction(thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/new-mail.svg")),
                        QStringLiteral("&New Message"), this);
        m_newMessageAction->setShortcut(QKeySequence::New);
        connect(m_newMessageAction, &QAction::triggered, this, &MainWindow::composeNewMessage);
        actionCollection()->addAction(QStringLiteral("compose_new_message"), m_newMessageAction);
        actionCollection()->setDefaultShortcut(m_newMessageAction, QKeySequence::New);

        m_replyAction =
            new QAction(thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/reply.svg")),
                        QStringLiteral("&Reply"), this);
        m_replyAction->setShortcut(QKeySequence{Qt::Key_R});
        connect(m_replyAction, &QAction::triggered, this, &MainWindow::composeReply);
        actionCollection()->addAction(QStringLiteral("compose_reply"), m_replyAction);
        actionCollection()->setDefaultShortcut(m_replyAction, QKeySequence{Qt::Key_R});

        m_replyAllAction =
            new QAction(thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/reply-all.svg")),
                        QStringLiteral("Reply &All"), this);
        connect(m_replyAllAction, &QAction::triggered, this, &MainWindow::composeReplyAll);
        actionCollection()->addAction(QStringLiteral("compose_reply_all"), m_replyAllAction);

        m_forwardAction =
            new QAction(thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/forward.svg")),
                        QStringLiteral("&Forward"), this);
        connect(m_forwardAction, &QAction::triggered, this, &MainWindow::composeForward);
        actionCollection()->addAction(QStringLiteral("compose_forward"), m_forwardAction);

        m_editDraftAction =
            new QAction(thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/draft.svg")),
                        QStringLiteral("Edit &Draft"), this);
        connect(m_editDraftAction, &QAction::triggered, this, &MainWindow::editSelectedDraft);
        actionCollection()->addAction(QStringLiteral("compose_edit_draft"), m_editDraftAction);

        m_archiveAction =
            new QAction(thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/archive.svg")),
                        QStringLiteral("&Archive"), this);
        m_archiveAction->setShortcut(QKeySequence{Qt::Key_A});
        connect(m_archiveAction, &QAction::triggered, this, &MainWindow::archiveSelectedEmail);
        actionCollection()->addAction(QStringLiteral("archive_email"), m_archiveAction);
        actionCollection()->setDefaultShortcut(m_archiveAction, QKeySequence{Qt::Key_A});

        m_markUnreadAction =
            new QAction(thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/unread.svg")),
                        QStringLiteral("Mark &Unread"), this);
        connect(m_markUnreadAction, &QAction::triggered, this,
                &MainWindow::markSelectedEmailUnread);
        actionCollection()->addAction(QStringLiteral("mark_email_unread"), m_markUnreadAction);

        m_deleteAction =
            new QAction(thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/delete.svg")),
                        QStringLiteral("&Delete"), this);
        m_deleteAction->setShortcut(QKeySequence::Delete);
        connect(m_deleteAction, &QAction::triggered, this, &MainWindow::deleteSelectedEmail);
        actionCollection()->addAction(QStringLiteral("delete_email"), m_deleteAction);
        actionCollection()->setDefaultShortcut(m_deleteAction, QKeySequence::Delete);

        m_permanentDeleteAction = new QAction(QStringLiteral("Delete Permanently"), this);
        connect(m_permanentDeleteAction, &QAction::triggered, this,
                &MainWindow::permanentlyDeleteSelectedEmail);
        actionCollection()->addAction(QStringLiteral("permanently_delete_email"),
                                      m_permanentDeleteAction);
        actionCollection()->setDefaultShortcut(m_permanentDeleteAction,
                                               QKeySequence{Qt::SHIFT | Qt::Key_Delete});

        m_moveAction = new QAction(QIcon::fromTheme(QStringLiteral("mail-move")),
                                   QStringLiteral("&Move to…"), this);
        connect(m_moveAction, &QAction::triggered, this, &MainWindow::showMoveMenu);
        actionCollection()->addAction(QStringLiteral("move_email"), m_moveAction);

        m_copyAction = new QAction(QIcon::fromTheme(QStringLiteral("edit-copy")),
                                   QStringLiteral("&Copy to…"), this);
        connect(m_copyAction, &QAction::triggered, this, &MainWindow::showCopyMenu);
        actionCollection()->addAction(QStringLiteral("copy_email"), m_copyAction);

        m_viewSourceAction = new QAction(QIcon::fromTheme(QStringLiteral("document-open")),
                                         QStringLiteral("View &Source"), this);
        connect(m_viewSourceAction, &QAction::triggered, this,
                &MainWindow::viewSelectedMessageSource);
        actionCollection()->addAction(QStringLiteral("view_message_source"), m_viewSourceAction);

        m_advancedSearchAction =
            new QAction(thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/search.svg")),
                        QStringLiteral("Advanced Search"), this);
        connect(m_advancedSearchAction, &QAction::triggered, this, &MainWindow::showAdvancedSearch);
        actionCollection()->addAction(QStringLiteral("advanced_search"), m_advancedSearchAction);

        auto* logAction = new QAction(QIcon::fromTheme(QStringLiteral("view-list-text")),
                                      QStringLiteral("Application Log"), this);
        connect(logAction, &QAction::triggered, this,
                [this]
                {
                    auto* dialog = new javelin::gui::logging::LogViewerDialog(this);
                    dialog->setAttribute(Qt::WA_DeleteOnClose);
                    dialog->show();
                });
        actionCollection()->addAction(QStringLiteral("open_application_log"), logAction);
    }

    void MainWindow::setupUi()
    {
        setWindowTitle(QStringLiteral("Javelin Mail"));

        m_mailboxModel = new javelin::gui::mailboxes::MailboxTreeModel(m_accountRepository,
                                                                       m_queryService, this);
        m_messageModel = new javelin::gui::messages::MessageListModel(m_queryService, this);

        m_mailboxSearchEdit = new QLineEdit(this);
        m_mailboxSearchEdit->setClearButtonEnabled(true);
        m_mailboxSearchEdit->setPlaceholderText(
            QStringLiteral("Search this account on the server"));

        m_tabBar = new QTabBar(this);
        m_tabBar->setDocumentMode(true);
        m_tabBar->setTabsClosable(true);
        m_tabBar->setExpanding(false);
        m_tabBar->setElideMode(Qt::ElideRight);
        m_tabBar->setUsesScrollButtons(true);
        m_tabBar->setStyleSheet(
            QStringLiteral("QTabBar::tab { max-width: 220px; min-width: 120px; }"));
        m_tabBar->hide();

        m_mailboxView = new QTreeView(this);
        m_mailboxView->setModel(m_mailboxModel);
        m_mailboxView->setItemDelegate(
            new javelin::gui::mailboxes::MailboxTreeDelegate(m_mailboxView));
        m_mailboxView->setIconSize(QSize{20, 20});
        m_mailboxView->setHeaderHidden(true);
        m_mailboxView->setExpandsOnDoubleClick(false);
        m_mailboxView->expandAll();
        m_mailboxView->setContextMenuPolicy(Qt::CustomContextMenu);
        m_mailboxView->setAcceptDrops(true);
        m_mailboxView->setDropIndicatorShown(true);
        m_mailboxView->setDragDropMode(QAbstractItemView::DropOnly);

        m_mailboxPane = new QWidget(this);
        auto* mailboxLayout = new QVBoxLayout(m_mailboxPane);
        mailboxLayout->setContentsMargins(0, 0, 0, 0);
        mailboxLayout->setSpacing(6);
        mailboxLayout->addWidget(m_mailboxSearchEdit);
        mailboxLayout->addWidget(m_mailboxView);

        m_messageView = new CompactDragListView(this);
        m_messageView->setModel(m_messageModel);
        auto* messageListDelegate = new javelin::gui::messages::MessageListDelegate(m_messageView);
        m_messageView->setItemDelegate(messageListDelegate);
        m_messageView->setSpacing(6);
        m_messageView->setFrameShape(QFrame::NoFrame);
        m_messageView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        m_messageView->setSelectionMode(QAbstractItemView::ExtendedSelection);
        m_messageView->setDragEnabled(true);
        m_messageView->setDragDropMode(QAbstractItemView::DragOnly);
        m_messageView->setDefaultDropAction(Qt::MoveAction);
        m_messageView->setStyleSheet(QStringLiteral("QListView { border: none; padding: 3px; }"));
        m_messageView->setMouseTracking(true);
        m_messageView->viewport()->setMouseTracking(true);
        m_messageView->installEventFilter(this);
        m_messageView->viewport()->installEventFilter(this);

        connect(m_mailboxModel, &javelin::gui::mailboxes::MailboxTreeModel::emailsDropped, this,
                [this](const QString& sourceAccountId, const QString& destinationAccountId,
                       const QString& destinationMailboxId, const QStringList& emailIds)
                {
                    Q_UNUSED(emailIds);
                    const auto sourceAccount = activeAccountId();
                    const auto sourceMailboxId = activeMailboxId();
                    if (!sourceAccount.has_value() || !sourceMailboxId.has_value() ||
                        sourceAccountId.toStdString() != *sourceAccount ||
                        sourceAccountId != destinationAccountId)
                    {
                        m_statusBar->showMessage(
                            QStringLiteral("Messages can only be moved within their account."),
                            5000);
                        return;
                    }

                    auto idsResult = selectedEmailIdsForMailboxAction(*sourceAccount);
                    if (const auto* error = std::get_if<QString>(&idsResult))
                    {
                        m_statusBar->showMessage(*error, 10000);
                        return;
                    }
                    auto ids = std::get<std::vector<std::string>>(std::move(idsResult));
                    queueMoveEmails(sourceAccountId.toStdString(), *sourceMailboxId,
                                    destinationMailboxId.toStdString(), std::move(ids),
                                    QStringLiteral("Queued move."));
                });

        auto* messagePane = new QWidget(this);
        auto* messageLayout = new QVBoxLayout(messagePane);
        messageLayout->setContentsMargins(0, 0, 0, 0);
        messageLayout->setSpacing(8);
        auto* messageHeader = new QWidget(messagePane);
        auto* messageHeaderLayout = new QHBoxLayout(messageHeader);
        messageHeaderLayout->setContentsMargins(8, 3, 8, 3);
        messageHeaderLayout->setSpacing(8);
        m_messageListTitleLabel = new QLabel(messageHeader);
        m_messageListMetaLabel = new QLabel(messageHeader);
        m_messageQuickFilterButton = new QToolButton(messageHeader);
        m_messageQuickFilterButton->setIcon(
            javelin::gui::themedSvgIcon(QStringLiteral(":/icons/thunderbird-icons/filter.svg"),
                                        palette().color(QPalette::Text)));
        m_messageQuickFilterButton->setText(QStringLiteral("Quick Filter"));
        m_messageQuickFilterButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        m_messageQuickFilterButton->setEnabled(false);
        m_messageSortButton = new QToolButton(messageHeader);
        m_messageSortButton->setIcon(javelin::gui::themedSvgIcon(
            QStringLiteral(":/icons/thunderbird-icons/display-options.svg"),
            palette().color(QPalette::Text)));
        m_messageSortButton->setToolTip(QStringLiteral("Sort messages"));
        m_previousPageButton = new QToolButton(messageHeader);
        m_previousPageButton->setIcon(
            javelin::gui::themedSvgIcon(QStringLiteral(":/icons/thunderbird-icons/nav-left.svg"),
                                        palette().color(QPalette::Text)));
        m_previousPageButton->setToolTip(QStringLiteral("Previous page"));
        m_nextPageButton = new QToolButton(messageHeader);
        m_nextPageButton->setIcon(
            javelin::gui::themedSvgIcon(QStringLiteral(":/icons/thunderbird-icons/nav-right.svg"),
                                        palette().color(QPalette::Text)));
        m_nextPageButton->setToolTip(QStringLiteral("Next page"));
        m_messagePageLabel = new QLabel(messageHeader);
        auto titleFont = m_messageListTitleLabel->font();
        titleFont.setPointSize(titleFont.pointSize() + 4);
        titleFont.setBold(true);
        m_messageListTitleLabel->setFont(titleFont);
        messageHeaderLayout->addWidget(m_messageListTitleLabel);
        messageHeaderLayout->addWidget(m_messageListMetaLabel);
        messageHeaderLayout->addStretch(1);
        messageHeaderLayout->addWidget(m_previousPageButton);
        messageHeaderLayout->addWidget(m_messagePageLabel);
        messageHeaderLayout->addWidget(m_nextPageButton);
        messageHeaderLayout->addWidget(m_messageSortButton);
        messageHeaderLayout->addWidget(m_messageQuickFilterButton);
        m_messageEmptyState = new QLabel(
            QStringLiteral("No messages are available for the selected mailbox yet."), messagePane);
        m_messageEmptyState->setWordWrap(true);
        messageLayout->addWidget(messageHeader);
        messageLayout->addWidget(m_messageEmptyState);
        messageLayout->addWidget(m_messageView, 1);

        m_messageViewContainer = new javelin::gui::messageview::MessageViewContainer(
            m_translationCacheRepository, m_contactIdentityLookup, this);
        connect(m_messageViewContainer,
                &javelin::gui::messageview::MessageViewContainer::saveAttachmentRequested, this,
                [this](const QString& accountId, const QString& emailId, const QString& partId)
                {
                    saveAttachment(accountId.toStdString(), emailId.toStdString(),
                                   partId.toStdString());
                });
        connect(m_messageViewContainer,
                &javelin::gui::messageview::MessageViewContainer::saveAllAttachmentsRequested, this,
                [this](const QString& accountId, const QString& emailId)
                { saveAllAttachments(accountId.toStdString(), emailId.toStdString()); });
        connect(m_messageViewContainer,
                &javelin::gui::messageview::MessageViewContainer::openAttachmentRequested, this,
                [this](const QString& accountId, const QString& emailId, const QString& partId)
                {
                    openAttachment(accountId.toStdString(), emailId.toStdString(),
                                   partId.toStdString());
                });
        connect(m_messageViewContainer,
                &javelin::gui::messageview::MessageViewContainer::viewSourceRequested, this,
                &MainWindow::viewSelectedMessageSource);
        connect(m_messageViewContainer,
                &javelin::gui::messageview::MessageViewContainer::messageActivated, this,
                &MainWindow::selectMessageAlone);
        connect(m_messageViewContainer,
                &javelin::gui::messageview::MessageViewContainer::hoveredLinkChanged, this,
                [this](const QString& url) { m_statusBar->setOverlayMessage(url); });

        m_messageView->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_messageView, &QListView::customContextMenuRequested, this,
                &MainWindow::showMessageListContextMenu);
        connect(m_mailboxView, &QTreeView::customContextMenuRequested, this,
                &MainWindow::showMailboxContextMenu);
        connect(
            messageListDelegate,
            &javelin::gui::messages::MessageListDelegate::threadExpansionToggled, this,
            [this](const QModelIndex& index)
            {
                if (!index.isValid())
                {
                    return;
                }

                const auto threadId =
                    index.data(javelin::gui::messages::MessageListModel::ThreadIdRole).toString();
                if (threadId.isEmpty())
                {
                    return;
                }

                const bool isExpanded =
                    index.data(javelin::gui::messages::MessageListModel::IsExpandedRole).toBool();
                if (isExpanded && currentThreadId(*m_messageView) ==
                                      std::optional<std::string>{threadId.toStdString()})
                {
                    m_messageView->setCurrentIndex(index);
                }

                static_cast<void>(
                    m_messageModel->setThreadExpanded(threadId.toStdString(), !isExpanded));
            });
        connect(messageListDelegate,
                &javelin::gui::messages::MessageListDelegate::attachmentButtonClicked, this,
                [this](const QModelIndex& index)
                {
                    if (!index.isValid())
                    {
                        return;
                    }
                    m_messageView->setCurrentIndex(index);
                    refreshSelectionFromModels();
                });
        connect(messageListDelegate, &javelin::gui::messages::MessageListDelegate::flaggedToggled,
                this, &MainWindow::toggleMessageFlagged);

        m_mainSplitter = new QSplitter(Qt::Horizontal, this);
        m_mainSplitter->addWidget(m_mailboxPane);
        m_mainSplitter->addWidget(messagePane);
        m_mainSplitter->addWidget(m_messageViewContainer);
        m_mainSplitter->setStretchFactor(0, 1);
        m_mainSplitter->setStretchFactor(1, 2);
        m_mainSplitter->setStretchFactor(2, 3);
        m_mainSplitter->setSizes({240, 420, 780});

        auto* centralContainer = new QWidget(this);
        auto* centralLayout = new QVBoxLayout(centralContainer);
        centralLayout->setContentsMargins(0, 0, 0, 0);
        centralLayout->setSpacing(0);
        m_contentStack = new QStackedWidget(centralContainer);
        m_contentStack->addWidget(m_mainSplitter);
        centralLayout->addWidget(m_tabBar);
        centralLayout->addWidget(m_contentStack);

        setCentralWidget(centralContainer);
        m_statusBar->showMessage(m_mailService.statusSummary());
        updateEmptyStates();
        updateMessageListHeader();
    }

    void MainWindow::connectSelection()
    {
        connect(m_tabBar, &QTabBar::currentChanged, this,
                [this](const int index)
                {
                    if (m_syncingNavigation)
                    {
                        return;
                    }

                    activateTab(index, false);
                });
        connect(m_tabBar, &QTabBar::tabCloseRequested, this, &MainWindow::closeTab);
        connect(m_previousPageButton, &QToolButton::clicked, this, &MainWindow::goToPreviousPage);
        connect(m_nextPageButton, &QToolButton::clicked, this, &MainWindow::goToNextPage);
        connect(m_messageSortButton, &QToolButton::clicked, this, &MainWindow::showSortMenu);
        connect(m_mailboxSearchEdit, &QLineEdit::returnPressed, this,
                [this] { executeSearch(m_mailboxSearchEdit->text()); });
        connect(m_mailboxSearchEdit, &QLineEdit::textChanged, this,
                [this](const QString& text)
                {
                    if (text.trimmed().isEmpty())
                    {
                        clearSearch();
                    }
                });

        connect(m_mailboxView->selectionModel(), &QItemSelectionModel::currentChanged, this,
                [this](const QModelIndex& current, const QModelIndex&)
                {
                    if (m_syncingNavigation)
                    {
                        return;
                    }

                    if (!current.isValid())
                    {
                        return;
                    }

                    activateMailboxSelection(false);
                });
        connect(m_mailboxView, &QTreeView::doubleClicked, this,
                [this](const QModelIndex& index)
                {
                    if (!index.isValid())
                    {
                        return;
                    }

                    openMailboxSelectionInTab(false);
                });

        connect(
            m_messageView->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex& current, const QModelIndex&)
            {
                const auto accountId = activeAccountId();
                const auto mailboxId = activeMailboxId();
                const bool allowSearchSelection = activeTabIsSearch() && accountId.has_value();
                if (!accountId.has_value() || (!mailboxId.has_value() && !allowSearchSelection))
                {
                    m_messageViewContainer->setSelection(m_messageViewService, accountId, mailboxId,
                                                         std::nullopt);
                    updateMessageActions();
                    return;
                }

                if (!current.isValid())
                {
                    m_messageViewContainer->setSelection(m_messageViewService, accountId, mailboxId,
                                                         std::nullopt);
                    updateMessageActions();
                    return;
                }

                const auto emailId =
                    current.data(javelin::gui::messages::MessageListModel::EmailIdRole).toString();
                const auto threadId =
                    current.data(javelin::gui::messages::MessageListModel::ThreadIdRole).toString();
                const bool isUnread = indexIsUnread(current);
                if (!threadId.isEmpty())
                {
                    QTimer::singleShot(0, this,
                                       [this, threadId = threadId.toStdString()]
                                       {
                                           if (currentThreadId(*m_messageView) ==
                                               std::optional<std::string>{threadId})
                                           {
                                               static_cast<void>(m_messageModel->setThreadExpanded(
                                                   threadId, true));
                                           }
                                       });
                }
                m_messageViewContainer->setSelection(
                    m_messageViewService, accountId, mailboxId,
                    emailId.isEmpty() ? std::optional<std::string>{std::nullopt}
                                      : std::optional<std::string>{emailId.toStdString()});
                updateEmptyStates();
                updateMessageListHeader();
                updateMessageActions();
                if (!emailId.isEmpty())
                {
                    if (!m_messageViewContainer->hasReadableBody())
                    {
                        m_messageViewContainer->setLoadingState(
                            true, QStringLiteral("Checking your saved copy, then downloading the "
                                                 "message if needed."));
                    }
                    refreshSelectedMessageContent(*accountId, emailId.toStdString());
                    if (isUnread)
                    {
                        queueMarkEmailRead(*accountId, emailId.toStdString());
                    }
                }

                syncActiveTabSelectionFromViews();
            });
        connect(m_messageView->selectionModel(), &QItemSelectionModel::selectionChanged, this,
                [this](const QItemSelection&, const QItemSelection&)
                { refreshSelectionFromModels(); });
    }

    void MainWindow::openContacts()
    {
        for (std::size_t index = 0; index < m_tabs.size(); ++index)
        {
            if (std::holds_alternative<ContactsTabState>(m_tabs[index].content))
            {
                m_activeTabIndex = static_cast<int>(index);
                updateTabBar();
                activateTab(*m_activeTabIndex, false);
                return;
            }
        }

        const auto result = m_contactRepository.listAccounts();
        const auto* accounts =
            std::get_if<std::vector<javelin::jmap::cache::ContactAccount>>(&result);
        if (accounts == nullptr || accounts->empty())
        {
            m_statusBar->showMessage(
                QStringLiteral("The configured server does not support JMAP Contacts."), 10000);
            return;
        }
        const auto active = activeAccountId();
        auto selected = accounts->begin();
        if (active.has_value())
        {
            const auto match = std::ranges::find(*accounts, *active,
                                                 &javelin::jmap::cache::ContactAccount::accountId);
            if (match != accounts->end())
            {
                selected = match;
            }
        }
        const auto settings = javelin::gui::settings::PreferencesDialog::loadSettingsForAccount(
            QString::fromStdString(selected->accountId));
        auto* widget = new javelin::gui::contacts::ContactsManagerWidget(
            m_contactRepository, m_mailService, selected->ownerAccountId, m_contentStack);
        connect(widget, &javelin::gui::contacts::ContactsManagerWidget::statusMessageRequested,
                m_statusBar, &LayeredStatusBar::showMessage);
        connect(widget, &javelin::gui::contacts::ContactsManagerWidget::userInterventionRequired,
                this, &MainWindow::presentUserInterventionError);
        connect(widget, &javelin::gui::contacts::ContactsManagerWidget::composeMailRequested, this,
                [this](const QString& accountId, const QString& name, const QString& email)
                {
                    openComposeForRequest({
                        .accountId = accountId.toStdString(),
                        .mode = javelin::jmap::submission::ComposeMode::NewMessage,
                        .referenceEmailId = std::nullopt,
                        .draftEmailId = std::nullopt,
                        .initialTo = {{.name = name.isEmpty()
                                                   ? std::nullopt
                                                   : std::optional<std::string>{name.toStdString()},
                                       .email = email.toStdString()}},
                    });
                });
        m_contentStack->addWidget(widget);
        m_tabs.push_back(TabState{.content = ContactsTabState{.accountId = selected->ownerAccountId,
                                                              .title = QStringLiteral("Contacts"),
                                                              .widget = widget,
                                                              .page = {},
                                                              .selection = {}}});
        m_activeTabIndex = static_cast<int>(m_tabs.size() - 1);
        updateTabBar();
        activateTab(*m_activeTabIndex, false);
        widget->requestRefresh();
    }

    void MainWindow::openCalendar()
    {
        for (std::size_t index = 0; index < m_tabs.size(); ++index)
        {
            if (std::holds_alternative<CalendarTabState>(m_tabs[index].content))
            {
                m_activeTabIndex = static_cast<int>(index);
                updateTabBar();
                activateTab(*m_activeTabIndex, false);
                return;
            }
        }

        const auto accountsResult = m_calendarService.accounts();
        const auto* accounts =
            std::get_if<std::vector<javelin::jmap::cache::CalendarAccount>>(&accountsResult);
        if (accounts == nullptr || accounts->empty())
        {
            m_statusBar->showMessage(
                QStringLiteral("The configured server does not support JMAP Calendars draft-26."),
                10000);
            return;
        }

        auto* widget = new javelin::gui::calendar::MonthCalendarWidget(m_contentStack);
        const auto loadVisible =
            [this, widget, accounts = *accounts](const QDate& start, const QDate& end)
        {
            std::vector<javelin::gui::calendar::MonthEvent> displayEvents;
            std::vector<javelin::gui::calendar::CalendarDisplay> calendarDisplays;
            std::unordered_map<std::string, QColor> calendarColors;
            const javelin::jmap::calendar::VisibleInterval interval{
                .start = {.value = start.toString(Qt::ISODate).toStdString() + "T00:00:00"},
                .end = {.value = end.toString(Qt::ISODate).toStdString() + "T00:00:00"}};
            const javelin::jmap::calendar::TimeZoneId timeZone{
                .value = QTimeZone::systemTimeZoneId().toStdString()};
            for (const auto& account : accounts)
            {
                const auto listed = m_calendarService.calendars(account.accountId);
                const auto* calendars =
                    std::get_if<std::vector<javelin::jmap::calendar::Calendar>>(&listed);
                if (calendars != nullptr)
                {
                    for (const auto& calendar : *calendars)
                    {
                        const auto key = account.accountId + '\n' + calendar.id;
                        const auto color = calendar.color
                                               ? QColor{QString::fromStdString(*calendar.color)}
                                               : QColor{QStringLiteral("#4f7cac")};
                        calendarColors.emplace(key, color);
                        calendarDisplays.push_back({.id = key,
                                                    .name = QStringLiteral("%1 — %2").arg(
                                                        QString::fromStdString(calendar.name),
                                                        QString::fromStdString(account.name)),
                                                    .color = color,
                                                    .visible = calendar.isVisible});
                    }
                }
                const auto loaded =
                    m_calendarService.loadCached(account.accountId, interval, timeZone);
                const auto* window =
                    std::get_if<std::optional<javelin::jmap::cache::CalendarWindow>>(&loaded);
                if (window == nullptr || !window->has_value())
                    continue;
                std::unordered_map<std::string, const javelin::jmap::calendar::CalendarEvent*>
                    events;
                for (const auto& event : window->value().events)
                    events.emplace(event.id, &event);
                for (const auto& occurrence : window->value().occurrences)
                {
                    const auto event = events.find(occurrence.eventId);
                    if (event == events.end())
                        continue;
                    const auto calendarId = std::ranges::find_if(
                        event->second->calendarIds, [](const auto& item) { return item.second; });
                    auto startTime = QDateTime::fromString(
                        QString::fromStdString(occurrence.localStart.value), Qt::ISODate);
                    auto endTime = QDateTime::fromString(
                        QString::fromStdString(occurrence.localEnd.value), Qt::ISODate);
                    if (!endTime.isValid() || endTime <= startTime)
                        endTime = startTime.addSecs(3600);
                    const auto displayCalendarId =
                        calendarId == event->second->calendarIds.end()
                            ? account.accountId + '\n'
                            : account.accountId + '\n' + calendarId->first;
                    const auto color = calendarColors.find(displayCalendarId);
                    displayEvents.push_back({.accountId = account.accountId,
                                             .calendarId = displayCalendarId,
                                             .eventId = event->second->id,
                                             .title = QString::fromStdString(event->second->title),
                                             .color = color == calendarColors.end()
                                                          ? QColor{QStringLiteral("#4f7cac")}
                                                          : color->second,
                                             .start = startTime,
                                             .end = endTime,
                                             .allDay = occurrence.allDay,
                                             .recurring = occurrence.recurrenceId.has_value()});
                }
            }
            widget->setCalendars(std::move(calendarDisplays));
            widget->setEvents(std::move(displayEvents));
        };
        connect(widget, &javelin::gui::calendar::MonthCalendarWidget::visibleIntervalChanged,
                widget, loadVisible);
        loadVisible(widget->visibleStart(), widget->visibleEnd());
        const auto refreshVisible =
            [this, widget, accounts = *accounts, loadVisible](const QDate& start, const QDate& end)
        {
            const javelin::jmap::calendar::VisibleInterval interval{
                .start = {.value = start.toString(Qt::ISODate).toStdString() + "T00:00:00"},
                .end = {.value = end.toString(Qt::ISODate).toStdString() + "T00:00:00"}};
            const javelin::jmap::calendar::TimeZoneId timeZone{
                .value = QTimeZone::systemTimeZoneId().toStdString()};
            std::unordered_set<std::string> owners;
            for (const auto& account : accounts)
            {
                if (!owners.insert(account.ownerAccountId).second)
                    continue;
                auto task =
                    m_mailService.requestCalendarRange(account.ownerAccountId, interval, timeZone);
                QCoro::connect(
                    std::move(task), widget,
                    [this, start, end,
                     loadVisible](javelin::jmap::calendar::CalendarRefreshResult result)
                    {
                        if (const auto* error =
                                std::get_if<javelin::jmap::calendar::CalendarServiceError>(&result))
                        {
                            m_statusBar->showMessage(error->message, 10000);
                            return;
                        }
                        loadVisible(start, end);
                    });
            }
        };
        connect(widget, &javelin::gui::calendar::MonthCalendarWidget::visibleIntervalChanged,
                widget, refreshVisible);
        connect(widget, &javelin::gui::calendar::MonthCalendarWidget::emptyTimeActivated, widget,
                [this, widget, accounts = *accounts, refreshVisible](const QDate& date)
                {
                    for (const auto& account : accounts)
                    {
                        const auto calendarsResult = m_calendarService.calendars(account.accountId);
                        const auto* calendars =
                            std::get_if<std::vector<javelin::jmap::calendar::Calendar>>(
                                &calendarsResult);
                        if (calendars == nullptr)
                            continue;
                        auto destination = std::ranges::find_if(
                            *calendars,
                            [](const auto& item)
                            {
                                return item.isDefault &&
                                       (item.myRights.mayWriteAll || item.myRights.mayWriteOwn);
                            });
                        if (destination == calendars->end())
                            destination = std::ranges::find_if(
                                *calendars, [](const auto& item)
                                { return item.myRights.mayWriteAll || item.myRights.mayWriteOwn; });
                        if (destination == calendars->end())
                            continue;

                        auto* dialog = new javelin::gui::calendar::EventDialog(*calendars, widget);
                        dialog->setAttribute(Qt::WA_DeleteOnClose, false);
                        dialog->setEvent(javelin::jmap::calendar::CalendarEvent{
                            .accountId = account.accountId,
                            .id = {},
                            .baseEventId = std::nullopt,
                            .recurrenceId = std::nullopt,
                            .uid = {},
                            .calendarIds = {{destination->id, true}},
                            .title = {},
                            .description = std::nullopt,
                            .location = std::nullopt,
                            .start = {.value = QDateTime{date, QTime{9, 0}}
                                                   .toString(Qt::ISODate)
                                                   .toStdString()},
                            .duration = {.value = "PT1H"},
                            .timeZone = {.value = QTimeZone::systemTimeZoneId().toStdString()},
                            .showWithoutTime = false,
                            .isDraft = false,
                            .isOrigin = true,
                            .utcStart = std::nullopt,
                            .utcEnd = std::nullopt,
                            .recurrenceRule = std::nullopt,
                            .recurrenceOverrides = {},
                            .attendees = {}});
                        if (dialog->exec() != QDialog::Accepted)
                        {
                            dialog->deleteLater();
                            return;
                        }
                        auto task = m_mailService.createCalendarEvent(
                            account.ownerAccountId, {.accountId = account.accountId,
                                                     .event = dialog->eventDocument(),
                                                     .ifInState = std::nullopt});
                        QCoro::connect(
                            std::move(task), dialog,
                            [dialog, widget,
                             refreshVisible](javelin::jmap::calendar::CalendarMutationResult result)
                            {
                                if (const auto* error =
                                        std::get_if<javelin::jmap::calendar::CalendarServiceError>(
                                            &result))
                                {
                                    dialog->showMutationError(error->message);
                                    dialog->show();
                                    return;
                                }
                                dialog->deleteLater();
                                refreshVisible(widget->visibleStart(), widget->visibleEnd());
                            });
                        return;
                    }
                    m_statusBar->showMessage(QStringLiteral("No writable calendar is available."),
                                             5000);
                });
        connect(
            widget, &javelin::gui::calendar::MonthCalendarWidget::eventActivated, widget,
            [this, widget, accounts = *accounts, refreshVisible](const QString& accountId,
                                                                 const QString& eventId)
            {
                const auto account =
                    std::ranges::find(accounts, accountId.toStdString(),
                                      &javelin::jmap::cache::CalendarAccount::accountId);
                if (account == accounts.end())
                    return;
                const javelin::jmap::calendar::VisibleInterval interval{
                    .start = {.value = widget->visibleStart().toString(Qt::ISODate).toStdString() +
                                       "T00:00:00"},
                    .end = {.value = widget->visibleEnd().toString(Qt::ISODate).toStdString() +
                                     "T00:00:00"}};
                const javelin::jmap::calendar::TimeZoneId timeZone{
                    .value = QTimeZone::systemTimeZoneId().toStdString()};
                const auto loaded =
                    m_calendarService.loadCached(account->accountId, interval, timeZone);
                const auto* window =
                    std::get_if<std::optional<javelin::jmap::cache::CalendarWindow>>(&loaded);
                if (window == nullptr || !window->has_value())
                    return;
                const auto event = std::ranges::find(window->value().events, eventId.toStdString(),
                                                     &javelin::jmap::calendar::CalendarEvent::id);
                if (event == window->value().events.end())
                    return;
                const auto calendarsResult = m_calendarService.calendars(account->accountId);
                const auto* calendars =
                    std::get_if<std::vector<javelin::jmap::calendar::Calendar>>(&calendarsResult);
                if (calendars == nullptr)
                    return;
                auto* dialog = new javelin::gui::calendar::EventDialog(*calendars, widget);
                dialog->setAttribute(Qt::WA_DeleteOnClose, false);
                dialog->setEvent(*event);
                const auto originalCalendarIds = event->calendarIds;
                const auto dialogResult = dialog->exec();
                if (dialogResult == QDialog::Rejected)
                {
                    dialog->deleteLater();
                    return;
                }
                const auto editedEvent = dialog->eventDocument();
                auto task =
                    dialogResult == javelin::gui::calendar::EventDialog::DeleteRequested
                        ? m_mailService.deleteCalendarEvent(
                              account->ownerAccountId,
                              {.accountId = account->accountId,
                               .eventId = editedEvent.id,
                               .calendarIds =
                                   [&originalCalendarIds]
                               {
                                   std::vector<std::string> ids;
                                   for (const auto& [calendarId, present] : originalCalendarIds)
                                   {
                                       if (present)
                                       {
                                           ids.push_back(calendarId);
                                       }
                                   }
                                   return ids;
                               }(),
                               .ifInState = std::nullopt})
                        : m_mailService.updateCalendarEvent(account->ownerAccountId,
                                                            {.accountId = account->accountId,
                                                             .event = editedEvent,
                                                             .ifInState = std::nullopt});
                QCoro::connect(
                    std::move(task), dialog,
                    [dialog, widget,
                     refreshVisible](javelin::jmap::calendar::CalendarMutationResult result)
                    {
                        if (const auto* error =
                                std::get_if<javelin::jmap::calendar::CalendarServiceError>(&result))
                        {
                            dialog->showMutationError(error->message);
                            dialog->show();
                            return;
                        }
                        dialog->deleteLater();
                        refreshVisible(widget->visibleStart(), widget->visibleEnd());
                    });
            });
        refreshVisible(widget->visibleStart(), widget->visibleEnd());
        m_contentStack->addWidget(widget);
        m_tabs.push_back(
            TabState{.content = CalendarTabState{.accountId = accounts->front().ownerAccountId,
                                                 .title = QStringLiteral("Calendar"),
                                                 .widget = widget,
                                                 .page = {},
                                                 .selection = {}}});
        m_activeTabIndex = static_cast<int>(m_tabs.size() - 1);
        updateTabBar();
        activateTab(*m_activeTabIndex, false);
    }

    void MainWindow::composeNewMessage()
    {
        const auto accountId =
            activeAccountId().has_value() ? activeAccountId() : currentAccountId(*m_mailboxView);
        if (!accountId.has_value())
        {
            m_statusBar->showMessage(
                QStringLiteral("Select an account before composing a message."), 5000);
            return;
        }

        openComposeForRequest({
            .accountId = *accountId,
            .mode = javelin::jmap::submission::ComposeMode::NewMessage,
            .referenceEmailId = std::nullopt,
            .draftEmailId = std::nullopt,
            .initialTo = {},
        });
    }

    void MainWindow::composeReply()
    {
        const auto accountId = activeAccountId();
        const auto emailId = currentEmailId(*m_messageView);
        if (!accountId.has_value() || !emailId.has_value())
        {
            m_statusBar->showMessage(QStringLiteral("Select a message to reply to."), 5000);
            return;
        }

        openComposeForRequest({
            .accountId = *accountId,
            .mode = javelin::jmap::submission::ComposeMode::Reply,
            .referenceEmailId = *emailId,
            .draftEmailId = std::nullopt,
            .initialTo = {},
        });
    }

    void MainWindow::composeReplyAll()
    {
        const auto accountId = activeAccountId();
        const auto emailId = currentEmailId(*m_messageView);
        if (!accountId.has_value() || !emailId.has_value())
        {
            m_statusBar->showMessage(QStringLiteral("Select a message to reply to."), 5000);
            return;
        }

        openComposeForRequest({
            .accountId = *accountId,
            .mode = javelin::jmap::submission::ComposeMode::ReplyAll,
            .referenceEmailId = *emailId,
            .draftEmailId = std::nullopt,
            .initialTo = {},
        });
    }

    void MainWindow::composeForward()
    {
        const auto accountId = activeAccountId();
        const auto emailId = currentEmailId(*m_messageView);
        if (!accountId.has_value() || !emailId.has_value())
        {
            m_statusBar->showMessage(QStringLiteral("Select a message to forward."), 5000);
            return;
        }

        openComposeForRequest({
            .accountId = *accountId,
            .mode = javelin::jmap::submission::ComposeMode::Forward,
            .referenceEmailId = *emailId,
            .draftEmailId = std::nullopt,
            .initialTo = {},
        });
    }

    void MainWindow::editSelectedDraft()
    {
        const auto accountId = activeAccountId();
        const auto emailId = currentEmailId(*m_messageView);
        if (!accountId.has_value() || !emailId.has_value())
        {
            m_statusBar->showMessage(QStringLiteral("Select a draft to edit."), 5000);
            return;
        }

        const auto draftsMailbox = findMailboxByRole(m_queryService, *accountId, "drafts");
        if (!activeMailboxId().has_value() || !draftsMailbox.has_value() ||
            draftsMailbox->id != *activeMailboxId())
        {
            m_statusBar->showMessage(QStringLiteral("Open a message from Drafts to edit it."),
                                     5000);
            return;
        }

        openComposeForRequest({
            .accountId = *accountId,
            .mode = javelin::jmap::submission::ComposeMode::EditDraft,
            .referenceEmailId = std::nullopt,
            .draftEmailId = *emailId,
            .initialTo = {},
        });
    }

    const MainWindow::TabState* MainWindow::activeTab() const
    {
        if (!m_activeTabIndex.has_value() || *m_activeTabIndex < 0 ||
            static_cast<std::size_t>(*m_activeTabIndex) >= m_tabs.size())
        {
            return nullptr;
        }

        return &m_tabs[static_cast<std::size_t>(*m_activeTabIndex)];
    }

    MainWindow::TabState* MainWindow::activeTab()
    {
        if (!m_activeTabIndex.has_value() || *m_activeTabIndex < 0 ||
            static_cast<std::size_t>(*m_activeTabIndex) >= m_tabs.size())
        {
            return nullptr;
        }

        return &m_tabs[static_cast<std::size_t>(*m_activeTabIndex)];
    }

    bool MainWindow::activeTabIsMailbox() const
    {
        const auto* tab = activeTab();
        return tab != nullptr && std::holds_alternative<MailboxTabState>(tab->content);
    }

    bool MainWindow::activeTabIsSearch() const
    {
        const auto* tab = activeTab();
        return tab != nullptr && std::holds_alternative<SearchTabState>(tab->content);
    }

    bool MainWindow::activeTabIsCompose() const
    {
        const auto* tab = activeTab();
        return tab != nullptr && std::holds_alternative<ComposeTabState>(tab->content);
    }

    bool MainWindow::activeTabIsContacts() const
    {
        const auto* tab = activeTab();
        return tab != nullptr && std::holds_alternative<ContactsTabState>(tab->content);
    }

    bool MainWindow::activeTabIsCalendar() const
    {
        const auto* tab = activeTab();
        return tab != nullptr && std::holds_alternative<CalendarTabState>(tab->content);
    }

    std::optional<std::string> MainWindow::activeAccountId() const
    {
        const auto* tab = activeTab();
        if (tab == nullptr)
        {
            return std::nullopt;
        }

        if (const auto* mailboxTab = std::get_if<MailboxTabState>(&tab->content))
        {
            return mailboxTab->accountId;
        }

        if (const auto* searchTab = std::get_if<SearchTabState>(&tab->content))
        {
            return searchTab->accountId;
        }

        if (const auto* contactsTab = std::get_if<ContactsTabState>(&tab->content))
        {
            return contactsTab->accountId;
        }

        if (const auto* calendarTab = std::get_if<CalendarTabState>(&tab->content))
        {
            return calendarTab->accountId;
        }

        return std::get<ComposeTabState>(tab->content).accountId;
    }

    std::optional<std::string> MainWindow::activeMailboxId() const
    {
        const auto* tab = activeTab();
        if (tab == nullptr)
        {
            return std::nullopt;
        }

        if (const auto* mailboxTab = std::get_if<MailboxTabState>(&tab->content))
        {
            return mailboxTab->mailboxId;
        }

        return std::nullopt;
    }

    QString MainWindow::titleForTab(const TabState& tab) const
    {
        QString title;
        std::string accountId;
        if (const auto* mailboxTab = std::get_if<MailboxTabState>(&tab.content))
        {
            title = mailboxTab->title;
            accountId = mailboxTab->accountId;
        }
        else if (const auto* searchTab = std::get_if<SearchTabState>(&tab.content))
        {
            title = searchTab->title;
            accountId = searchTab->accountId;
        }
        else if (const auto* contactsTab = std::get_if<ContactsTabState>(&tab.content))
        {
            title = contactsTab->title;
            accountId = contactsTab->accountId;
        }
        else if (const auto* calendarTab = std::get_if<CalendarTabState>(&tab.content))
        {
            title = calendarTab->title;
            accountId = calendarTab->accountId;
        }
        else
        {
            const auto& composeTab = std::get<ComposeTabState>(tab.content);
            title = composeTab.title;
            accountId = composeTab.accountId;
        }

        const auto settings = javelin::gui::settings::PreferencesDialog::loadSettingsForAccount(
            QString::fromStdString(accountId));
        auto accountName = settings.displayName;
        if (accountName.isEmpty())
        {
            const auto cached = m_accountRepository.listAll();
            if (const auto* accounts =
                    std::get_if<std::vector<javelin::jmap::cache::CachedAccount>>(&cached))
            {
                const auto account = std::ranges::find(
                    *accounts, accountId, &javelin::jmap::cache::CachedAccount::accountId);
                if (account != accounts->end())
                    accountName = QString::fromStdString(account->name);
            }
        }
        if (accountName.isEmpty())
            accountName = settings.loginEmail;
        return accountName.isEmpty() ? title : QStringLiteral("%1 - %2").arg(title, accountName);
    }

    QIcon MainWindow::iconForTab(const TabState& tab) const
    {
        const auto color = palette().color(QPalette::Text);
        if (const auto* mailboxTab = std::get_if<MailboxTabState>(&tab.content))
        {
            return javelin::gui::mailboxes::mailboxIcon(mailboxTab->role, color);
        }

        if (std::holds_alternative<SearchTabState>(tab.content))
        {
            return javelin::gui::themedSvgIcon(
                QStringLiteral(":/icons/thunderbird-icons/search.svg"), color);
        }

        if (std::holds_alternative<ContactsTabState>(tab.content))
        {
            return QIcon::fromTheme(QStringLiteral("view-pim-contacts"));
        }

        if (std::holds_alternative<CalendarTabState>(tab.content))
        {
            return QIcon::fromTheme(QStringLiteral("view-calendar-month"));
        }

        return javelin::gui::themedSvgIcon(QStringLiteral(":/icons/thunderbird-icons/new-mail.svg"),
                                           color);
    }

    void MainWindow::updateTabBar()
    {
        QSignalBlocker blocker{m_tabBar};
        if (m_tabBar->count() != static_cast<int>(m_tabs.size()))
        {
            while (m_tabBar->count() > 0)
            {
                m_tabBar->removeTab(0);
            }

            for (const auto& tab : m_tabs)
            {
                m_tabBar->addTab(iconForTab(tab), titleForTab(tab));
            }

            for (int index = 0; index < m_tabBar->count(); ++index)
            {
                m_tabBar->setTabButton(index, QTabBar::RightSide, nullptr);
            }
            if (m_tabBar->count() > 0)
            {
                for (int index = 0; index < m_tabBar->count(); ++index)
                {
                    const auto& content = m_tabs[static_cast<std::size_t>(index)].content;
                    const bool canClose = index != 0 ||
                                          std::holds_alternative<ComposeTabState>(content) ||
                                          std::holds_alternative<ContactsTabState>(content) ||
                                          std::holds_alternative<CalendarTabState>(content);
                    if (!canClose)
                    {
                        continue;
                    }
                    auto* closeButton = new QToolButton(m_tabBar);
                    closeButton->setAutoRaise(true);
                    closeButton->setText(QStringLiteral("x"));
                    connect(closeButton, &QToolButton::clicked, this,
                            [this, index] { closeTab(index); });
                    m_tabBar->setTabButton(index, QTabBar::RightSide, closeButton);
                }
            }
        }
        else
        {
            for (int index = 0; index < static_cast<int>(m_tabs.size()); ++index)
            {
                const auto title = titleForTab(m_tabs[static_cast<std::size_t>(index)]);
                if (m_tabBar->tabText(index) != title)
                {
                    m_tabBar->setTabText(index, title);
                }
                const auto icon = iconForTab(m_tabs[static_cast<std::size_t>(index)]);
                m_tabBar->setTabIcon(index, icon);
            }
        }

        for (int index = 0; index < static_cast<int>(m_tabs.size()); ++index)
        {
            const auto& content = m_tabs[static_cast<std::size_t>(index)].content;
            const bool canClose = index != 0 || std::holds_alternative<ComposeTabState>(content) ||
                                  std::holds_alternative<ContactsTabState>(content) ||
                                  std::holds_alternative<CalendarTabState>(content);
            if (!canClose)
            {
                m_tabBar->setTabButton(index, QTabBar::RightSide, nullptr);
                continue;
            }

            if (m_tabBar->tabButton(index, QTabBar::RightSide) != nullptr)
            {
                continue;
            }

            auto* closeButton = new QToolButton(m_tabBar);
            closeButton->setAutoRaise(true);
            closeButton->setText(QStringLiteral("x"));
            connect(closeButton, &QToolButton::clicked, this, [this, index] { closeTab(index); });
            m_tabBar->setTabButton(index, QTabBar::RightSide, closeButton);
        }

        if (m_activeTabIndex.has_value() && *m_activeTabIndex >= 0 &&
            *m_activeTabIndex < m_tabBar->count() && m_tabBar->currentIndex() != *m_activeTabIndex)
        {
            m_tabBar->setCurrentIndex(*m_activeTabIndex);
        }
        m_tabBar->setVisible(m_tabs.size() > 1);
        updateWindowTitle();
    }

    void MainWindow::updateWindowTitle()
    {
        const auto* tab = activeTab();
        if (tab == nullptr)
        {
            setWindowTitle(QStringLiteral("Javelin Mail"));
            return;
        }

        setWindowTitle(titleForTab(*tab));
    }

    void MainWindow::openOrActivateMailboxTab(std::string accountId, std::string mailboxId,
                                              const QString title, std::optional<std::string> role,
                                              const bool refreshRemote)
    {
        for (std::size_t index = 0; index < m_tabs.size(); ++index)
        {
            if (auto* mailboxTab = std::get_if<MailboxTabState>(&m_tabs[index].content);
                mailboxTab != nullptr && mailboxTab->accountId == accountId &&
                mailboxTab->mailboxId == mailboxId)
            {
                mailboxTab->title = title;
                mailboxTab->role = std::move(role);
                m_activeTabIndex = static_cast<int>(index);
                updateTabBar();
                activateTab(*m_activeTabIndex, refreshRemote);
                return;
            }
        }

        m_tabs.push_back(TabState{
            .content =
                MailboxTabState{
                    .accountId = std::move(accountId),
                    .mailboxId = std::move(mailboxId),
                    .title = title,
                    .role = std::move(role),
                    .page = {},
                    .selection = {},
                    .observationId = {},
                },
        });
        ensureMailboxObservation(std::get<MailboxTabState>(m_tabs.back().content));
        m_activeTabIndex = static_cast<int>(m_tabs.size() - 1);
        updateTabBar();
        activateTab(*m_activeTabIndex, refreshRemote);
    }

    void MainWindow::activateMailboxInHomeTab(std::string accountId, std::string mailboxId,
                                              QString title, std::optional<std::string> role,
                                              const std::optional<std::size_t> total,
                                              const bool refreshRemote)
    {
        if (m_tabs.empty())
        {
            m_tabs.push_back(TabState{
                .content =
                    MailboxTabState{
                        .accountId = std::move(accountId),
                        .mailboxId = std::move(mailboxId),
                        .title = std::move(title),
                        .role = std::move(role),
                        .page =
                            PageState{
                                .offset = 0,
                                .total = total,
                                .items = {},
                                .cacheLoaded = false,
                                .refreshInFlight = false,
                                .stale = false,
                                .refreshError = {},
                            },
                        .selection = {},
                        .observationId = {},
                    },
            });
        }
        else
        {
            if (auto* previous = std::get_if<MailboxTabState>(&m_tabs[0].content))
                releaseMailboxObservation(*previous);
            m_tabs[0].content = MailboxTabState{
                .accountId = std::move(accountId),
                .mailboxId = std::move(mailboxId),
                .title = std::move(title),
                .role = std::move(role),
                .page =
                    PageState{
                        .offset = 0,
                        .total = total,
                        .items = {},
                        .cacheLoaded = false,
                        .refreshInFlight = false,
                        .stale = false,
                        .refreshError = {},
                    },
                .selection = {},
                .observationId = {},
            };
        }

        ensureMailboxObservation(std::get<MailboxTabState>(m_tabs[0].content));
        m_activeTabIndex = 0;
        updateTabBar();
        activateTab(0, refreshRemote);
    }

    void MainWindow::openOrActivateSearchTab(std::string accountId, QString query,
                                             const bool refreshRemote)
    {
        openOrActivateSearchTab(
            std::move(accountId),
            javelin::jmap::search::EmailSearchCriteria{.text = query.toStdString()}, refreshRemote);
    }

    void MainWindow::openOrActivateSearchTab(std::string accountId,
                                             javelin::jmap::search::EmailSearchCriteria criteria,
                                             const bool refreshRemote)
    {
        const auto queryString = javelin::jmap::search::displayString(criteria);
        const auto query = QString::fromStdString(queryString);
        for (std::size_t index = 0; index < m_tabs.size(); ++index)
        {
            if (auto* searchTab = std::get_if<SearchTabState>(&m_tabs[index].content);
                searchTab != nullptr && searchTab->accountId == accountId &&
                searchTab->query == queryString)
            {
                m_activeTabIndex = static_cast<int>(index);
                updateTabBar();
                activateTab(*m_activeTabIndex, refreshRemote);
                return;
            }
        }

        m_tabs.push_back(TabState{
            .content =
                SearchTabState{
                    .accountId = std::move(accountId),
                    .query = queryString,
                    .criteria = std::move(criteria),
                    .title = QStringLiteral("Search: %1").arg(query),
                    .page = {},
                    .selection = {},
                },
        });
        m_activeTabIndex = static_cast<int>(m_tabs.size() - 1);
        updateTabBar();
        activateTab(*m_activeTabIndex, refreshRemote);
    }

    void MainWindow::activateTab(const int index, const bool refreshRemote)
    {
        QElapsedTimer timer;
        timer.start();
        if (index < 0 || static_cast<std::size_t>(index) >= m_tabs.size())
        {
            m_activeTabIndex.reset();
            m_messageModel->clear();
            m_messageViewContainer->setSelection(m_messageViewService, std::nullopt, std::nullopt,
                                                 std::nullopt);
            if (m_contentStack != nullptr)
            {
                m_contentStack->setCurrentIndex(0);
            }
            updateTabBar();
            updateEmptyStates();
            updateMessageListHeader();
            updateMessageActions();
            return;
        }

        m_activeTabIndex = index;
        if (m_tabBar->currentIndex() != index)
        {
            QSignalBlocker blocker{m_tabBar};
            m_tabBar->setCurrentIndex(index);
        }
        if (const auto* composeTab =
                std::get_if<ComposeTabState>(&m_tabs[static_cast<std::size_t>(index)].content);
            composeTab != nullptr && composeTab->widget != nullptr)
        {
            m_contentStack->setCurrentWidget(composeTab->widget);
        }
        else if (const auto* contactsTab = std::get_if<ContactsTabState>(
                     &m_tabs[static_cast<std::size_t>(index)].content);
                 contactsTab != nullptr && contactsTab->widget != nullptr)
        {
            m_contentStack->setCurrentWidget(contactsTab->widget);
        }
        else if (const auto* calendarTab = std::get_if<CalendarTabState>(
                     &m_tabs[static_cast<std::size_t>(index)].content);
                 calendarTab != nullptr && calendarTab->widget != nullptr)
        {
            m_contentStack->setCurrentWidget(calendarTab->widget);
        }
        else if (m_contentStack != nullptr)
        {
            m_contentStack->setCurrentIndex(0);
        }
        syncNavigationForActiveTab();
        loadActiveTabFromCache();
        if (refreshRemote)
        {
            refreshActiveTabFromServer();
        }

        qCDebug(logGuiMailbox).noquote() << "activate tab" << index << "refreshRemote"
                                         << refreshRemote << "ms" << timer.elapsed();
    }

    void MainWindow::closeTab(const int index)
    {
        if (index < 0 || static_cast<std::size_t>(index) >= m_tabs.size())
        {
            return;
        }
        if (index == 0 && !std::holds_alternative<ComposeTabState>(m_tabs[0].content) &&
            !std::holds_alternative<ContactsTabState>(m_tabs[0].content) &&
            !std::holds_alternative<CalendarTabState>(m_tabs[0].content))
        {
            return;
        }

        if (std::holds_alternative<ComposeTabState>(
                m_tabs[static_cast<std::size_t>(index)].content) &&
            !closeComposeTab(index))
        {
            return;
        }

        if (auto* contactsTab =
                std::get_if<ContactsTabState>(&m_tabs[static_cast<std::size_t>(index)].content))
        {
            if (contactsTab->widget != nullptr && contactsTab->widget->operationInFlight())
            {
                m_statusBar->showMessage(
                    QStringLiteral("Wait for the Contacts operation to finish."), 5000);
                return;
            }
            if (contactsTab->widget != nullptr)
            {
                m_contentStack->removeWidget(contactsTab->widget);
                contactsTab->widget->deleteLater();
            }
        }

        if (auto* calendarTab =
                std::get_if<CalendarTabState>(&m_tabs[static_cast<std::size_t>(index)].content))
        {
            if (calendarTab->widget != nullptr)
            {
                m_contentStack->removeWidget(calendarTab->widget);
                calendarTab->widget->deleteLater();
            }
        }

        if (auto* mailboxTab =
                std::get_if<MailboxTabState>(&m_tabs[static_cast<std::size_t>(index)].content))
            releaseMailboxObservation(*mailboxTab);
        m_tabs.erase(m_tabs.begin() + index);
        if (m_tabs.empty())
        {
            m_activeTabIndex.reset();
            activateTab(-1, false);
            return;
        }

        if (!m_activeTabIndex.has_value() || *m_activeTabIndex >= index)
        {
            m_activeTabIndex = std::max(0, index - 1);
        }
        updateTabBar();
        activateTab(*m_activeTabIndex, false);
    }

    void MainWindow::syncNavigationForActiveTab()
    {
        const auto* tab = activeTab();
        if (tab == nullptr)
        {
            if (m_mailboxPane != nullptr)
            {
                m_mailboxPane->setVisible(true);
            }
            return;
        }

        if (m_mailboxPane != nullptr)
        {
            m_mailboxPane->setVisible(m_activeTabIndex == std::optional<int>{0} &&
                                      !activeTabIsCompose() && !activeTabIsContacts() &&
                                      !activeTabIsCalendar());
        }

        m_syncingNavigation = true;
        QSignalBlocker mailboxBlocker{m_mailboxView->selectionModel()};
        QSignalBlocker searchBlocker{m_mailboxSearchEdit};

        if (const auto* composeTab = std::get_if<ComposeTabState>(&tab->content))
        {
            Q_UNUSED(composeTab);
            m_mailboxSearchEdit->clear();
        }
        else if (std::holds_alternative<ContactsTabState>(tab->content) ||
                 std::holds_alternative<CalendarTabState>(tab->content))
        {
            m_mailboxSearchEdit->clear();
        }
        else if (const auto* mailboxTab = std::get_if<MailboxTabState>(&tab->content))
        {
            const auto mailboxIndex = findMailboxIndexForSelection(
                *m_mailboxModel, QString::fromStdString(mailboxTab->accountId),
                std::optional<QString>{QString::fromStdString(mailboxTab->mailboxId)});
            if (mailboxIndex.isValid())
            {
                m_mailboxView->setCurrentIndex(mailboxIndex);
                m_mailboxView->scrollTo(mailboxIndex);
            }
            m_mailboxSearchEdit->clear();
        }
        else
        {
            const auto& searchTab = std::get<SearchTabState>(tab->content);
            const auto accountIndex = findMailboxIndexForSelection(
                *m_mailboxModel, QString::fromStdString(searchTab.accountId), std::nullopt);
            if (accountIndex.isValid())
            {
                m_mailboxView->setCurrentIndex(accountIndex);
                m_mailboxView->scrollTo(accountIndex);
            }
            m_mailboxSearchEdit->setText(QString::fromStdString(searchTab.query));
        }

        m_syncingNavigation = false;
    }

    void MainWindow::loadMailboxTabPageFromCache(MailboxTabState& tab, const bool forceReload)
    {
        QElapsedTimer timer;
        timer.start();
        if (tab.page.cacheLoaded && !forceReload)
        {
            qCDebug(logGuiMailbox).noquote()
                << "cache load skipped" << QString::fromStdString(tab.accountId)
                << QString::fromStdString(tab.mailboxId);
            return;
        }

        const auto pageResult = m_queryService.listMailboxMessages(
            tab.accountId, tab.mailboxId, pageSize, tab.page.offset, m_emailListSort);
        if (const auto* items =
                std::get_if<std::vector<javelin::jmap::cache::MessageListItem>>(&pageResult))
        {
            tab.page.items = *items;
            tab.page.cacheLoaded = true;
        }
        else
        {
            tab.page.items.clear();
            tab.page.cacheLoaded = false;
        }

        if (!tab.page.total.has_value() || forceReload)
        {
            const auto totalResult =
                m_queryService.countMailboxMessages(tab.accountId, tab.mailboxId);
            if (const auto* total = std::get_if<std::size_t>(&totalResult))
            {
                tab.page.total = *total;
            }
            else
            {
                tab.page.total.reset();
            }
        }

        qCDebug(logGuiMailbox).noquote()
            << "cache load" << QString::fromStdString(tab.accountId)
            << QString::fromStdString(tab.mailboxId) << "offset"
            << static_cast<qulonglong>(tab.page.offset) << "rows"
            << static_cast<qulonglong>(tab.page.items.size()) << "ms" << timer.elapsed();
    }

    void MainWindow::applySearchTabCachedPage(SearchTabState& tab, const bool forceReload)
    {
        if (tab.page.cacheLoaded && !forceReload)
        {
            return;
        }

        const auto queryKey = javelin::jmap::search::cacheKey(tab.criteria, m_emailListSort);
        const auto pageResult =
            m_queryService.loadSearchWindow(tab.accountId, queryKey, tab.page.offset, pageSize);
        const auto* page =
            std::get_if<std::optional<javelin::jmap::cache::SearchWindowPage>>(&pageResult);
        if (page == nullptr || !page->has_value())
        {
            tab.page.items.clear();
            tab.page.total.reset();
            tab.page.cacheLoaded = page != nullptr;
            return;
        }

        tab.page.items = (*page)->items;
        tab.page.total = (*page)->total;
        tab.page.cacheLoaded = true;
    }

    void MainWindow::applyActiveTabPageToModel()
    {
        if (const auto* tab = activeTab())
        {
            if (const auto* mailboxTab = std::get_if<MailboxTabState>(&tab->content))
            {
                m_messageModel->setPage(mailboxTab->accountId, mailboxTab->page.items);
                return;
            }

            if (const auto* searchTab = std::get_if<SearchTabState>(&tab->content))
            {
                m_messageModel->setPage(searchTab->accountId, searchTab->page.items);
                return;
            }

            m_messageModel->clear();
            return;
        }

        m_messageModel->clear();
    }

    void MainWindow::loadMailboxTabFromCache(const std::string_view accountId,
                                             const std::string_view mailboxId,
                                             const bool applyIfActive,
                                             const std::optional<std::size_t> requiredOffset)
    {
        auto* active = activeTab();
        bool activeTabReloaded = false;
        for (auto& tabState : m_tabs)
        {
            auto* mailboxTab = std::get_if<MailboxTabState>(&tabState.content);
            if (mailboxTab == nullptr || mailboxTab->accountId != accountId ||
                mailboxTab->mailboxId != mailboxId ||
                (requiredOffset.has_value() && mailboxTab->page.offset != *requiredOffset))
            {
                continue;
            }

            loadMailboxTabPageFromCache(*mailboxTab, true);
            activeTabReloaded = activeTabReloaded || &tabState == active;
        }

        if (!applyIfActive || !activeTabReloaded)
        {
            return;
        }

        const auto previousMessageRow = currentMessageRow(*m_messageView);
        {
            QSignalBlocker messageSelectionBlocker{m_messageView->selectionModel()};
            applyActiveTabPageToModel();
            restoreActiveTabMessageSelection(previousMessageRow);
        }
        refreshSelectionFromModels();
    }

    void MainWindow::loadSearchTabFromCache(const std::string_view accountId,
                                            const std::string_view queryKey,
                                            const std::size_t offset, const bool applyIfActive)
    {
        auto* active = activeTab();
        bool activeTabReloaded = false;
        for (auto& tabState : m_tabs)
        {
            auto* searchTab = std::get_if<SearchTabState>(&tabState.content);
            if (searchTab == nullptr || searchTab->accountId != accountId ||
                searchTab->page.offset != offset ||
                javelin::jmap::search::cacheKey(searchTab->criteria, m_emailListSort) != queryKey)
            {
                continue;
            }

            applySearchTabCachedPage(*searchTab, true);
            searchTab->page.stale = false;
            activeTabReloaded = activeTabReloaded || &tabState == active;
        }

        if (!applyIfActive || !activeTabReloaded)
        {
            return;
        }

        const auto previousMessageRow = currentMessageRow(*m_messageView);
        {
            QSignalBlocker messageSelectionBlocker{m_messageView->selectionModel()};
            applyActiveTabPageToModel();
            restoreActiveTabMessageSelection(previousMessageRow);
        }
        refreshSelectionFromModels();
    }

    void MainWindow::ensureMailboxObservation(MailboxTabState& tab)
    {
        if (!tab.observationId.has_value())
            tab.observationId = m_mailService.observeMailbox(tab.accountId, tab.mailboxId);
    }

    void MainWindow::releaseMailboxObservation(MailboxTabState& tab)
    {
        tab.observationId.reset();
    }

    void MainWindow::loadActiveTabFromCache(const bool forceReload, const bool refreshRemote)
    {
        auto* tab = activeTab();
        if (tab == nullptr)
        {
            applyActiveTabPageToModel();
            refreshSelectionFromModels();
            return;
        }

        if (auto* mailboxTab = std::get_if<MailboxTabState>(&tab->content))
        {
            loadMailboxTabPageFromCache(*mailboxTab, forceReload);
            if (refreshRemote)
            {
                refreshMailboxTabFromServer(*mailboxTab);
            }
        }
        else if (auto* searchTab = std::get_if<SearchTabState>(&tab->content))
        {
            applySearchTabCachedPage(*searchTab, forceReload);
            if (refreshRemote && shouldRefreshSearchTabFromServer(*searchTab))
            {
                refreshSearchTabFromServer(*searchTab);
            }
        }
        else
        {
            if (std::holds_alternative<ContactsTabState>(tab->content) ||
                std::holds_alternative<CalendarTabState>(tab->content))
            {
                m_messageModel->clear();
                m_messageViewContainer->setSelection(m_messageViewService, std::nullopt,
                                                     std::nullopt, std::nullopt);
            }
            updateMessageActions();
            return;
        }

        const auto previousMessageRow = currentMessageRow(*m_messageView);
        {
            QSignalBlocker messageSelectionBlocker{m_messageView->selectionModel()};
            applyActiveTabPageToModel();
            restoreActiveTabMessageSelection(previousMessageRow);
        }
        refreshSelectionFromModels();
    }

    void MainWindow::refreshActiveTabFromServer()
    {
        if (!m_activeTabIndex.has_value() || *m_activeTabIndex < 0 ||
            static_cast<std::size_t>(*m_activeTabIndex) >= m_tabs.size())
        {
            return;
        }

        refreshTabFromServer(static_cast<std::size_t>(*m_activeTabIndex));
    }

    void MainWindow::refreshTabFromServer(const std::size_t tabIndex)
    {
        if (tabIndex >= m_tabs.size())
        {
            return;
        }

        auto& tab = m_tabs[tabIndex];
        if (auto* mailboxTab = std::get_if<MailboxTabState>(&tab.content))
        {
            refreshMailboxTabFromServer(*mailboxTab);
            return;
        }

        if (auto* searchTab = std::get_if<SearchTabState>(&tab.content))
        {
            refreshSearchTabFromServer(*searchTab);
            return;
        }

        if (auto* contactsTab = std::get_if<ContactsTabState>(&tab.content);
            contactsTab != nullptr && contactsTab->widget != nullptr)
        {
            contactsTab->widget->requestRefresh();
        }
        else if (auto* calendarTab = std::get_if<CalendarTabState>(&tab.content);
                 calendarTab != nullptr && calendarTab->widget != nullptr)
        {
            calendarTab->widget->setDisplayedMonth(calendarTab->widget->displayedMonth());
        }
    }

    void MainWindow::refreshMailboxTabFromServer(MailboxTabState& tab)
    {
        if (tab.page.refreshInFlight)
        {
            return;
        }

        tab.page.refreshInFlight = true;
        tab.page.refreshError.clear();
        updateEmptyStates();
        const auto tabAccountId = tab.accountId;
        const auto tabMailboxId = tab.mailboxId;
        const auto tabOffset = tab.page.offset;
        auto task = m_mailService.requestMailboxWindow(javelin::app::MailboxWindowIntent{
            .accountId = tab.accountId,
            .mailboxId = tab.mailboxId,
            .offset = tab.page.offset,
            .limit = pageSize,
            .sort = m_emailListSort,
            .forceRefresh = tab.page.stale,
        });
        QCoro::connect(
            std::move(task), this,
            [this, tabAccountId, tabMailboxId, tabOffset](javelin::app::MailboxWindowResult result)
            {
                if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&result))
                {
                    for (auto& tabState : m_tabs)
                    {
                        auto* mailboxTab = std::get_if<MailboxTabState>(&tabState.content);
                        if (mailboxTab != nullptr && mailboxTab->accountId == tabAccountId &&
                            mailboxTab->mailboxId == tabMailboxId &&
                            mailboxTab->page.offset == tabOffset)
                        {
                            mailboxTab->page.refreshInFlight = false;
                            mailboxTab->page.refreshError = error->message;
                            break;
                        }
                    }
                    presentError(*error);
                    updateEmptyStates();
                    return;
                }

                for (auto& tabState : m_tabs)
                {
                    auto* mailboxTab = std::get_if<MailboxTabState>(&tabState.content);
                    if (mailboxTab == nullptr || mailboxTab->accountId != tabAccountId ||
                        mailboxTab->mailboxId != tabMailboxId ||
                        mailboxTab->page.offset != tabOffset)
                    {
                        continue;
                    }

                    const auto& summary = std::get<javelin::app::MailboxWindowSummary>(result);
                    mailboxTab->page.total = summary.total;
                    mailboxTab->page.refreshInFlight = false;
                    mailboxTab->page.stale = false;
                    mailboxTab->page.refreshError.clear();
                    updateEmptyStates();
                    m_statusBar->showMessage(
                        QStringLiteral("Loaded %1 mailbox conversations.")
                            .arg(static_cast<qulonglong>(summary.representativeCount)),
                        5000);
                    return;
                }
            });
    }

    void MainWindow::refreshSearchTabFromServer(SearchTabState& tab)
    {
        if (tab.page.refreshInFlight)
        {
            return;
        }

        tab.page.refreshInFlight = true;
        tab.page.refreshError.clear();
        updateEmptyStates();
        const auto tabAccountId = tab.accountId;
        const auto tabQuery = tab.query;
        const auto tabCriteria = tab.criteria;
        const auto tabOffset = tab.page.offset;
        auto task = m_mailService.requestSearchWindow(javelin::app::SearchWindowIntent{
            .accountId = tab.accountId,
            .criteria = tabCriteria,
            .offset = tab.page.offset,
            .limit = pageSize,
            .sort = m_emailListSort,
        });
        QCoro::connect(
            std::move(task), this,
            [this, tabAccountId, tabQuery, tabOffset](javelin::app::SearchWindowResult result)
            {
                if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&result))
                {
                    for (auto& tabState : m_tabs)
                    {
                        auto* searchTab = std::get_if<SearchTabState>(&tabState.content);
                        if (searchTab != nullptr && searchTab->accountId == tabAccountId &&
                            searchTab->query == tabQuery && searchTab->page.offset == tabOffset)
                        {
                            searchTab->page.refreshInFlight = false;
                            searchTab->page.refreshError = error->message;
                            break;
                        }
                    }
                    presentError(*error);
                    updateEmptyStates();
                    return;
                }

                for (auto& tabState : m_tabs)
                {
                    auto* searchTab = std::get_if<SearchTabState>(&tabState.content);
                    if (searchTab == nullptr || searchTab->accountId != tabAccountId ||
                        searchTab->query != tabQuery || searchTab->page.offset != tabOffset)
                    {
                        continue;
                    }

                    const auto& summary = std::get<javelin::app::SearchWindowSummary>(result);
                    searchTab->page.total = summary.total;
                    searchTab->page.refreshInFlight = false;
                    searchTab->page.stale = false;
                    searchTab->page.refreshError.clear();
                    updateEmptyStates();
                    m_statusBar->showMessage(
                        summary.total.has_value() && *summary.total > summary.representativeCount
                            ? QStringLiteral("Showing the first %1 of %2 server matches.")
                                  .arg(static_cast<qulonglong>(summary.representativeCount))
                                  .arg(static_cast<qulonglong>(*summary.total))
                            : QStringLiteral("Loaded %1 server search matches.")
                                  .arg(static_cast<qulonglong>(summary.representativeCount)),
                        5000);
                    return;
                }
            });
    }

    bool MainWindow::shouldRefreshSearchTabFromServer(const SearchTabState& tab) const
    {
        if (tab.page.refreshInFlight)
        {
            return false;
        }

        return tab.page.stale;
    }

    void MainWindow::activateMailboxSelection(const bool refreshRemote)
    {
        QElapsedTimer timer;
        timer.start();
        const auto accountId = currentAccountId(*m_mailboxView);
        const auto mailboxId = currentMailboxId(*m_mailboxView);
        if (!accountId.has_value() || !mailboxId.has_value())
        {
            return;
        }

        const auto currentIndex = m_mailboxView->currentIndex();
        const auto totalThreadsValue =
            currentIndex.data(javelin::gui::mailboxes::MailboxTreeModel::TotalThreadsRole);
        const auto totalThreads = totalThreadsValue.isValid()
                                      ? std::optional<std::size_t>{static_cast<std::size_t>(
                                            totalThreadsValue.toULongLong())}
                                      : std::nullopt;
        activateMailboxInHomeTab(*accountId, *mailboxId,
                                 currentIndex.data(Qt::DisplayRole).toString(),
                                 currentMailboxRole(*m_mailboxView), totalThreads, refreshRemote);
        qInfo().noquote() << "GUI activate mailbox selection" << QString::fromStdString(*accountId)
                          << QString::fromStdString(*mailboxId) << "refreshRemote" << refreshRemote
                          << "ms" << timer.elapsed();
    }

    void
    MainWindow::markTabsStaleForAccount(const std::string_view accountId,
                                        const std::optional<std::string_view> refreshedMailboxId)
    {
        for (auto& tab : m_tabs)
        {
            std::visit(
                [accountId, refreshedMailboxId](auto& content)
                {
                    if constexpr (!std::is_same_v<std::decay_t<decltype(content)>, ComposeTabState>)
                    {
                        if (content.accountId == accountId)
                        {
                            if constexpr (std::is_same_v<std::decay_t<decltype(content)>,
                                                         MailboxTabState>)
                            {
                                if (refreshedMailboxId.has_value() &&
                                    content.mailboxId == *refreshedMailboxId)
                                {
                                    return;
                                }
                            }
                            content.page.stale = true;
                        }
                    }
                },
                tab.content);
        }
    }

    void MainWindow::openMailboxSelectionInTab(const bool refreshRemote)
    {
        const auto accountId = currentAccountId(*m_mailboxView);
        const auto mailboxId = currentMailboxId(*m_mailboxView);
        if (!accountId.has_value() || !mailboxId.has_value())
        {
            return;
        }

        const auto currentIndex = m_mailboxView->currentIndex();
        const auto title = currentIndex.data(Qt::DisplayRole).toString();
        const auto role = currentMailboxRole(*m_mailboxView);
        for (std::size_t index = 1; index < m_tabs.size(); ++index)
        {
            if (auto* mailboxTab = std::get_if<MailboxTabState>(&m_tabs[index].content);
                mailboxTab != nullptr && mailboxTab->accountId == *accountId &&
                mailboxTab->mailboxId == *mailboxId)
            {
                mailboxTab->role = role;
                m_activeTabIndex = static_cast<int>(index);
                updateTabBar();
                activateTab(*m_activeTabIndex, refreshRemote);
                return;
            }
        }

        m_tabs.push_back(TabState{
            .content =
                MailboxTabState{
                    .accountId = *accountId,
                    .mailboxId = *mailboxId,
                    .title = title,
                    .role = role,
                    .page = {},
                    .selection = {},
                    .observationId = {},
                },
        });
        ensureMailboxObservation(std::get<MailboxTabState>(m_tabs.back().content));
        m_activeTabIndex = static_cast<int>(m_tabs.size() - 1);
        updateTabBar();
        activateTab(*m_activeTabIndex, refreshRemote);
    }

    void MainWindow::openComposeForRequest(javelin::jmap::submission::OpenComposeRequest request)
    {
        const auto settings = javelin::gui::settings::PreferencesDialog::loadSettingsForAccount(
            QString::fromStdString(request.accountId));
        if (settings.sessionUrl.isEmpty() || settings.loginEmail.isEmpty() ||
            settings.apiKey.isEmpty())
        {
            presentUserInterventionError(
                QStringLiteral("Set Session URL, Login Email, and API Key in Preferences first."));
            return;
        }

        auto task =
            m_composeService.open(toAccountConnectionSettings(settings), std::move(request));
        QCoro::connect(
            std::move(task), this,
            [this](std::variant<javelin::jmap::submission::DraftSnapshot,
                                javelin::jmap::LiveRefreshError>
                       result)
            {
                if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&result))
                {
                    qWarning().noquote() << "GUI compose open failed" << error->message;
                    presentError(*error);
                    return;
                }

                openOrActivateComposeTab(
                    std::get<javelin::jmap::submission::DraftSnapshot>(std::move(result)));
            });
    }

    void MainWindow::openOrActivateComposeTab(javelin::jmap::submission::DraftSnapshot snapshot)
    {
        for (std::size_t index = 0; index < m_tabs.size(); ++index)
        {
            if (auto* composeTab = std::get_if<ComposeTabState>(&m_tabs[index].content);
                composeTab != nullptr && composeTab->composeSessionId == snapshot.composeSessionId)
            {
                composeTab->title = snapshot.subject.has_value()
                                        ? QString::fromStdString(*snapshot.subject)
                                        : composeTab->title;
                m_activeTabIndex = static_cast<int>(index);
                updateTabBar();
                activateTab(*m_activeTabIndex, false);
                return;
            }
        }

        m_tabs.push_back(TabState{
            .content =
                ComposeTabState{
                    .accountId = snapshot.accountId,
                    .composeSessionId = snapshot.composeSessionId,
                    .title = snapshot.subject.has_value()
                                 ? QString::fromStdString(*snapshot.subject)
                                 : QStringLiteral("Compose"),
                    .widget = nullptr,
                    .page = {},
                    .selection = {},
                },
        });
        const auto index = static_cast<int>(m_tabs.size() - 1);
        auto* widget = new javelin::gui::compose::ComposeTabWidget(
            m_composeService, m_identityRepository, m_contactIdentityLookup, std::move(snapshot),
            m_contentStack);
        attachComposeWidget(widget, index);
        m_activeTabIndex = index;
        updateTabBar();
        activateTab(index, false);
    }

    void MainWindow::attachComposeWidget(javelin::gui::compose::ComposeTabWidget* widget,
                                         const int tabIndex)
    {
        if (widget == nullptr || tabIndex < 0 ||
            static_cast<std::size_t>(tabIndex) >= m_tabs.size())
        {
            return;
        }

        auto* composeTab =
            std::get_if<ComposeTabState>(&m_tabs[static_cast<std::size_t>(tabIndex)].content);
        if (composeTab == nullptr)
        {
            return;
        }

        composeTab->widget = widget;
        composeTab->title = widget->tabTitle();
        m_contentStack->addWidget(widget);
        connect(widget, &javelin::gui::compose::ComposeTabWidget::titleChanged, this,
                [this, widget](const QString& title)
                {
                    for (std::size_t index = 0; index < m_tabs.size(); ++index)
                    {
                        auto* matchingTab = std::get_if<ComposeTabState>(&m_tabs[index].content);
                        if (matchingTab == nullptr || matchingTab->widget != widget)
                        {
                            continue;
                        }

                        matchingTab->title = title;
                        updateTabBar();
                        return;
                    }
                });
        connect(widget, &javelin::gui::compose::ComposeTabWidget::accountChanged, this,
                [this, widget](const QString& accountId)
                {
                    for (auto& tab : m_tabs)
                    {
                        auto* matchingTab = std::get_if<ComposeTabState>(&tab.content);
                        if (matchingTab != nullptr && matchingTab->widget == widget)
                        {
                            matchingTab->accountId = accountId.toStdString();
                            return;
                        }
                    }
                });
        connect(widget, &javelin::gui::compose::ComposeTabWidget::statusMessageRequested, this,
                [this](const QString& message, const int timeoutMs)
                { m_statusBar->showMessage(message, timeoutMs); });
        connect(widget, &javelin::gui::compose::ComposeTabWidget::userInterventionRequired, this,
                [this](const QString& message)
                { QMessageBox::critical(this, QStringLiteral("Action Required"), message); });
        connect(widget, &javelin::gui::compose::ComposeTabWidget::closeRequested, this,
                [this, widget]
                {
                    for (std::size_t index = 0; index < m_tabs.size(); ++index)
                    {
                        auto* matchingTab = std::get_if<ComposeTabState>(&m_tabs[index].content);
                        if (matchingTab != nullptr && matchingTab->widget == widget)
                        {
                            closeTab(static_cast<int>(index));
                            return;
                        }
                    }
                });
    }

    bool MainWindow::closeComposeTab(const int index)
    {
        if (index < 0 || static_cast<std::size_t>(index) >= m_tabs.size())
        {
            return false;
        }

        auto* composeTab =
            std::get_if<ComposeTabState>(&m_tabs[static_cast<std::size_t>(index)].content);
        if (composeTab == nullptr || composeTab->widget == nullptr)
        {
            return false;
        }

        auto* widget = composeTab->widget;
        if (widget->operationInFlight())
        {
            m_statusBar->showMessage(
                QStringLiteral("Wait for the current compose operation to finish first."), 5000);
            return false;
        }

        if (widget->closeWithoutPrompt())
        {
            m_contentStack->removeWidget(widget);
            widget->deleteLater();
            composeTab->widget = nullptr;
            return true;
        }

        if (widget->isEmptyDraft())
        {
            if (const auto error = m_composeService.discard(widget->composeSessionId()))
            {
                presentError(*error);
                return false;
            }
            m_contentStack->removeWidget(widget);
            widget->deleteLater();
            composeTab->widget = nullptr;
            return true;
        }

        if (widget->draftEmailId().has_value())
        {
            QMessageBox messageBox{this};
            messageBox.setWindowTitle(QStringLiteral("Close Draft"));
            messageBox.setText(QStringLiteral(
                "This message is already saved in Drafts. Close the tab and keep it there?"));
            QAbstractButton* keepDraftButton =
                messageBox.addButton(QStringLiteral("Keep Draft"), QMessageBox::AcceptRole);
            messageBox.addButton(QMessageBox::Cancel);
            messageBox.exec();
            if (messageBox.clickedButton() != keepDraftButton)
            {
                return false;
            }

            if (const auto error = m_composeService.discard(widget->composeSessionId()))
            {
                presentError(*error);
                return false;
            }
            m_contentStack->removeWidget(widget);
            widget->deleteLater();
            composeTab->widget = nullptr;
            return true;
        }

        QMessageBox messageBox{this};
        messageBox.setWindowTitle(QStringLiteral("Close Compose Tab"));
        messageBox.setText(QStringLiteral("Save this message as a draft before closing?"));
        QAbstractButton* saveButton =
            messageBox.addButton(QStringLiteral("Save Draft"), QMessageBox::AcceptRole);
        QAbstractButton* discardButton =
            messageBox.addButton(QStringLiteral("Discard"), QMessageBox::DestructiveRole);
        messageBox.addButton(QMessageBox::Cancel);
        messageBox.exec();
        if (messageBox.clickedButton() == saveButton)
        {
            widget->saveDraftAndClose();
            return false;
        }
        if (messageBox.clickedButton() != discardButton)
        {
            return false;
        }

        if (const auto error = m_composeService.discard(widget->composeSessionId()))
        {
            presentError(*error);
            return false;
        }

        m_contentStack->removeWidget(widget);
        widget->deleteLater();
        composeTab->widget = nullptr;
        return true;
    }

    void MainWindow::executeSearch(const QString& text)
    {
        const QString trimmed = text.trimmed();
        if (trimmed.isEmpty())
        {
            clearSearch();
            return;
        }

        const auto accountId =
            activeAccountId().has_value() ? activeAccountId() : currentAccountId(*m_mailboxView);
        if (!accountId.has_value())
        {
            m_statusBar->showMessage(QStringLiteral("Select an account before searching."), 5000);
            return;
        }

        openOrActivateSearchTab(*accountId, trimmed, true);
    }

    void MainWindow::showAdvancedSearch()
    {
        const auto accountId =
            activeAccountId().has_value() ? activeAccountId() : currentAccountId(*m_mailboxView);
        if (!accountId.has_value())
        {
            m_statusBar->showMessage(QStringLiteral("Select an account before searching."), 5000);
            return;
        }

        javelin::gui::search::AdvancedSearchDialog dialog{this};
        if (dialog.exec() != QDialog::Accepted)
        {
            return;
        }

        auto criteria = dialog.criteria();
        if (javelin::jmap::search::isEmpty(criteria))
        {
            m_statusBar->showMessage(QStringLiteral("Enter at least one search field."), 5000);
            return;
        }

        openOrActivateSearchTab(*accountId, std::move(criteria), true);
    }

    void MainWindow::clearSearch()
    {
        const auto* tab = activeTab();
        if (tab != nullptr && std::holds_alternative<SearchTabState>(tab->content))
        {
            closeTab(*m_activeTabIndex);
        }
        else
        {
            m_mailboxSearchEdit->clear();
        }
    }

    void MainWindow::showSortMenu()
    {
        QMenu menu{this};
        const auto addPropertyAction =
            [this, &menu](const QString& label,
                          const javelin::jmap::query::EmailListSortProperty property)
        {
            auto* action = menu.addAction(label);
            action->setCheckable(true);
            action->setChecked(m_emailListSort.property == property);
            connect(action, &QAction::triggered, this,
                    [this, property]
                    {
                        setEmailListSort(javelin::jmap::query::EmailListSort{
                            .property = property,
                            .direction = m_emailListSort.direction,
                        });
                    });
        };

        addPropertyAction(QStringLiteral("Date received"),
                          javelin::jmap::query::EmailListSortProperty::ReceivedAt);
        addPropertyAction(QStringLiteral("Date sent"),
                          javelin::jmap::query::EmailListSortProperty::SentAt);
        addPropertyAction(QStringLiteral("From"),
                          javelin::jmap::query::EmailListSortProperty::From);
        addPropertyAction(QStringLiteral("To"), javelin::jmap::query::EmailListSortProperty::To);
        addPropertyAction(QStringLiteral("Subject"),
                          javelin::jmap::query::EmailListSortProperty::Subject);
        addPropertyAction(QStringLiteral("Size"),
                          javelin::jmap::query::EmailListSortProperty::Size);
        menu.addSeparator();

        const auto addDirectionAction =
            [this, &menu](const QString& label,
                          const javelin::jmap::query::EmailListSortDirection direction)
        {
            auto* action = menu.addAction(label);
            action->setCheckable(true);
            action->setChecked(m_emailListSort.direction == direction);
            connect(action, &QAction::triggered, this,
                    [this, direction]
                    {
                        setEmailListSort(javelin::jmap::query::EmailListSort{
                            .property = m_emailListSort.property,
                            .direction = direction,
                        });
                    });
        };

        addDirectionAction(QStringLiteral("Descending"),
                           javelin::jmap::query::EmailListSortDirection::Descending);
        addDirectionAction(QStringLiteral("Ascending"),
                           javelin::jmap::query::EmailListSortDirection::Ascending);

        menu.exec(m_messageSortButton->mapToGlobal(QPoint{0, m_messageSortButton->height()}));
    }

    void MainWindow::setEmailListSort(javelin::jmap::query::EmailListSort sort)
    {
        if (m_emailListSort.property == sort.property &&
            m_emailListSort.direction == sort.direction)
        {
            return;
        }

        m_emailListSort = std::move(sort);
        for (auto& tabState : m_tabs)
        {
            std::visit(
                [](auto& content)
                {
                    if constexpr (!std::is_same_v<std::decay_t<decltype(content)>, ComposeTabState>)
                    {
                        content.page.offset = 0;
                        content.page.total.reset();
                        content.page.items.clear();
                        content.page.cacheLoaded = false;
                        content.page.stale = true;
                    }
                },
                tabState.content);
        }

        QSettings settings;
        settings.beginGroup(QLatin1StringView{windowGroup});
        settings.setValue(QLatin1StringView{emailListSortPropertyKey},
                          sortPropertySetting(m_emailListSort.property));
        settings.setValue(QLatin1StringView{emailListSortDirectionKey},
                          sortDirectionSetting(m_emailListSort.direction));
        settings.endGroup();
        settings.sync();

        updateSortButton();
        loadActiveTabFromCache(true);
        updateMessageListHeader();
        m_statusBar->showMessage(QStringLiteral("Sorting by %1, %2.")
                                     .arg(sortPropertyLabel(m_emailListSort.property),
                                          sortDirectionLabel(m_emailListSort.direction)),
                                 5000);
    }

    void MainWindow::goToPreviousPage()
    {
        auto* tab = activeTab();
        if (tab == nullptr)
        {
            return;
        }

        auto moveToPrevious = [](auto& content)
        {
            if constexpr (std::is_same_v<std::decay_t<decltype(content)>, ComposeTabState>)
            {
                return false;
            }
            if (content.page.offset >= MainWindow::pageSize)
            {
                content.page.offset -= MainWindow::pageSize;
                content.page.items.clear();
                content.page.cacheLoaded = false;
                content.selection = {};
                return true;
            }
            return false;
        };
        if (!std::visit(moveToPrevious, tab->content))
        {
            return;
        }

        loadActiveTabFromCache();
        refreshActiveTabFromServer();
    }

    void MainWindow::goToNextPage()
    {
        auto* tab = activeTab();
        if (tab == nullptr)
        {
            return;
        }

        auto moveToNext = [](auto& content)
        {
            if constexpr (std::is_same_v<std::decay_t<decltype(content)>, ComposeTabState>)
            {
                return false;
            }
            if (content.page.total.has_value() &&
                content.page.offset + MainWindow::pageSize >= *content.page.total)
            {
                return false;
            }

            content.page.offset += MainWindow::pageSize;
            content.page.items.clear();
            content.page.cacheLoaded = false;
            content.selection = {};
            return true;
        };
        if (!std::visit(moveToNext, tab->content))
        {
            return;
        }

        loadActiveTabFromCache();
        refreshActiveTabFromServer();
    }

    void MainWindow::syncActiveTabSelectionFromViews()
    {
        if (auto* tab = activeTab())
        {
            auto selection = TabSelectionState{
                .threadId = currentThreadId(*m_messageView),
                .emailId = currentEmailId(*m_messageView),
                .selectedEmailIds = selectedEmailIds(),
            };
            std::visit([&selection](auto& content) { content.selection = selection; },
                       tab->content);
        }
    }

    void MainWindow::reloadAccounts()
    {
        m_mailboxModel->refresh();
        m_mailboxView->expandAll();
        if (m_contactsAction != nullptr)
        {
            const auto result = m_contactRepository.listAccounts();
            const auto* accounts =
                std::get_if<std::vector<javelin::jmap::cache::ContactAccount>>(&result);
            m_contactsAction->setEnabled(accounts != nullptr && !accounts->empty());
        }
        if (m_calendarAction != nullptr)
        {
            const auto result = m_calendarService.accounts();
            const auto* accounts =
                std::get_if<std::vector<javelin::jmap::cache::CalendarAccount>>(&result);
            m_calendarAction->setEnabled(accounts != nullptr && !accounts->empty());
        }
    }

    void MainWindow::refreshViewsFromCache()
    {
        m_mailboxModel->refresh();
        m_mailboxView->expandAll();
        loadActiveTabFromCache(true);
    }

    void MainWindow::restoreSelection(std::optional<std::string> accountId,
                                      std::optional<std::string> mailboxId,
                                      std::optional<std::string> threadId,
                                      std::optional<std::string> emailId,
                                      const bool scrollToSelection)
    {
        if (accountId.has_value())
        {
            const auto mailboxIdValue =
                mailboxId.has_value() ? std::optional<QString>{QString::fromStdString(*mailboxId)}
                                      : std::optional<QString>{std::nullopt};
            const QModelIndex mailboxIndex = findMailboxIndexForSelection(
                *m_mailboxModel, QString::fromStdString(*accountId), mailboxIdValue);
            if (mailboxIndex.isValid())
            {
                m_mailboxView->setCurrentIndex(mailboxIndex);
                if (scrollToSelection)
                {
                    m_mailboxView->scrollTo(mailboxIndex);
                }
            }
        }

        const QModelIndex selectedMessageIndex = restoreMessageSelection(threadId, emailId);
        if (selectedMessageIndex.isValid())
        {
            m_messageView->setCurrentIndex(selectedMessageIndex);
            if (scrollToSelection)
            {
                m_messageView->scrollTo(selectedMessageIndex);
            }
        }
    }

    void MainWindow::restoreActiveTabMessageSelection(const std::optional<int> previousMessageRow)
    {
        const auto* tab = activeTab();
        if (tab == nullptr)
        {
            return;
        }

        const auto& selection = std::visit([](const auto& content) -> const TabSelectionState&
                                           { return content.selection; }, tab->content);
        restoreSelectionAfterMessageRefresh(activeAccountId(), activeMailboxId(),
                                            selection.threadId, selection.emailId,
                                            selection.selectedEmailIds, previousMessageRow);
    }

    void MainWindow::restoreSelectionAfterMessageRefresh(
        std::optional<std::string> accountId, std::optional<std::string> mailboxId,
        std::optional<std::string> threadId, std::optional<std::string> emailId,
        const std::vector<std::string>& selectedEmailIds,
        const std::optional<int> previousMessageRow)
    {
        Q_UNUSED(accountId);
        Q_UNUSED(mailboxId);
        auto* selectionModel = m_messageView->selectionModel();
        if (selectionModel != nullptr && !selectedEmailIds.empty())
        {
            QItemSelection restoredSelection;
            QModelIndex currentSelectionIndex;
            for (const auto& selectedEmailId : selectedEmailIds)
            {
                const QModelIndex index = findIndexByRole(
                    *m_messageModel, javelin::gui::messages::MessageListModel::EmailIdRole,
                    QString::fromStdString(selectedEmailId));
                if (!index.isValid())
                {
                    continue;
                }

                restoredSelection.select(index, index);
                if (emailId == std::optional<std::string>{selectedEmailId})
                {
                    currentSelectionIndex = index;
                }
            }

            if (!restoredSelection.isEmpty())
            {
                selectionModel->select(restoredSelection, QItemSelectionModel::ClearAndSelect |
                                                              QItemSelectionModel::Rows);
                if (!currentSelectionIndex.isValid())
                {
                    currentSelectionIndex = restoredSelection.indexes().constLast();
                }
                selectionModel->setCurrentIndex(currentSelectionIndex,
                                                QItemSelectionModel::NoUpdate);
                return;
            }
        }

        const QModelIndex selectedMessageIndex = restoreMessageSelection(threadId, emailId);
        if (selectedMessageIndex.isValid())
        {
            m_messageView->setCurrentIndex(selectedMessageIndex);
        }
        if (m_messageView->currentIndex().isValid() || !previousMessageRow.has_value())
        {
            return;
        }

        const auto fallbackRow = javelin::jmap::query::selectionFallbackIndexAfterRemoval(
            static_cast<std::size_t>(*previousMessageRow),
            static_cast<std::size_t>(m_messageModel->rowCount()));
        if (!fallbackRow.has_value())
        {
            return;
        }

        const QModelIndex fallbackIndex = m_messageModel->index(static_cast<int>(*fallbackRow), 0);
        if (!fallbackIndex.isValid())
        {
            return;
        }

        m_messageView->setCurrentIndex(fallbackIndex);
        m_messageView->scrollTo(fallbackIndex);
    }

    QModelIndex MainWindow::restoreMessageSelection(std::optional<std::string> threadId,
                                                    std::optional<std::string> emailId)
    {
        if (emailId.has_value())
        {
            const QModelIndex selectedMessageIndex = findIndexByRole(
                *m_messageModel, javelin::gui::messages::MessageListModel::EmailIdRole,
                QString::fromStdString(*emailId));
            if (selectedMessageIndex.isValid())
            {
                return selectedMessageIndex;
            }
        }

        if (threadId.has_value())
        {
            return findIndexByRole(*m_messageModel,
                                   javelin::gui::messages::MessageListModel::ThreadIdRole,
                                   QString::fromStdString(*threadId));
        }

        return {};
    }

    void MainWindow::refreshSelectionFromModels()
    {
        if (activeTabIsCompose())
        {
            updateMessageActions();
            return;
        }

        const auto accountId = activeAccountId();
        const auto mailboxId = activeMailboxId();
        auto selectedSummaries = selectedMessageSummaries();
        if (selectedSummaries.size() > 1)
        {
            m_messageViewContainer->setMultipleSelection(accountId, mailboxId,
                                                         std::move(selectedSummaries));
            syncActiveTabSelectionFromViews();
            updateEmptyStates();
            updateMessageListHeader();
            updateMessageActions();
            return;
        }

        const auto emailId = currentEmailId(*m_messageView);
        m_messageViewContainer->setSelection(m_messageViewService, accountId, mailboxId, emailId);
        syncActiveTabSelectionFromViews();
        updateEmptyStates();
        updateMessageListHeader();
        updateMessageActions();

        if (accountId.has_value() && emailId.has_value() &&
            !m_messageViewContainer->hasReadableBody())
        {
            m_messageViewContainer->setLoadingState(
                true, QStringLiteral("Checking your saved copy, then downloading the message if "
                                     "needed."));
            refreshSelectedMessageContent(*accountId, *emailId);
        }
    }

    void MainWindow::updateEmptyStates()
    {
        const bool hasMessages = m_messageModel->rowCount() > 0;
        const PageState* page = nullptr;
        if (const auto* tab = activeTab())
        {
            page = std::visit([](const auto& content) { return &content.page; }, tab->content);
        }

        if (page != nullptr && !page->refreshError.isEmpty())
        {
            m_messageEmptyState->setText(
                QStringLiteral("Could not refresh the message list.\n%1").arg(page->refreshError));
            m_messageEmptyState->setStyleSheet(QStringLiteral("color: #e58b8b;"));
        }
        else if (page != nullptr && page->refreshInFlight && !hasMessages)
        {
            m_messageEmptyState->setText(QStringLiteral("Loading messages..."));
            m_messageEmptyState->setStyleSheet(QString{});
        }
        else if (activeTabIsSearch())
        {
            m_messageEmptyState->setText(
                QStringLiteral("No server results matched your search in this account."));
            m_messageEmptyState->setStyleSheet(QString{});
        }
        else
        {
            m_messageEmptyState->setText(
                QStringLiteral("No messages are available for the selected mailbox yet."));
            m_messageEmptyState->setStyleSheet(QString{});
        }
        m_messageEmptyState->setVisible(!hasMessages ||
                                        (page != nullptr && !page->refreshError.isEmpty()));
        m_messageView->setVisible(true);
    }

    void MainWindow::updateMessageListHeader()
    {
        const auto* tab = activeTab();
        if (tab == nullptr)
        {
            m_messageListTitleLabel->setText(QStringLiteral("Messages"));
            m_messageListMetaLabel->clear();
            m_messagePageLabel->clear();
            m_previousPageButton->setEnabled(false);
            m_nextPageButton->setEnabled(false);
            return;
        }

        if (const auto* composeTab = std::get_if<ComposeTabState>(&tab->content))
        {
            m_messageListTitleLabel->setText(composeTab->title);
            m_messageListMetaLabel->setText(QStringLiteral("Compose"));
            m_messagePageLabel->clear();
            m_previousPageButton->setEnabled(false);
            m_nextPageButton->setEnabled(false);
            return;
        }

        std::visit(
            [this](const auto& content)
            {
                m_messageListTitleLabel->setText(content.title);
                if (content.page.total.has_value())
                {
                    m_messageListMetaLabel->setText(
                        activeTabIsSearch()
                            ? QStringLiteral("%1 Matches")
                                  .arg(static_cast<qulonglong>(*content.page.total))
                            : QStringLiteral("%1 Messages")
                                  .arg(static_cast<qulonglong>(*content.page.total)));
                    if (*content.page.total == 0)
                    {
                        m_messagePageLabel->setText(QStringLiteral("0-0"));
                    }
                    else
                    {
                        const auto start = content.page.offset + 1;
                        const auto end =
                            std::min(content.page.offset +
                                         static_cast<std::size_t>(m_messageModel->rowCount()),
                                     *content.page.total);
                        m_messagePageLabel->setText(QStringLiteral("%1-%2")
                                                        .arg(static_cast<qulonglong>(start))
                                                        .arg(static_cast<qulonglong>(end)));
                    }
                    m_previousPageButton->setEnabled(content.page.offset > 0);
                    m_nextPageButton->setEnabled(content.page.offset + pageSize <
                                                 *content.page.total);
                }
                else
                {
                    m_messageListMetaLabel->setText(
                        activeTabIsSearch()
                            ? QStringLiteral("%1 Matches").arg(m_messageModel->rowCount())
                            : QStringLiteral("%1 Messages").arg(m_messageModel->rowCount()));
                    m_messagePageLabel->clear();
                    m_previousPageButton->setEnabled(content.page.offset > 0);
                    m_nextPageButton->setEnabled(false);
                }
            },
            tab->content);
    }

    void MainWindow::updateMessageActions()
    {
        const auto selectedIds = selectedEmailIds();
        const bool hasEmailSelection =
            activeAccountId().has_value() && !selectedIds.empty() && !activeTabIsContacts();
        const bool hasMailboxSelection = activeTabIsMailbox() && activeAccountId().has_value() &&
                                         activeMailboxId().has_value() && !selectedIds.empty();
        const auto draftsMailbox =
            activeAccountId().has_value()
                ? findMailboxByRole(m_queryService, *activeAccountId(), "drafts")
                : std::optional<javelin::jmap::cache::MailboxTreeItem>{std::nullopt};
        const bool canEditDraft =
            activeTabIsMailbox() && activeMailboxId().has_value() && draftsMailbox.has_value() &&
            activeMailboxId() == std::optional<std::string>{draftsMailbox->id} &&
            selectedIds.size() == 1;
        const bool isUnread = indexIsUnread(m_messageView->currentIndex());
        m_newMessageAction->setEnabled(true);
        m_replyAction->setEnabled(hasEmailSelection && !activeTabIsCompose());
        m_replyAllAction->setEnabled(hasEmailSelection && !activeTabIsCompose());
        m_forwardAction->setEnabled(hasEmailSelection && !activeTabIsCompose());
        m_editDraftAction->setEnabled(canEditDraft);
        m_archiveAction->setEnabled(hasMailboxSelection);
        m_markUnreadAction->setEnabled(hasEmailSelection && !isUnread);
        m_deleteAction->setEnabled(hasMailboxSelection);
        m_permanentDeleteAction->setEnabled(hasEmailSelection);
        m_moveAction->setEnabled(hasMailboxSelection);
        m_copyAction->setEnabled(hasMailboxSelection);
        m_viewSourceAction->setEnabled(hasEmailSelection);
    }

    void MainWindow::updateSortButton()
    {
        if (m_messageSortButton == nullptr)
        {
            return;
        }

        m_messageSortButton->setToolTip(QStringLiteral("Sort messages: %1, %2")
                                            .arg(sortPropertyLabel(m_emailListSort.property),
                                                 sortDirectionLabel(m_emailListSort.direction)));
    }

    bool MainWindow::eventFilter(QObject* watched, QEvent* event)
    {
        if (watched == m_messageView->viewport() && event->type() == QEvent::MouseButtonPress)
        {
            const auto* mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::RightButton)
            {
                const QModelIndex index = m_messageView->indexAt(mouseEvent->position().toPoint());
                if (index.isValid() && m_messageView->selectionModel()->isSelected(index))
                {
                    return true;
                }
            }
        }

        if (watched == m_messageView && event->type() == QEvent::KeyPress)
        {
            const auto* keyEvent = static_cast<QKeyEvent*>(event);
            const QModelIndex currentIndex = m_messageView->currentIndex();
            if (!currentIndex.isValid())
            {
                return KXmlGuiWindow::eventFilter(watched, event);
            }

            const bool controlOnly = keyEvent->modifiers() == Qt::ControlModifier;
            if (controlOnly && (keyEvent->key() == Qt::Key_Up || keyEvent->key() == Qt::Key_Down))
            {
                const int direction = keyEvent->key() == Qt::Key_Up ? -1 : 1;
                const int nextRow = currentIndex.row() + direction;
                if (nextRow < 0 || nextRow >= m_messageModel->rowCount())
                {
                    return true;
                }

                const QModelIndex nextIndex = m_messageModel->index(nextRow, 0);
                if (!nextIndex.isValid())
                {
                    return true;
                }

                m_messageView->selectionModel()->setCurrentIndex(
                    nextIndex, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                m_messageView->scrollTo(nextIndex);
                return true;
            }

            const auto threadId =
                currentIndex.data(javelin::gui::messages::MessageListModel::ThreadIdRole)
                    .toString();
            if (threadId.isEmpty())
            {
                return KXmlGuiWindow::eventFilter(watched, event);
            }

            if (keyEvent->key() == Qt::Key_Right)
            {
                if (m_messageModel->setThreadExpanded(threadId.toStdString(), true))
                {
                    return true;
                }
            }

            if (keyEvent->key() == Qt::Key_Left)
            {
                if (m_messageModel->setThreadExpanded(threadId.toStdString(), false))
                {
                    const auto summaryEmailId =
                        m_messageModel->summaryEmailIdForThread(threadId.toStdString());
                    if (summaryEmailId.has_value())
                    {
                        restoreSelection(activeAccountId(), activeMailboxId(),
                                         std::optional<std::string>{threadId.toStdString()},
                                         summaryEmailId);
                    }
                    return true;
                }
            }
        }

        return KXmlGuiWindow::eventFilter(watched, event);
    }

    void MainWindow::saveAttachment(std::string accountId, std::string emailId, std::string partId)
    {
        const auto attachmentSettings =
            javelin::gui::settings::PreferencesDialog::loadAttachmentSaveSettings();
        if (!attachmentSettings.alwaysAsk && (attachmentSettings.directory.isEmpty() ||
                                              !QDir{attachmentSettings.directory}.exists()))
        {
            presentUserInterventionError(
                QStringLiteral("Select a valid attachment save directory in Preferences."));
            return;
        }

        m_statusBar->showMessage(QStringLiteral("Downloading attachment..."));
        auto task = m_mailService.requestAttachment(std::move(accountId), std::move(emailId),
                                                    std::move(partId));
        QCoro::connect(
            std::move(task), this,
            [this, attachmentSettings](javelin::jmap::AttachmentDownloadResult result)
            {
                if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&result))
                {
                    presentError(*error);
                    return;
                }

                const auto& download = std::get<javelin::jmap::AttachmentDownload>(result);
                const QString targetPath =
                    attachmentSettings.alwaysAsk
                        ? QFileDialog::getSaveFileName(this, QStringLiteral("Save Attachment"),
                                                       suggestedFileName(download))
                        : uniqueFilePath(attachmentSettings.directory, suggestedFileName(download));
                if (targetPath.isEmpty())
                {
                    m_statusBar->showMessage(QStringLiteral("Attachment save canceled."), 3000);
                    return;
                }

                m_statusBar->showMessage(QStringLiteral("Saving attachment..."));

                auto* watcher = new QFutureWatcher<FileWriteResult>(this);
                connect(watcher, &QFutureWatcher<FileWriteResult>::finished, this,
                        [this, watcher]
                        {
                            const auto writeResult = watcher->result();
                            if (!writeResult.errorMessage.isEmpty())
                            {
                                presentUserInterventionError(
                                    QStringLiteral("Failed to save attachment: %1")
                                        .arg(writeResult.errorMessage));
                            }
                            else
                            {
                                m_statusBar->showMessage(
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

    void MainWindow::saveAllAttachments(std::string accountId, std::string emailId)
    {
        const auto snapshotResult = m_messageViewService.load(accountId, emailId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&snapshotResult))
        {
            presentUserInterventionError(error->message);
            return;
        }

        const auto& snapshot =
            std::get<std::optional<javelin::jmap::cache::MessageViewSnapshot>>(snapshotResult);
        if (!snapshot.has_value())
        {
            m_statusBar->showMessage(QStringLiteral("The selected message is unavailable."), 5000);
            return;
        }

        const auto attachments = visibleDownloadableAttachments(*snapshot);
        if (attachments.empty())
        {
            m_statusBar->showMessage(QStringLiteral("No downloadable attachments are available."),
                                     5000);
            return;
        }

        const auto attachmentSettings =
            javelin::gui::settings::PreferencesDialog::loadAttachmentSaveSettings();
        const QString targetDirectory =
            attachmentSettings.alwaysAsk
                ? QFileDialog::getExistingDirectory(this, QStringLiteral("Save All Attachments"),
                                                    QDir::homePath())
                : attachmentSettings.directory;
        if (targetDirectory.isEmpty())
        {
            m_statusBar->showMessage(QStringLiteral("Save all attachments canceled."), 3000);
            return;
        }
        if (!QDir{targetDirectory}.exists())
        {
            presentUserInterventionError(
                QStringLiteral("Select a valid attachment save directory in Preferences."));
            return;
        }

        m_statusBar->showMessage(QStringLiteral("Downloading attachments..."));
        auto task = downloadAttachments(m_mailService, accountId, emailId, attachments);
        QCoro::connect(
            std::move(task), this,
            [this, targetDirectory](SaveAllDownloadResult result)
            {
                if (!result.errorMessage.isEmpty())
                {
                    presentUserInterventionError(result.errorMessage);
                    return;
                }

                m_statusBar->showMessage(QStringLiteral("Saving attachments..."));
                auto* watcher = new QFutureWatcher<BatchWriteResult>(this);
                connect(
                    watcher, &QFutureWatcher<BatchWriteResult>::finished, this,
                    [this, watcher]
                    {
                        const auto writeResult = watcher->result();
                        if (!writeResult.errorMessage.isEmpty())
                        {
                            presentUserInterventionError(
                                QStringLiteral("Failed to save attachments to %1: %2")
                                    .arg(writeResult.failedPath, writeResult.errorMessage));
                        }
                        else
                        {
                            m_statusBar->showMessage(
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

    void MainWindow::openAttachment(std::string accountId, std::string emailId, std::string partId)
    {
        if (!m_openAttachmentDirectory.isValid())
        {
            presentUserInterventionError(
                QStringLiteral("A temporary directory for attachments is unavailable."));
            return;
        }

        m_statusBar->showMessage(QStringLiteral("Downloading attachment..."));
        auto task = m_mailService.requestAttachment(std::move(accountId), std::move(emailId),
                                                    std::move(partId));
        QCoro::connect(
            std::move(task), this,
            [this](javelin::jmap::AttachmentDownloadResult result)
            {
                if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&result))
                {
                    presentError(*error);
                    return;
                }

                const auto& download = std::get<javelin::jmap::AttachmentDownload>(result);
                const QString targetPath = tempAttachmentPath(m_openAttachmentDirectory, download);
                m_statusBar->showMessage(QStringLiteral("Preparing attachment..."));

                auto* watcher = new QFutureWatcher<FileWriteResult>(this);
                connect(watcher, &QFutureWatcher<FileWriteResult>::finished, this,
                        [this, watcher]
                        {
                            const auto writeResult = watcher->result();
                            if (!writeResult.errorMessage.isEmpty())
                            {
                                presentUserInterventionError(
                                    QStringLiteral("Failed to prepare attachment: %1")
                                        .arg(writeResult.errorMessage));
                                watcher->deleteLater();
                                return;
                            }

                            const bool opened =
                                QDesktopServices::openUrl(QUrl::fromLocalFile(writeResult.path));
                            m_statusBar->showMessage(
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

    void MainWindow::openPreferences()
    {
        javelin::gui::settings::PreferencesDialog dialog{m_accountRepository, m_queryService, this};
        if (dialog.exec() == QDialog::Accepted)
        {
            m_statusBar->showMessage(QStringLiteral("Saved connection preferences."), 3000);
            Q_EMIT accountSettingsChanged();
            m_mailboxModel->refresh();
            const auto accountId = activeAccountId().has_value() ? activeAccountId()
                                                                 : currentAccountId(*m_mailboxView);
            if (accountId.has_value())
            {
                refreshAccountFromServer(*accountId);
            }
        }
    }

    void MainWindow::refreshFromServer()
    {
        const auto accountId =
            activeAccountId().has_value() ? activeAccountId() : currentAccountId(*m_mailboxView);
        if (!accountId.has_value())
        {
            presentUserInterventionError(QStringLiteral("Select an account to refresh."));
            return;
        }
        refreshAccountFromServer(*accountId);
    }

    void MainWindow::refreshAccountFromServer(std::string accountId)
    {
        if (!m_mailService.requestAccountSynchronization(accountId))
        {
            refreshConnectionSettings(
                javelin::gui::settings::PreferencesDialog::loadSettingsForAccount(
                    QString::fromStdString(accountId)));
            return;
        }
        m_statusBar->showMessage(QStringLiteral("Synchronizing account..."));
    }

    void MainWindow::refreshConnectionSettings(javelin::gui::settings::ConnectionSettings settings)
    {
        if (m_refreshInFlight)
        {
            return;
        }

        if (settings.loginEmail.isEmpty() || settings.apiKey.isEmpty())
        {
            presentUserInterventionError(
                QStringLiteral("Set Session URL, Login Email, and API Key in Preferences first."));
            return;
        }

        m_refreshInFlight = true;
        m_refreshAction->setEnabled(false);
        m_preferencesAction->setEnabled(false);
        m_statusBar->showMessage(QStringLiteral("Refreshing mail from server..."));
        qInfo().noquote() << "GUI refresh requested" << settings.loginEmail << settings.sessionUrl;

        std::vector<std::string> mailboxIds;
        for (const auto& accountId : std::as_const(settings.cachedAccountIds))
        {
            const auto syncedMailboxIds =
                javelin::gui::settings::PreferencesDialog::syncedMailboxIds(accountId);
            for (const auto& mailboxId : syncedMailboxIds)
            {
                mailboxIds.push_back(mailboxId.toStdString());
            }
        }
        auto task = m_mailService.bootstrapAccount(javelin::app::AccountBootstrapIntent{
            .settings = toAccountConnectionSettings(settings),
            .mailboxIds = std::move(mailboxIds),
        });
        QCoro::connect(
            std::move(task), this,
            [this, settings](javelin::jmap::LiveRefreshResult result)
            {
                m_refreshInFlight = false;
                m_refreshAction->setEnabled(true);
                m_preferencesAction->setEnabled(true);

                if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&result))
                {
                    qWarning().noquote() << "GUI refresh failed" << error->message;
                    presentError(*error);
                    return;
                }

                const auto& summary = std::get<javelin::jmap::LiveRefreshSummary>(result);
                javelin::gui::settings::PreferencesDialog::saveResolvedSessionUrl(
                    settings.id, QString::fromStdString(summary.resolvedSessionUrl));
                const auto ownedAccounts = m_accountRepository.listOwnedBy(summary.accountId);
                if (const auto* accounts =
                        std::get_if<std::vector<javelin::jmap::cache::CachedAccount>>(
                            &ownedAccounts))
                {
                    for (const auto& account : *accounts)
                    {
                        javelin::gui::settings::PreferencesDialog::associateCachedAccount(
                            settings.id, QString::fromStdString(account.accountId));
                    }
                }
                qInfo().noquote() << "GUI refresh succeeded"
                                  << QString::fromStdString(summary.accountId)
                                  << static_cast<qulonglong>(summary.mailboxCount)
                                  << static_cast<qulonglong>(summary.emailCount);
                if (summary.selectedMailboxId.has_value())
                {
                    markTabsStaleForAccount(summary.accountId,
                                            std::string_view{*summary.selectedMailboxId});
                }
                else
                {
                    markTabsStaleForAccount(summary.accountId);
                }
                reloadAccounts();
                refreshViewsFromCache();
                m_statusBar->showMessage(
                    QStringLiteral("Synced %1 mailboxes and %2 messages for %3.")
                        .arg(summary.mailboxCount)
                        .arg(summary.emailCount)
                        .arg(QString::fromStdString(summary.accountId)),
                    10000);
                Q_EMIT accountSettingsChanged();
                auto contactsTask = m_mailService.requestContacts(summary.accountId);
                QCoro::connect(
                    std::move(contactsTask), this,
                    [this](javelin::jmap::contacts::ContactRefreshResult contactsResult)
                    {
                        if (const auto* error =
                                std::get_if<javelin::jmap::LiveRefreshError>(&contactsResult))
                        {
                            qWarning().noquote() << "Contacts refresh failed" << error->message;
                            return;
                        }
                        reloadAccounts();
                        const auto& contacts =
                            std::get<javelin::jmap::contacts::ContactRefreshSummary>(
                                contactsResult);
                        qInfo() << "Contacts cache refreshed" << contacts.contactCount;
                    });
            });
    }

    void MainWindow::refreshSelectedMessageContent(std::string accountId, std::string emailId)
    {
        if (m_messageContentRequestInFlight.has_value() &&
            m_messageContentRequestInFlight->accountId == accountId &&
            m_messageContentRequestInFlight->emailId == emailId)
        {
            qDebug().noquote() << "GUI message content refresh already in flight"
                               << QString::fromStdString(emailId);
            return;
        }

        const auto requestToken = m_nextMessageContentRequestToken++;
        m_messageContentRequestInFlight = MessageContentRequestState{
            .accountId = accountId,
            .emailId = emailId,
            .token = requestToken,
        };

        auto task = m_mailService.requestMessageContent(accountId, emailId);
        QCoro::connect(
            std::move(task), this,
            [this, accountId = std::move(accountId), emailId = std::move(emailId),
             requestToken](javelin::jmap::MessageContentRefreshResult result)
            {
                const bool isCurrentRequest =
                    m_messageContentRequestInFlight.has_value() &&
                    m_messageContentRequestInFlight->token == requestToken &&
                    m_messageContentRequestInFlight->accountId == accountId &&
                    m_messageContentRequestInFlight->emailId == emailId;
                if (!isCurrentRequest)
                {
                    qDebug().noquote() << "GUI message content refresh ignored stale completion"
                                       << QString::fromStdString(emailId);
                    return;
                }

                m_messageContentRequestInFlight.reset();
                if (const auto* unavailable =
                        std::get_if<javelin::jmap::MessageContentUnavailable>(&result))
                {
                    markTabsStaleForAccount(accountId);
                    refreshActiveTabFromServer();
                    const QString message =
                        unavailable->message + QStringLiteral(" Refreshing the current view…");
                    m_messageViewContainer->setErrorState(message);
                    m_statusBar->showMessage(message, 10000);
                    qWarning().noquote()
                        << "GUI message content unavailable" << unavailable->message;
                    return;
                }

                if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&result))
                {
                    m_messageViewContainer->setErrorState(error->message);
                    qWarning().noquote() << "GUI message content refresh failed" << error->message;
                    presentError(*error);
                    return;
                }

                const auto currentAccount = activeAccountId();
                const auto selectedEmails = selectedEmailIds();
                if (currentAccount != std::optional<std::string>{accountId} ||
                    selectedEmails.size() != 1 || selectedEmails.front() != emailId)
                {
                    return;
                }

                m_messageViewContainer->setLoadingState(false);
                m_messageViewContainer->refresh(m_messageViewService);
                updateEmptyStates();
                updateMessageListHeader();
                updateMessageActions();

                const auto& summary = std::get<javelin::jmap::MessageContentRefreshSummary>(result);
                qInfo().noquote() << "GUI message content refresh succeeded"
                                  << QString::fromStdString(summary.emailId)
                                  << static_cast<qulonglong>(summary.partCount)
                                  << static_cast<qulonglong>(summary.bodyValueCount)
                                  << summary.usedCachedContent;
                if (!summary.usedCachedContent)
                {
                    m_statusBar->showMessage(QStringLiteral("Message ready."), 5000);
                }
            });
    }

    std::vector<std::string> MainWindow::selectedEmailIds() const
    {
        std::vector<std::string> emailIds;
        std::unordered_set<std::string> seen;
        const auto* selectionModel = m_messageView->selectionModel();
        if (selectionModel != nullptr)
        {
            const QModelIndexList indexes = selectionModel->selectedRows();
            emailIds.reserve(static_cast<std::size_t>(indexes.size()));
            for (const auto& index : indexes)
            {
                const auto emailId =
                    index.data(javelin::gui::messages::MessageListModel::EmailIdRole)
                        .toString()
                        .toStdString();
                if (!emailId.empty() && seen.insert(emailId).second)
                {
                    emailIds.push_back(emailId);
                }
            }
        }

        if (emailIds.empty())
        {
            if (const auto emailId = currentEmailId(*m_messageView); emailId.has_value())
            {
                emailIds.push_back(*emailId);
            }
        }

        return emailIds;
    }

    std::variant<std::vector<std::string>, QString>
    MainWindow::selectedEmailIdsForMailboxAction(const std::string_view accountId) const
    {
        std::vector<QModelIndex> indexes;
        const auto* selectionModel = m_messageView->selectionModel();
        if (selectionModel != nullptr)
        {
            const QModelIndexList selectedRows = selectionModel->selectedRows();
            indexes.reserve(static_cast<std::size_t>(selectedRows.size()));
            for (const auto& index : selectedRows)
            {
                if (index.isValid())
                {
                    indexes.push_back(index);
                }
            }
        }

        if (indexes.empty() && m_messageView->currentIndex().isValid())
        {
            indexes.push_back(m_messageView->currentIndex());
        }

        std::ranges::sort(indexes, [](const QModelIndex& left, const QModelIndex& right)
                          { return left.row() < right.row(); });

        std::vector<std::string> emailIds;
        std::unordered_set<std::string> seen;
        const auto appendEmailId = [&emailIds, &seen](std::string emailId)
        {
            if (!emailId.empty() && seen.insert(emailId).second)
            {
                emailIds.push_back(std::move(emailId));
            }
        };

        for (const auto& index : indexes)
        {
            const auto emailId = index.data(javelin::gui::messages::MessageListModel::EmailIdRole)
                                     .toString()
                                     .toStdString();
            const auto threadId = index.data(javelin::gui::messages::MessageListModel::ThreadIdRole)
                                      .toString()
                                      .toStdString();
            const auto rowKind = static_cast<javelin::gui::messages::MessageListModel::RowKind>(
                index.data(javelin::gui::messages::MessageListModel::RowKindRole).toInt());
            const auto threadMessageCount =
                index.data(javelin::gui::messages::MessageListModel::ThreadMessageCountRole)
                    .toULongLong();
            const bool isCollapsedThreadSummary =
                rowKind == javelin::gui::messages::MessageListModel::RowKind::ThreadSummary &&
                !index.data(javelin::gui::messages::MessageListModel::IsExpandedRole).toBool() &&
                threadMessageCount > 1;
            if (!isCollapsedThreadSummary)
            {
                appendEmailId(emailId);
                continue;
            }

            const auto threadMessagesResult =
                m_queryService.listThreadMessages(accountId, threadId);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&threadMessagesResult))
            {
                return error->message;
            }

            const auto& threadMessages =
                std::get<std::vector<javelin::jmap::cache::MessageListItem>>(threadMessagesResult);
            if (threadMessages.empty())
            {
                appendEmailId(emailId);
                continue;
            }

            for (const auto& item : threadMessages)
            {
                appendEmailId(item.emailId);
            }
        }

        return emailIds;
    }

    std::vector<javelin::jmap::cache::MessageListItem> MainWindow::selectedMessageSummaries() const
    {
        std::vector<QModelIndex> indexes;
        const auto* selectionModel = m_messageView->selectionModel();
        if (selectionModel != nullptr)
        {
            const QModelIndexList selectedRows = selectionModel->selectedRows();
            indexes.reserve(static_cast<std::size_t>(selectedRows.size()));
            for (const auto& index : selectedRows)
            {
                if (index.isValid())
                {
                    indexes.push_back(index);
                }
            }
        }

        std::ranges::sort(indexes, [](const QModelIndex& left, const QModelIndex& right)
                          { return left.row() < right.row(); });

        std::vector<javelin::jmap::cache::MessageListItem> summaries;
        summaries.reserve(indexes.size());
        std::unordered_set<std::string> seen;
        for (const auto& index : indexes)
        {
            auto emailId = index.data(javelin::gui::messages::MessageListModel::EmailIdRole)
                               .toString()
                               .toStdString();
            if (emailId.empty() || !seen.insert(emailId).second)
            {
                continue;
            }

            const auto subject =
                index.data(javelin::gui::messages::MessageListModel::SubjectRole).toString();
            const auto preview =
                index.data(javelin::gui::messages::MessageListModel::PreviewRole).toString();
            summaries.push_back(javelin::jmap::cache::MessageListItem{
                .emailId = std::move(emailId),
                .threadId = index.data(javelin::gui::messages::MessageListModel::ThreadIdRole)
                                .toString()
                                .toStdString(),
                .subject = subject.isEmpty() ? std::nullopt
                                             : std::optional<std::string>{subject.toStdString()},
                .preview = preview.isEmpty() ? std::nullopt
                                             : std::optional<std::string>{preview.toStdString()},
                .receivedAt = index.data(javelin::gui::messages::MessageListModel::ReceivedAtRole)
                                  .toString()
                                  .toStdString(),
                .sentAt = std::nullopt,
                .threadMessageCount =
                    index.data(javelin::gui::messages::MessageListModel::ThreadMessageCountRole)
                        .toULongLong(),
                .hasAttachment =
                    index.data(javelin::gui::messages::MessageListModel::HasAttachmentRole)
                        .toBool(),
                .isUnread =
                    index.data(javelin::gui::messages::MessageListModel::IsUnreadRole).toBool(),
                .isFlagged =
                    index.data(javelin::gui::messages::MessageListModel::IsFlaggedRole).toBool(),
                .from = std::nullopt,
            });
        }

        return summaries;
    }

    void MainWindow::selectMessageAlone(const QString& emailId)
    {
        if (emailId.isEmpty())
        {
            return;
        }

        const QModelIndex index = findIndexByRole(
            *m_messageModel, javelin::gui::messages::MessageListModel::EmailIdRole, emailId);
        if (!index.isValid())
        {
            return;
        }

        auto* selectionModel = m_messageView->selectionModel();
        selectionModel->select(index,
                               QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        m_messageView->setCurrentIndex(index);
        m_messageView->scrollTo(index);
        refreshSelectionFromModels();
    }

    void MainWindow::archiveSelectedEmail()
    {
        const auto accountId = activeAccountId();
        const auto mailboxId = activeMailboxId();
        if (!accountId.has_value() || !mailboxId.has_value())
        {
            m_statusBar->showMessage(QStringLiteral("Select a message to archive."), 3000);
            return;
        }

        auto emailIdsResult = selectedEmailIdsForMailboxAction(*accountId);
        if (const auto* error = std::get_if<QString>(&emailIdsResult))
        {
            m_statusBar->showMessage(*error, 10000);
            return;
        }

        auto emailIds = std::get<std::vector<std::string>>(std::move(emailIdsResult));
        if (emailIds.empty())
        {
            m_statusBar->showMessage(QStringLiteral("Select a message to archive."), 3000);
            return;
        }

        queueArchiveEmails(*accountId, *mailboxId, std::move(emailIds));
    }

    void MainWindow::deleteSelectedEmail()
    {
        const auto accountId = activeAccountId();
        const auto mailboxId = activeMailboxId();
        if (!accountId.has_value() || !mailboxId.has_value())
        {
            m_statusBar->showMessage(QStringLiteral("Select a message to delete."), 3000);
            return;
        }

        auto emailIdsResult = selectedEmailIdsForMailboxAction(*accountId);
        if (const auto* error = std::get_if<QString>(&emailIdsResult))
        {
            m_statusBar->showMessage(*error, 10000);
            return;
        }

        auto emailIds = std::get<std::vector<std::string>>(std::move(emailIdsResult));
        if (emailIds.empty())
        {
            m_statusBar->showMessage(QStringLiteral("Select a message to delete."), 3000);
            return;
        }

        const auto trashMailbox = findMailboxByRole(m_queryService, *accountId, "trash");
        if (trashMailbox.has_value() && trashMailbox->id == *mailboxId)
        {
            const auto count = emailIds.size();
            const auto prompt =
                count == 1 ? QStringLiteral(
                                 "Permanently delete the selected message? This cannot be undone.")
                           : QStringLiteral(
                                 "Permanently delete %1 selected messages? This cannot be undone.")
                                 .arg(count);
            if (QMessageBox::warning(this, QStringLiteral("Delete Permanently"), prompt,
                                     QMessageBox::Yes | QMessageBox::Cancel,
                                     QMessageBox::Cancel) != QMessageBox::Yes)
            {
                return;
            }
            queueDestroyEmails(*accountId, std::move(emailIds));
            return;
        }

        queueDeleteEmails(*accountId, *mailboxId, std::move(emailIds));
    }

    void MainWindow::permanentlyDeleteSelectedEmail()
    {
        const auto accountId = activeAccountId();
        if (!accountId.has_value())
        {
            m_statusBar->showMessage(QStringLiteral("Select a message to delete."), 3000);
            return;
        }

        auto emailIdsResult = selectedEmailIdsForMailboxAction(*accountId);
        if (const auto* error = std::get_if<QString>(&emailIdsResult))
        {
            m_statusBar->showMessage(*error, 10000);
            return;
        }

        auto emailIds = std::get<std::vector<std::string>>(std::move(emailIdsResult));
        if (emailIds.empty())
        {
            m_statusBar->showMessage(QStringLiteral("Select a message to delete."), 3000);
            return;
        }

        const auto count = emailIds.size();
        const auto prompt =
            count == 1
                ? QStringLiteral("Permanently delete the selected message? This cannot be undone.")
                : QStringLiteral("Permanently delete %1 selected messages? This cannot be undone.")
                      .arg(count);
        if (QMessageBox::warning(this, QStringLiteral("Delete Permanently"), prompt,
                                 QMessageBox::Yes | QMessageBox::Cancel,
                                 QMessageBox::Cancel) != QMessageBox::Yes)
        {
            return;
        }

        queueDestroyEmails(*accountId, std::move(emailIds));
    }

    void MainWindow::showMoveMenu()
    {
        const auto accountId = activeAccountId();
        const auto sourceMailboxId = activeMailboxId();
        if (!accountId.has_value() || !sourceMailboxId.has_value())
        {
            m_statusBar->showMessage(QStringLiteral("Select a message to move."), 3000);
            return;
        }

        auto emailIdsResult = selectedEmailIdsForMailboxAction(*accountId);
        if (const auto* error = std::get_if<QString>(&emailIdsResult))
        {
            m_statusBar->showMessage(*error, 10000);
            return;
        }

        auto emailIds = std::get<std::vector<std::string>>(std::move(emailIdsResult));
        if (emailIds.empty())
        {
            m_statusBar->showMessage(QStringLiteral("Select a message to move."), 3000);
            return;
        }

        QMenu menu{this};
        menu.setTitle(QStringLiteral("Move to"));

        const auto mailboxesResult = m_queryService.listMailboxTree(*accountId);
        const auto* mailboxes =
            std::get_if<std::vector<javelin::jmap::cache::MailboxTreeItem>>(&mailboxesResult);
        if (mailboxes != nullptr)
        {
            for (const auto& mailbox : *mailboxes)
            {
                if (mailbox.id == *sourceMailboxId)
                {
                    continue;
                }

                auto* action = menu.addAction(QString::fromStdString(mailbox.name));
                connect(action, &QAction::triggered, this,
                        [this, accountId = *accountId, sourceMailboxId = *sourceMailboxId,
                         destinationMailboxId = mailbox.id, emailIds]
                        {
                            queueMoveEmails(accountId, sourceMailboxId, destinationMailboxId,
                                            emailIds, QStringLiteral("Queued move."));
                        });
            }
        }
        if (menu.actions().empty())
        {
            m_statusBar->showMessage(QStringLiteral("No destination mailboxes available."), 3000);
            return;
        }

        menu.exec(QCursor::pos());
    }

    void MainWindow::showCopyMenu()
    {
        const auto accountId = activeAccountId();
        const auto sourceMailboxId = activeMailboxId();
        if (!accountId.has_value() || !sourceMailboxId.has_value())
        {
            m_statusBar->showMessage(QStringLiteral("Select a message to copy."), 3000);
            return;
        }

        auto emailIdsResult = selectedEmailIdsForMailboxAction(*accountId);
        if (const auto* error = std::get_if<QString>(&emailIdsResult))
        {
            m_statusBar->showMessage(*error, 10000);
            return;
        }

        auto emailIds = std::get<std::vector<std::string>>(std::move(emailIdsResult));
        if (emailIds.empty())
        {
            m_statusBar->showMessage(QStringLiteral("Select a message to copy."), 3000);
            return;
        }

        QMenu menu{this};
        menu.setTitle(QStringLiteral("Copy to"));

        const auto mailboxesResult = m_queryService.listMailboxTree(*accountId);
        const auto* mailboxes =
            std::get_if<std::vector<javelin::jmap::cache::MailboxTreeItem>>(&mailboxesResult);
        if (mailboxes != nullptr)
        {
            for (const auto& mailbox : *mailboxes)
            {
                if (mailbox.id == *sourceMailboxId)
                {
                    continue;
                }

                auto* action = menu.addAction(QString::fromStdString(mailbox.name));
                connect(action, &QAction::triggered, this,
                        [this, accountId = *accountId, sourceMailboxId = *sourceMailboxId,
                         destinationMailboxId = mailbox.id, emailIds]
                        {
                            queueCopyEmails(accountId, sourceMailboxId, destinationMailboxId,
                                            emailIds, QStringLiteral("Queued copy."));
                        });
            }
        }
        if (menu.actions().empty())
        {
            m_statusBar->showMessage(QStringLiteral("No destination mailboxes available."), 3000);
            return;
        }

        menu.exec(QCursor::pos());
    }

    void MainWindow::queueArchiveEmail(std::string accountId, std::string mailboxId,
                                       std::string emailId)
    {
        const auto archiveMailbox = findMailboxByRole(m_queryService, accountId, "archive");
        if (!archiveMailbox.has_value())
        {
            m_statusBar->showMessage(QStringLiteral("No Archive mailbox is available."), 5000);
            return;
        }

        queueMoveEmail(std::move(accountId), std::move(mailboxId), archiveMailbox->id,
                       std::move(emailId), QStringLiteral("Queued archive."));
    }

    void MainWindow::queueArchiveEmails(std::string accountId, std::string mailboxId,
                                        std::vector<std::string> emailIds)
    {
        const auto archiveMailbox = findMailboxByRole(m_queryService, accountId, "archive");
        if (!archiveMailbox.has_value())
        {
            m_statusBar->showMessage(QStringLiteral("No Archive mailbox is available."), 5000);
            return;
        }

        queueMoveEmails(std::move(accountId), std::move(mailboxId), archiveMailbox->id,
                        std::move(emailIds), QStringLiteral("Queued archive."));
    }

    void MainWindow::queueDeleteEmail(std::string accountId, std::string mailboxId,
                                      std::string emailId)
    {
        const auto trashMailbox = findMailboxByRole(m_queryService, accountId, "trash");
        if (!trashMailbox.has_value())
        {
            m_statusBar->showMessage(QStringLiteral("No Trash mailbox is available."), 5000);
            return;
        }

        queueMoveEmail(std::move(accountId), std::move(mailboxId), trashMailbox->id,
                       std::move(emailId), QStringLiteral("Queued delete."));
    }

    void MainWindow::queueDeleteEmails(std::string accountId, std::string mailboxId,
                                       std::vector<std::string> emailIds)
    {
        const auto trashMailbox = findMailboxByRole(m_queryService, accountId, "trash");
        if (!trashMailbox.has_value())
        {
            m_statusBar->showMessage(QStringLiteral("No Trash mailbox is available."), 5000);
            return;
        }

        queueMoveEmails(std::move(accountId), std::move(mailboxId), trashMailbox->id,
                        std::move(emailIds), QStringLiteral("Queued delete."));
    }

    void MainWindow::queueDestroyEmails(std::string accountId, std::vector<std::string> emailIds)
    {
        const auto selectedCount = emailIds.size();
        qCInfo(logUserOperations) << "permanently delete requested" << selectedCount
                                  << "message(s)";
        for (const auto& emailId : emailIds)
        {
            const auto result = m_mailService.queueDestroyEmail(accountId, emailId);
            if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&result))
            {
                presentError(*error);
                return;
            }
        }

        refreshMessageListPreservingSelection();
        refreshSelectionFromModels();
        updateEmptyStates();
        updateMessageListHeader();
        m_statusBar->showMessage(
            selectedCount == 1
                ? QStringLiteral("Queued permanent deletion.")
                : QStringLiteral("Queued permanent deletion for %1 messages.").arg(selectedCount),
            5000);
        submitQueuedEmailMutations(std::move(accountId));
    }

    void MainWindow::queueMoveEmails(std::string accountId, std::string sourceMailboxId,
                                     std::string destinationMailboxId,
                                     std::vector<std::string> emailIds, QString successMessage)
    {
        const auto selectedCount = emailIds.size();
        qCInfo(logUserOperations).noquote()
            << "move requested" << selectedCount << "message(s) from"
            << QString::fromStdString(sourceMailboxId) << "to"
            << QString::fromStdString(destinationMailboxId);
        for (const auto& emailId : emailIds)
        {
            const auto result = m_mailService.queueMoveEmail(accountId, emailId, sourceMailboxId,
                                                             destinationMailboxId);
            if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&result))
            {
                presentError(*error);
                return;
            }
        }

        refreshMessageListPreservingSelection();
        refreshSelectionFromModels();
        updateEmptyStates();
        updateMessageListHeader();
        if (selectedCount > 1)
        {
            if (successMessage.endsWith(QLatin1Char('.')))
            {
                successMessage.chop(1);
            }
            m_statusBar->showMessage(
                QStringLiteral("%1 for %2 messages.").arg(successMessage).arg(selectedCount), 5000);
        }
        else
        {
            m_statusBar->showMessage(std::move(successMessage), 5000);
        }

        submitQueuedEmailMutations(std::move(accountId));
    }

    void MainWindow::queueCopyEmails(std::string accountId, std::string sourceMailboxId,
                                     std::string destinationMailboxId,
                                     std::vector<std::string> emailIds, QString successMessage)
    {
        const auto selectedCount = emailIds.size();
        qCInfo(logUserOperations).noquote()
            << "copy requested" << selectedCount << "message(s) from"
            << QString::fromStdString(sourceMailboxId) << "to"
            << QString::fromStdString(destinationMailboxId);
        for (const auto& emailId : emailIds)
        {
            const auto result = m_mailService.queueCopyEmail(accountId, emailId, sourceMailboxId,
                                                             destinationMailboxId);
            if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&result))
            {
                presentError(*error);
                return;
            }
        }

        refreshMessageListPreservingSelection();
        refreshSelectionFromModels();
        updateEmptyStates();
        updateMessageListHeader();
        if (selectedCount > 1)
        {
            if (successMessage.endsWith(QLatin1Char('.')))
            {
                successMessage.chop(1);
            }
            m_statusBar->showMessage(
                QStringLiteral("%1 for %2 messages.").arg(successMessage).arg(selectedCount), 5000);
        }
        else
        {
            m_statusBar->showMessage(std::move(successMessage), 5000);
        }

        submitQueuedEmailMutations(std::move(accountId));
    }

    void MainWindow::queueMoveEmail(std::string accountId, std::string sourceMailboxId,
                                    std::string destinationMailboxId, std::string emailId,
                                    QString successMessage)
    {
        queueMoveEmails(std::move(accountId), std::move(sourceMailboxId),
                        std::move(destinationMailboxId), {std::move(emailId)},
                        std::move(successMessage));
    }

    void MainWindow::queueMarkEmailRead(std::string accountId, std::string emailId)
    {
        qCInfo(logUserOperations) << "mark read requested";
        const auto result = m_mailService.queueMarkEmailRead(accountId, emailId);
        if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&result))
        {
            presentError(*error);
            return;
        }

        refreshMessageListPreservingSelection();
        refreshSelectionFromModels();
        submitQueuedEmailMutations(std::move(accountId));
    }

    void MainWindow::toggleMessageFlagged(const QModelIndex& index)
    {
        const auto accountId = activeAccountId();
        if (!accountId.has_value() || !index.isValid())
        {
            return;
        }

        const auto emailId =
            index.data(javelin::gui::messages::MessageListModel::EmailIdRole).toString();
        if (emailId.isEmpty())
        {
            return;
        }

        const bool isFlagged =
            index.data(javelin::gui::messages::MessageListModel::IsFlaggedRole).toBool();
        qCInfo(logUserOperations) << (isFlagged ? "remove star requested" : "add star requested");
        const auto result =
            m_mailService.queueSetEmailFlagged(*accountId, emailId.toStdString(), !isFlagged);
        if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&result))
        {
            presentError(*error);
            return;
        }

        m_messageView->setCurrentIndex(index);
        refreshMessageListPreservingSelection();
        refreshSelectionFromModels();
        m_statusBar->showMessage(
            isFlagged ? QStringLiteral("Removed star.") : QStringLiteral("Added star."), 5000);
        submitQueuedEmailMutations(*accountId);
    }

    void MainWindow::markSelectedEmailUnread()
    {
        const auto accountId = activeAccountId();
        const auto emailId = currentEmailId(*m_messageView);
        if (!accountId.has_value() || !emailId.has_value())
        {
            m_statusBar->showMessage(QStringLiteral("Select a message to mark unread."), 3000);
            return;
        }

        if (indexIsUnread(m_messageView->currentIndex()))
        {
            return;
        }

        qCInfo(logUserOperations) << "mark unread requested";

        const auto result = m_mailService.queueMarkEmailUnread(*accountId, *emailId);
        if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&result))
        {
            presentError(*error);
            return;
        }

        refreshMessageListPreservingSelection();
        refreshSelectionFromModels();
        m_statusBar->showMessage(QStringLiteral("Marked unread."), 5000);
        submitQueuedEmailMutations(*accountId);
    }

    void MainWindow::findConversationsWithSender(const QModelIndex& index)
    {
        const auto accountId = activeAccountId();
        const auto senderEmail =
            index.data(javelin::gui::messages::MessageListModel::SenderEmailRole)
                .toString()
                .trimmed();
        if (!accountId.has_value() || senderEmail.isEmpty())
        {
            m_statusBar->showMessage(QStringLiteral("No sender address is available."), 5000);
            return;
        }

        openOrActivateSearchTab(
            *accountId,
            javelin::jmap::search::EmailSearchCriteria{.with = senderEmail.toStdString()}, true);
    }

    void MainWindow::showMessageListContextMenu(const QPoint& position)
    {
        const QModelIndex index = m_messageView->indexAt(position);
        if (!index.isValid())
        {
            return;
        }

        const auto accountId = activeAccountId();
        const auto sourceMailboxId = activeMailboxId();
        const auto clickedEmailId =
            index.data(javelin::gui::messages::MessageListModel::EmailIdRole)
                .toString()
                .toStdString();
        if (!accountId.has_value() || clickedEmailId.empty())
        {
            return;
        }

        if (!m_messageView->selectionModel()->isSelected(index))
        {
            m_messageView->selectionModel()->select(index, QItemSelectionModel::ClearAndSelect |
                                                               QItemSelectionModel::Rows);
            m_messageView->setCurrentIndex(index);
        }
        const auto emailIds = selectedEmailIds();

        QMenu menu{this};
        menu.addAction(m_viewSourceAction);
        menu.addAction(m_markUnreadAction);
        const auto senderEmail =
            index.data(javelin::gui::messages::MessageListModel::SenderEmailRole)
                .toString()
                .trimmed();
        if (!senderEmail.isEmpty())
        {
            auto* findSenderAction =
                menu.addAction(QStringLiteral("Find all conversations with %1").arg(senderEmail));
            connect(findSenderAction, &QAction::triggered, this,
                    [this, index] { findConversationsWithSender(index); });
        }
        if (sourceMailboxId.has_value())
        {
            menu.addSeparator();
            menu.addAction(m_archiveAction);
            menu.addAction(m_deleteAction);
            menu.addAction(m_permanentDeleteAction);
            menu.addSeparator();
            auto* moveMenu = menu.addMenu(QStringLiteral("Move to"));
            auto* copyMenu = menu.addMenu(QStringLiteral("Copy to"));

            const auto mailboxesResult = m_queryService.listMailboxTree(*accountId);
            const auto* mailboxes =
                std::get_if<std::vector<javelin::jmap::cache::MailboxTreeItem>>(&mailboxesResult);
            if (mailboxes != nullptr)
            {
                for (const auto& mailbox : *mailboxes)
                {
                    if (mailbox.id == *sourceMailboxId)
                    {
                        continue;
                    }

                    auto* action = moveMenu->addAction(QString::fromStdString(mailbox.name));
                    connect(action, &QAction::triggered, this,
                            [this, accountId = *accountId, sourceMailboxId = *sourceMailboxId,
                             destinationMailboxId = mailbox.id, emailIds]
                            {
                                queueMoveEmails(accountId, sourceMailboxId, destinationMailboxId,
                                                emailIds, QStringLiteral("Queued move."));
                            });
                    auto* copyAction = copyMenu->addAction(QString::fromStdString(mailbox.name));
                    connect(copyAction, &QAction::triggered, this,
                            [this, accountId = *accountId, sourceMailboxId = *sourceMailboxId,
                             destinationMailboxId = mailbox.id, emailIds]
                            {
                                queueCopyEmails(accountId, sourceMailboxId, destinationMailboxId,
                                                emailIds, QStringLiteral("Queued copy."));
                            });
                }
            }
            if (moveMenu->actions().empty())
            {
                moveMenu->setEnabled(false);
            }
            if (copyMenu->actions().empty())
            {
                copyMenu->setEnabled(false);
            }
        }

        menu.exec(m_messageView->viewport()->mapToGlobal(position));
    }

    void MainWindow::showMailboxContextMenu(const QPoint& position)
    {
        const QModelIndex index = m_mailboxView->indexAt(position);
        if (!index.isValid())
        {
            return;
        }

        const auto accountId =
            index.data(javelin::gui::mailboxes::MailboxTreeModel::AccountIdRole).toString();
        const auto mailboxId =
            index.data(javelin::gui::mailboxes::MailboxTreeModel::MailboxIdRole).toString();
        if (accountId.isEmpty())
        {
            return;
        }

        m_mailboxView->setCurrentIndex(index);

        QMenu menu{this};
        if (mailboxId.isEmpty())
        {
            auto* refreshAccountAction = menu.addAction(QStringLiteral("Refresh Account"));
            connect(refreshAccountAction, &QAction::triggered, this,
                    [this, account = accountId.toStdString()]
                    { refreshAccountFromServer(account); });
            menu.exec(m_mailboxView->viewport()->mapToGlobal(position));
            return;
        }
        auto* openAsTabAction = menu.addAction(QStringLiteral("Open as Tab"));
        connect(openAsTabAction, &QAction::triggered, this,
                [this] { openMailboxSelectionInTab(true); });
        menu.exec(m_mailboxView->viewport()->mapToGlobal(position));
    }

    void MainWindow::refreshMessageListPreservingSelection()
    {
        syncActiveTabSelectionFromViews();

        QSignalBlocker blocker{m_messageView->selectionModel()};
        loadActiveTabFromCache(true, false);
    }

    void MainWindow::submitQueuedEmailMutations(std::string accountId)
    {
        auto task = m_mailService.submitPendingEmailMutations(accountId);
        QCoro::connect(
            std::move(task), this,
            [this](javelin::jmap::SubmittedEmailMutationsResult submitResult)
            {
                if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&submitResult))
                {
                    presentError(*error);
                    return;
                }

                const auto& summary =
                    std::get<javelin::jmap::SubmittedEmailMutations>(submitResult);
                if (summary.failedEmailCount > 0)
                {
                    markTabsStaleForAccount(summary.accountId);
                    refreshActiveTabFromServer();
                    presentError(javelin::jmap::LiveRefreshError{
                        .message = QStringLiteral("The server rejected %1 email mutation(s). "
                                                  "The mailbox has been refreshed to restore "
                                                  "the server state.")
                                       .arg(summary.failedEmailCount),
                    });
                    return;
                }
                if (summary.updatedEmailCount == 0)
                {
                    return;
                }

                refreshMessageListPreservingSelection();
                refreshSelectionFromModels();
            });
    }

    void MainWindow::viewSelectedMessageSource()
    {
        const auto accountId = activeAccountId();
        const auto emailId = currentEmailId(*m_messageView);
        if (!accountId.has_value() || !emailId.has_value())
        {
            m_statusBar->showMessage(QStringLiteral("Select a message to view its source."), 3000);
            return;
        }

        if (!m_openAttachmentDirectory.isValid())
        {
            presentUserInterventionError(
                QStringLiteral("A temporary directory for message source files is unavailable."));
            return;
        }

        m_statusBar->showMessage(QStringLiteral("Downloading message source..."));
        auto task = m_mailService.requestMessageSource(*accountId, *emailId);
        QCoro::connect(
            std::move(task), this,
            [this](javelin::jmap::MessageSourceDownloadResult result)
            {
                if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&result))
                {
                    presentError(*error);
                    return;
                }

                const auto& download = std::get<javelin::jmap::MessageSourceDownload>(result);
                const QString targetPath =
                    tempMessageSourcePath(m_openAttachmentDirectory, download);
                m_statusBar->showMessage(QStringLiteral("Preparing message source..."));

                auto* watcher = new QFutureWatcher<FileWriteResult>(this);
                connect(watcher, &QFutureWatcher<FileWriteResult>::finished, this,
                        [this, watcher]
                        {
                            const auto writeResult = watcher->result();
                            if (!writeResult.errorMessage.isEmpty())
                            {
                                presentUserInterventionError(
                                    QStringLiteral("Failed to prepare message source: %1")
                                        .arg(writeResult.errorMessage));
                                watcher->deleteLater();
                                return;
                            }

                            const bool opened =
                                QDesktopServices::openUrl(QUrl::fromLocalFile(writeResult.path));
                            m_statusBar->showMessage(
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

    void MainWindow::restorePersistentState()
    {
        QSettings settings;
        settings.beginGroup(QLatin1StringView{windowGroup});

        if (const auto geometry = settings.value(QLatin1StringView{geometryKey}).toByteArray();
            !geometry.isEmpty())
        {
            restoreGeometry(geometry);
        }

        if (const auto splitterState = settings.value(QLatin1StringView{splitterKey}).toByteArray();
            !splitterState.isEmpty())
        {
            m_mainSplitter->restoreState(splitterState);
        }

        m_emailListSort.property =
            sortPropertyFromSetting(settings
                                        .value(QLatin1StringView{emailListSortPropertyKey},
                                               sortPropertySetting(m_emailListSort.property))
                                        .toString());
        m_emailListSort.direction =
            sortDirectionFromSetting(settings
                                         .value(QLatin1StringView{emailListSortDirectionKey},
                                                sortDirectionSetting(m_emailListSort.direction))
                                         .toString());
        updateSortButton();

        const auto activeTabIndexValue =
            settings.value(QLatin1StringView{activeTabIndexKey}, 0).toInt();
        const auto tabCount = settings.beginReadArray(QLatin1StringView{tabsKey});
        m_tabs.clear();
        m_tabs.reserve(static_cast<std::size_t>(std::max(0, tabCount)));
        for (int tabIndex = 0; tabIndex < tabCount; ++tabIndex)
        {
            settings.setArrayIndex(tabIndex);
            const auto type = settings.value(QStringLiteral("type")).toString();
            const auto accountId = settings.value(QStringLiteral("accountId")).toString();
            if (type.isEmpty() || accountId.isEmpty())
            {
                continue;
            }

            if (type == QStringLiteral("mailbox"))
            {
                restoreMailboxTab(settings, accountId);
                continue;
            }

            if (type == QStringLiteral("search"))
            {
                restoreSearchTab(settings, accountId);
                continue;
            }

            if (type == QStringLiteral("compose"))
            {
                restoreComposeTab(settings);
                continue;
            }

            if (type == QStringLiteral("calendar"))
            {
                openCalendar();
                if (!m_tabs.empty())
                {
                    if (auto* calendarTab = std::get_if<CalendarTabState>(&m_tabs.back().content))
                    {
                        const auto month = QDate::fromString(
                            settings.value(QStringLiteral("displayedMonth")).toString(),
                            Qt::ISODate);
                        if (month.isValid() && calendarTab->widget != nullptr)
                            calendarTab->widget->setDisplayedMonth(month);
                    }
                }
            }
        }
        settings.endArray();

        settings.endGroup();
        if (m_tabs.empty())
        {
            for (int rootRow = 0; rootRow < m_mailboxModel->rowCount(); ++rootRow)
            {
                const auto rootIndex = m_mailboxModel->index(rootRow, 0);
                if (!rootIndex.isValid() || m_mailboxModel->rowCount(rootIndex) <= 0)
                {
                    continue;
                }

                const auto mailboxIndex = m_mailboxModel->index(0, 0, rootIndex);
                if (!mailboxIndex.isValid())
                {
                    continue;
                }

                m_mailboxView->setCurrentIndex(mailboxIndex);
                activateMailboxSelection(true);
                break;
            }
            return;
        }

        m_activeTabIndex = std::clamp(activeTabIndexValue, 0, static_cast<int>(m_tabs.size() - 1));
        updateTabBar();
        activateTab(*m_activeTabIndex, false);
        for (std::size_t index = 0; index < m_tabs.size(); ++index)
        {
            refreshTabFromServer(index);
        }
    }

    void MainWindow::restoreMailboxTab(const QSettings& settings, const QString& accountId)
    {
        const auto mailboxId = settings.value(QStringLiteral("mailboxId")).toString();
        if (mailboxId.isEmpty())
        {
            return;
        }

        m_tabs.push_back(TabState{
            .content =
                MailboxTabState{
                    .accountId = accountId.toStdString(),
                    .mailboxId = mailboxId.toStdString(),
                    .title = settings.value(QStringLiteral("title"), mailboxId).toString(),
                    .role = optionalStringSetting(settings, QStringLiteral("mailboxRole")),
                    .page =
                        PageState{
                            .offset = static_cast<std::size_t>(
                                settings.value(QStringLiteral("offset"), 0).toULongLong()),
                            .total = std::nullopt,
                            .items = {},
                            .cacheLoaded = false,
                            .refreshInFlight = false,
                            .stale = false,
                            .refreshError = {},
                        },
                    .selection =
                        TabSelectionState{
                            .threadId = optionalStringSetting(settings, QStringLiteral("threadId")),
                            .emailId = optionalStringSetting(settings, QStringLiteral("emailId")),
                            .selectedEmailIds = {},
                        },
                    .observationId = {},
                },
        });
    }

    void MainWindow::restoreSearchTab(const QSettings& settings, const QString& accountId)
    {
        const auto query = settings.value(QStringLiteral("query")).toString();
        auto criteria = searchCriteriaFromSettings(settings);
        auto items = cachedSearchItems(settings);

        m_tabs.push_back(
            TabState{
                .content =
                    SearchTabState{
                        .accountId = accountId.toStdString(),
                        .query = query.toStdString(),
                        .criteria = std::move(criteria),
                        .title = settings
                                     .value(QStringLiteral("title"),
                                            QStringLiteral("Search: %1").arg(query))
                                     .toString(),
                        .page =
                            PageState{
                                .offset = static_cast<std::size_t>(
                                    settings.value(QStringLiteral("offset"), 0).toULongLong()),
                                .total = settings.value(QStringLiteral("total")).isValid()
                                             ? std::optional<std::size_t>{static_cast<std::size_t>(
                                                   settings.value(QStringLiteral("total"))
                                                       .toULongLong())}
                                             : std::nullopt,
                                .items = std::move(items),
                                .cacheLoaded = true,
                                .refreshInFlight = false,
                                .stale = false,
                                .refreshError = {},
                            },
                        .selection =
                            TabSelectionState{
                                .threadId = optionalStringSetting(settings,
                                                                  QStringLiteral("threadId")),
                                .emailId =
                                    optionalStringSetting(settings, QStringLiteral("emailId")),
                                .selectedEmailIds = {},
                            },
                    },
            });
    }

    void MainWindow::restoreComposeTab(const QSettings& settings)
    {
        const auto composeSessionId = settings.value(QStringLiteral("composeSessionId")).toString();
        if (composeSessionId.isEmpty())
        {
            return;
        }

        const auto draftResult = m_composeService.loadWorkingCopy(composeSessionId.toStdString());
        if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&draftResult))
        {
            presentError(*error);
            return;
        }

        const auto& snapshot =
            std::get<std::optional<javelin::jmap::submission::DraftSnapshot>>(draftResult);
        if (!snapshot.has_value())
        {
            return;
        }

        m_tabs.push_back(TabState{
            .content =
                ComposeTabState{
                    .accountId = snapshot->accountId,
                    .composeSessionId = snapshot->composeSessionId,
                    .title = settings.value(QStringLiteral("title"), QStringLiteral("Compose"))
                                 .toString(),
                    .widget = nullptr,
                    .page = {},
                    .selection = {},
                },
        });
        auto* widget = new javelin::gui::compose::ComposeTabWidget(
            m_composeService, m_identityRepository, m_contactIdentityLookup, *snapshot,
            m_contentStack);
        attachComposeWidget(widget, static_cast<int>(m_tabs.size() - 1));
    }

    void MainWindow::savePersistentState() const
    {
        QSettings settings;
        settings.beginGroup(QLatin1StringView{windowGroup});
        settings.setValue(QLatin1StringView{geometryKey}, saveGeometry());
        settings.setValue(QLatin1StringView{splitterKey}, m_mainSplitter->saveState());
        settings.setValue(QLatin1StringView{activeTabIndexKey}, m_activeTabIndex.value_or(0));
        settings.setValue(QLatin1StringView{emailListSortPropertyKey},
                          sortPropertySetting(m_emailListSort.property));
        settings.setValue(QLatin1StringView{emailListSortDirectionKey},
                          sortDirectionSetting(m_emailListSort.direction));
        settings.beginWriteArray(QLatin1StringView{tabsKey});
        for (int tabIndex = 0; tabIndex < static_cast<int>(m_tabs.size()); ++tabIndex)
        {
            settings.setArrayIndex(tabIndex);
            const auto& tab = m_tabs[static_cast<std::size_t>(tabIndex)];
            writePersistentTab(settings, tab);
        }
        settings.endArray();
        settings.endGroup();
        settings.sync();
    }

    void MainWindow::writePersistentTab(QSettings& settings, const TabState& tab) const
    {
        std::visit(
            [&settings](const auto& content)
            {
                writeCommonTabSettings(settings, content.accountId, content.title,
                                       content.page.offset, content.selection.threadId,
                                       content.selection.emailId);
                if constexpr (std::is_same_v<std::decay_t<decltype(content)>, MailboxTabState>)
                {
                    settings.setValue(QStringLiteral("type"), QStringLiteral("mailbox"));
                    settings.setValue(QStringLiteral("mailboxId"),
                                      QString::fromStdString(content.mailboxId));
                    settings.setValue(QStringLiteral("mailboxRole"),
                                      content.role.has_value()
                                          ? QString::fromStdString(*content.role)
                                          : QString{});
                }
                else if constexpr (std::is_same_v<std::decay_t<decltype(content)>, SearchTabState>)
                {
                    settings.setValue(QStringLiteral("type"), QStringLiteral("search"));
                    settings.setValue(QStringLiteral("query"),
                                      QString::fromStdString(content.query));
                    writeOptionalSearchField(settings, QStringLiteral("searchText"),
                                             content.criteria.text);
                    writeOptionalSearchField(settings, QStringLiteral("searchWith"),
                                             content.criteria.with);
                    writeOptionalSearchField(settings, QStringLiteral("searchFrom"),
                                             content.criteria.from);
                    writeOptionalSearchField(settings, QStringLiteral("searchTo"),
                                             content.criteria.to);
                    writeOptionalSearchField(settings, QStringLiteral("searchCc"),
                                             content.criteria.cc);
                    writeOptionalSearchField(settings, QStringLiteral("searchBcc"),
                                             content.criteria.bcc);
                    writeOptionalSearchField(settings, QStringLiteral("searchSubject"),
                                             content.criteria.subject);
                    writeOptionalSearchField(settings, QStringLiteral("searchBody"),
                                             content.criteria.body);
                    if (content.page.total.has_value())
                    {
                        settings.setValue(QStringLiteral("total"),
                                          static_cast<qulonglong>(*content.page.total));
                    }
                    else
                    {
                        settings.remove(QStringLiteral("total"));
                    }

                    QJsonArray itemsArray;
                    for (const auto& item : content.page.items)
                    {
                        itemsArray.push_back(serializeMessageListItem(item));
                    }
                    settings.setValue(QStringLiteral("cachedItems"),
                                      QJsonDocument{itemsArray}.toJson(QJsonDocument::Compact));
                }
                else if constexpr (std::is_same_v<std::decay_t<decltype(content)>, ComposeTabState>)
                {
                    settings.setValue(QStringLiteral("type"), QStringLiteral("compose"));
                    settings.setValue(QStringLiteral("composeSessionId"),
                                      QString::fromStdString(content.composeSessionId));
                }
                else if constexpr (std::is_same_v<std::decay_t<decltype(content)>,
                                                  CalendarTabState>)
                {
                    settings.setValue(QStringLiteral("type"), QStringLiteral("calendar"));
                    settings.setValue(QStringLiteral("displayedMonth"),
                                      content.widget != nullptr
                                          ? content.widget->displayedMonth().toString(Qt::ISODate)
                                          : QString{});
                }
                else
                {
                    settings.setValue(QStringLiteral("type"), QStringLiteral("contacts"));
                }
            },
            tab.content);
    }

    void MainWindow::closeEvent(QCloseEvent* event)
    {
        savePersistentState();
        KXmlGuiWindow::closeEvent(event);
    }

} // namespace javelin::gui::shell
