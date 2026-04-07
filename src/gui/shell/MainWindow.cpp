#include "gui/shell/MainWindow.h"

#include "gui/mailboxes/MailboxTreeModel.h"
#include "gui/messages/MessageListDelegate.h"
#include "gui/messages/MessageListModel.h"
#include "gui/messageview/MessageViewContainer.h"
#include "gui/settings/PreferencesDialog.h"
#include "jmap/JmapCore.h"
#include "jmap/cache/AccountRepository.h"
#include "jmap/cache/MessageViewService.h"
#include "jmap/cache/QueryService.h"

#include <QCoroTask>

#include <KActionCollection>
#include <KStandardAction>

#include <QAction>
#include <QCloseEvent>
#include <QDebug>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QMenuBar>
#include <QMetaObject>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QtConcurrent>

namespace javelin::gui::shell
{
    namespace
    {
        constexpr auto windowGroup = "mainWindow";
        constexpr auto geometryKey = "geometry";
        constexpr auto splitterKey = "splitterState";

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

        [[nodiscard]] javelin::jmap::LiveConnectionSettings
        toLiveConnectionSettings(const javelin::gui::settings::ConnectionSettings& settings)
        {
            return javelin::jmap::LiveConnectionSettings{
                .sessionUrl = settings.sessionUrl.toStdString(),
                .loginEmail = settings.loginEmail.toStdString(),
                .apiKey = settings.apiKey.toStdString(),
            };
        }

        struct FileWriteResult
        {
            QString path;
            QString errorMessage;
        };

        [[nodiscard]] QString sanitizedFileName(QString name, const QString& fallback)
        {
            name = name.trimmed();
            if (name.isEmpty())
            {
                name = fallback;
            }

            static const QRegularExpression invalidPattern{
                QStringLiteral(R"([\\/:*?"<>|\x00-\x1F])")};
            name.replace(invalidPattern, QStringLiteral("_"));
            return name.isEmpty() ? fallback : name;
        }

        [[nodiscard]] QString suggestedFileName(const javelin::jmap::AttachmentDownload& download)
        {
            QString fileName =
                QString::fromStdString(download.name.value_or("attachment-" + download.partId));
            fileName = sanitizedFileName(
                fileName,
                QStringLiteral("attachment-%1").arg(QString::fromStdString(download.partId)));

            if (!fileName.contains(QLatin1Char('.')))
            {
                const QMimeDatabase mimeDatabase;
                const auto mimeType =
                    mimeDatabase.mimeTypeForName(QString::fromStdString(download.mediaType));
                const auto suffix = mimeType.preferredSuffix();
                if (!suffix.isEmpty())
                {
                    fileName += QStringLiteral(".") + suffix;
                }
            }

            return fileName;
        }

        [[nodiscard]] FileWriteResult writePayloadToPath(const QString& path,
                                                         const QByteArray& payload)
        {
            QSaveFile file{path};
            if (!file.open(QIODevice::WriteOnly))
            {
                return FileWriteResult{
                    .path = path,
                    .errorMessage = file.errorString(),
                };
            }

            if (file.write(payload) != payload.size())
            {
                return FileWriteResult{
                    .path = path,
                    .errorMessage = file.errorString(),
                };
            }

            if (!file.commit())
            {
                return FileWriteResult{
                    .path = path,
                    .errorMessage = file.errorString(),
                };
            }

            return FileWriteResult{.path = path, .errorMessage = {}};
        }

        [[nodiscard]] QString tempAttachmentPath(QTemporaryDir& directory,
                                                 const javelin::jmap::AttachmentDownload& download)
        {
            return directory.filePath(QStringLiteral("%1-%2").arg(
                QString::fromStdString(download.emailId), suggestedFileName(download)));
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

    } // namespace

    MainWindow::MainWindow(javelin::jmap::JmapCore& jmapCore,
                           javelin::jmap::cache::AccountRepository& accountRepository,
                           javelin::jmap::cache::MessageViewService& messageViewService,
                           javelin::jmap::cache::QueryService& queryService, QWidget* parent)
        : KXmlGuiWindow(parent), m_jmapCore(jmapCore), m_accountRepository(accountRepository),
          m_messageViewService(messageViewService), m_queryService(queryService)
    {
        setupUi();
        createActions();
        setupGUI(KXmlGuiWindow::Default, QStringLiteral("javelinmailui.rc"));
        connectSelection();
        restorePersistentState();
    }

    void MainWindow::createActions()
    {
        m_refreshAction = new QAction(QIcon::fromTheme(QStringLiteral("view-refresh")),
                                      QStringLiteral("Refresh From Server"), this);
        m_refreshAction->setShortcut(QKeySequence::Refresh);
        connect(m_refreshAction, &QAction::triggered, this, &MainWindow::refreshFromServer);
        actionCollection()->addAction(QStringLiteral("refresh_from_server"), m_refreshAction);
        actionCollection()->setDefaultShortcut(m_refreshAction, QKeySequence::Refresh);

        m_preferencesAction =
            KStandardAction::preferences(this, &MainWindow::openPreferences, actionCollection());
    }

    void MainWindow::setupUi()
    {
        setWindowTitle(QStringLiteral("Javelin Mail"));

        m_mailboxModel = new javelin::gui::mailboxes::MailboxTreeModel(
            m_accountRepository, m_queryService, this);
        m_messageModel = new javelin::gui::messages::MessageListModel(m_queryService, this);

        m_mailboxView = new QTreeView(this);
        m_mailboxView->setModel(m_mailboxModel);
        m_mailboxView->setHeaderHidden(true);
        m_mailboxView->setExpandsOnDoubleClick(false);
        m_mailboxView->expandAll();

        m_messageView = new QListView(this);
        m_messageView->setModel(m_messageModel);
        m_messageView->setItemDelegate(
            new javelin::gui::messages::MessageListDelegate(m_messageView));
        m_messageView->setSpacing(6);
        m_messageView->setFrameShape(QFrame::NoFrame);
        m_messageView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        m_messageView->setSelectionMode(QAbstractItemView::SingleSelection);
        m_messageView->setStyleSheet(
            QStringLiteral("QListView { background: #26272c; border: none; padding: 3px; }"));

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
        m_messageQuickFilterButton->setText(QStringLiteral("Quick Filter"));
        m_messageQuickFilterButton->setEnabled(false);
        auto titleFont = m_messageListTitleLabel->font();
        titleFont.setPointSize(titleFont.pointSize() + 4);
        titleFont.setBold(true);
        m_messageListTitleLabel->setFont(titleFont);
        messageHeaderLayout->addWidget(m_messageListTitleLabel);
        messageHeaderLayout->addWidget(m_messageListMetaLabel);
        messageHeaderLayout->addStretch(1);
        messageHeaderLayout->addWidget(m_messageQuickFilterButton);
        m_messageEmptyState = new QLabel(
            QStringLiteral("No messages are available for the selected mailbox yet."), messagePane);
        m_messageEmptyState->setWordWrap(true);
        messageLayout->addWidget(messageHeader);
        messageLayout->addWidget(m_messageEmptyState);
        messageLayout->addWidget(m_messageView);

        m_messageViewContainer = new javelin::gui::messageview::MessageViewContainer(this);
        connect(m_messageViewContainer,
                &javelin::gui::messageview::MessageViewContainer::saveAttachmentRequested, this,
                [this](const QString& accountId, const QString& emailId, const QString& partId)
                {
                    saveAttachment(accountId.toStdString(), emailId.toStdString(),
                                   partId.toStdString());
                });
        connect(m_messageViewContainer,
                &javelin::gui::messageview::MessageViewContainer::openAttachmentRequested, this,
                [this](const QString& accountId, const QString& emailId, const QString& partId)
                {
                    openAttachment(accountId.toStdString(), emailId.toStdString(),
                                   partId.toStdString());
                });
        connect(m_messageViewContainer,
                &javelin::gui::messageview::MessageViewContainer::archiveRequested, this,
                [this](const QString& accountId, const QString& mailboxId, const QString& emailId)
                {
                    queueArchiveEmail(accountId.toStdString(), mailboxId.toStdString(),
                                      emailId.toStdString());
                });
        connect(m_messageViewContainer,
                &javelin::gui::messageview::MessageViewContainer::deleteRequested, this,
                [this](const QString& accountId, const QString& mailboxId, const QString& emailId)
                {
                    queueDeleteEmail(accountId.toStdString(), mailboxId.toStdString(),
                                     emailId.toStdString());
                });

        m_messageView->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_messageView, &QListView::customContextMenuRequested, this,
                &MainWindow::showMessageListContextMenu);

        m_mainSplitter = new QSplitter(Qt::Horizontal, this);
        m_mainSplitter->addWidget(m_mailboxView);
        m_mainSplitter->addWidget(messagePane);
        m_mainSplitter->addWidget(m_messageViewContainer);
        m_mainSplitter->setStretchFactor(0, 1);
        m_mainSplitter->setStretchFactor(1, 2);
        m_mainSplitter->setStretchFactor(2, 3);
        m_mainSplitter->setSizes({240, 420, 780});

        setCentralWidget(m_mainSplitter);
        statusBar()->showMessage(m_jmapCore.statusSummary());
        updateEmptyStates();
        updateMessageListHeader();
    }

    void MainWindow::connectSelection()
    {
        connect(m_mailboxView->selectionModel(), &QItemSelectionModel::currentChanged, this,
                [this](const QModelIndex& current, const QModelIndex&)
                {
                    const auto accountId = currentAccountId(*m_mailboxView);
                    const auto mailboxId = currentMailboxId(*m_mailboxView);

                    if (!current.isValid() || !accountId.has_value())
                    {
                        m_messageView->clearSelection();
                        m_messageModel->setMailboxContext(std::nullopt, std::nullopt);
                        m_messageViewContainer->setSelection(m_messageViewService, std::nullopt,
                                                             std::nullopt, std::nullopt);
                        updateEmptyStates();
                        updateMessageListHeader();
                        return;
                    }

                    // Account-level node selected — clear mailbox context.
                    if (!mailboxId.has_value())
                    {
                        m_messageView->clearSelection();
                        m_messageModel->setMailboxContext(accountId, std::nullopt);
                        m_messageViewContainer->setSelection(m_messageViewService, accountId,
                                                             std::nullopt, std::nullopt);
                        updateEmptyStates();
                        updateMessageListHeader();
                        return;
                    }

                    m_messageView->clearSelection();
                    m_messageModel->setMailboxContext(*accountId, *mailboxId);
                    m_messageViewContainer->setSelection(m_messageViewService, accountId,
                                                         *mailboxId, std::nullopt);
                    updateEmptyStates();
                    updateMessageListHeader();
                    refreshSelectedMailboxMessages(*accountId, *mailboxId);
                });

        connect(
            m_messageView->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex& current, const QModelIndex&)
            {
                const auto accountId = currentAccountId(*m_mailboxView);
                const auto mailboxId = currentMailboxId(*m_mailboxView);
                if (!accountId.has_value() || !mailboxId.has_value())
                {
                    m_messageViewContainer->setSelection(m_messageViewService, accountId, mailboxId,
                                                         std::nullopt);
                    return;
                }

                if (!current.isValid())
                {
                    m_messageViewContainer->setSelection(m_messageViewService, accountId, mailboxId,
                                                         std::nullopt);
                    return;
                }

                const auto emailId =
                    current.data(javelin::gui::messages::MessageListModel::EmailIdRole).toString();
                m_messageViewContainer->setSelection(
                    m_messageViewService, accountId, mailboxId,
                    emailId.isEmpty() ? std::optional<std::string>{std::nullopt}
                                      : std::optional<std::string>{emailId.toStdString()});
                updateEmptyStates();
                updateMessageListHeader();
                if (!emailId.isEmpty())
                {
                    refreshSelectedMessageContent(*accountId, emailId.toStdString());
                }
            });
    }

    void MainWindow::reloadAccounts()
    {
        m_mailboxModel->refresh();
        m_mailboxView->expandAll();
    }

    void MainWindow::refreshViewsFromCache()
    {
        m_mailboxModel->refresh();
        m_mailboxView->expandAll();
        m_messageModel->refresh();
        m_messageViewContainer->refresh(m_messageViewService);
        updateEmptyStates();
        updateMessageListHeader();
    }

    void MainWindow::restoreSelection(std::optional<std::string> mailboxId,
                                      std::optional<std::string> emailId)
    {
        if (mailboxId.has_value())
        {
            const QModelIndex mailboxIndex = findIndexByRole(
                *m_mailboxModel, javelin::gui::mailboxes::MailboxTreeModel::MailboxIdRole,
                QString::fromStdString(*mailboxId));
            if (mailboxIndex.isValid())
            {
                m_mailboxView->setCurrentIndex(mailboxIndex);
                m_mailboxView->scrollTo(mailboxIndex);
            }
        }

        if (emailId.has_value())
        {
            const QModelIndex emailIndex = findIndexByRole(
                *m_messageModel, javelin::gui::messages::MessageListModel::EmailIdRole,
                QString::fromStdString(*emailId));
            if (emailIndex.isValid())
            {
                m_messageView->setCurrentIndex(emailIndex);
                m_messageView->scrollTo(emailIndex);
            }
        }
    }

    void MainWindow::updateEmptyStates()
    {
        const bool hasMessages = m_messageModel->rowCount() > 0;
        m_messageEmptyState->setVisible(!hasMessages);
        m_messageView->setVisible(hasMessages);
    }

    void MainWindow::updateMessageListHeader()
    {
        const auto mailboxIndex = m_mailboxView->currentIndex();
        const auto mailboxName = mailboxIndex.isValid()
                                     ? mailboxIndex.data(Qt::DisplayRole).toString()
                                     : QStringLiteral("Messages");
        m_messageListTitleLabel->setText(mailboxName);
        m_messageListMetaLabel->setText(
            QStringLiteral("%1 Messages").arg(m_messageModel->rowCount()));
    }

    void MainWindow::saveAttachment(std::string accountId, std::string emailId, std::string partId)
    {
        const auto settings = javelin::gui::settings::PreferencesDialog::loadSettings();
        if (settings.sessionUrl.isEmpty() || settings.loginEmail.isEmpty() ||
            settings.apiKey.isEmpty())
        {
            statusBar()->showMessage(
                QStringLiteral("Set Session URL, Login Email, and API Key in Preferences first."),
                5000);
            return;
        }

        statusBar()->showMessage(QStringLiteral("Downloading attachment..."));
        auto task =
            m_jmapCore.downloadAttachment(toLiveConnectionSettings(settings), std::move(accountId),
                                          std::move(emailId), std::move(partId));
        QCoro::connect(
            std::move(task), this,
            [this](javelin::jmap::AttachmentDownloadResult result)
            {
                if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&result))
                {
                    statusBar()->showMessage(error->message, 10000);
                    return;
                }

                const auto& download = std::get<javelin::jmap::AttachmentDownload>(result);
                const QString targetPath = QFileDialog::getSaveFileName(
                    this, QStringLiteral("Save Attachment"), suggestedFileName(download));
                if (targetPath.isEmpty())
                {
                    statusBar()->showMessage(QStringLiteral("Attachment save canceled."), 3000);
                    return;
                }

                statusBar()->showMessage(QStringLiteral("Saving attachment..."));

                auto* watcher = new QFutureWatcher<FileWriteResult>(this);
                connect(watcher, &QFutureWatcher<FileWriteResult>::finished, this,
                        [this, watcher]
                        {
                            const auto writeResult = watcher->result();
                            if (!writeResult.errorMessage.isEmpty())
                            {
                                statusBar()->showMessage(
                                    QStringLiteral("Failed to save attachment: %1")
                                        .arg(writeResult.errorMessage),
                                    10000);
                            }
                            else
                            {
                                statusBar()->showMessage(
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

    void MainWindow::openAttachment(std::string accountId, std::string emailId, std::string partId)
    {
        const auto settings = javelin::gui::settings::PreferencesDialog::loadSettings();
        if (settings.sessionUrl.isEmpty() || settings.loginEmail.isEmpty() ||
            settings.apiKey.isEmpty())
        {
            statusBar()->showMessage(
                QStringLiteral("Set Session URL, Login Email, and API Key in Preferences first."),
                5000);
            return;
        }

        if (!m_openAttachmentDirectory.isValid())
        {
            statusBar()->showMessage(
                QStringLiteral("A temporary directory for attachments is unavailable."), 10000);
            return;
        }

        statusBar()->showMessage(QStringLiteral("Downloading attachment..."));
        auto task =
            m_jmapCore.downloadAttachment(toLiveConnectionSettings(settings), std::move(accountId),
                                          std::move(emailId), std::move(partId));
        QCoro::connect(
            std::move(task), this,
            [this](javelin::jmap::AttachmentDownloadResult result)
            {
                if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&result))
                {
                    statusBar()->showMessage(error->message, 10000);
                    return;
                }

                const auto& download = std::get<javelin::jmap::AttachmentDownload>(result);
                const QString targetPath = tempAttachmentPath(m_openAttachmentDirectory, download);
                statusBar()->showMessage(QStringLiteral("Preparing attachment..."));

                auto* watcher = new QFutureWatcher<FileWriteResult>(this);
                connect(watcher, &QFutureWatcher<FileWriteResult>::finished, this,
                        [this, watcher]
                        {
                            const auto writeResult = watcher->result();
                            if (!writeResult.errorMessage.isEmpty())
                            {
                                statusBar()->showMessage(
                                    QStringLiteral("Failed to prepare attachment: %1")
                                        .arg(writeResult.errorMessage),
                                    10000);
                                watcher->deleteLater();
                                return;
                            }

                            const bool opened =
                                QDesktopServices::openUrl(QUrl::fromLocalFile(writeResult.path));
                            statusBar()->showMessage(
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
        javelin::gui::settings::PreferencesDialog dialog{this};
        if (dialog.exec() == QDialog::Accepted)
        {
            statusBar()->showMessage(QStringLiteral("Saved connection preferences."), 3000);
            const auto settings = dialog.settings();
            if (!settings.sessionUrl.isEmpty() && !settings.loginEmail.isEmpty() &&
                !settings.apiKey.isEmpty())
            {
                refreshFromServer();
            }
        }
    }

    void MainWindow::refreshFromServer()
    {
        if (m_refreshInFlight)
        {
            return;
        }

        const auto settings = javelin::gui::settings::PreferencesDialog::loadSettings();
        if (settings.sessionUrl.isEmpty() || settings.loginEmail.isEmpty() ||
            settings.apiKey.isEmpty())
        {
            statusBar()->showMessage(
                QStringLiteral("Set Session URL, Login Email, and API Key in Preferences first."),
                5000);
            return;
        }

        m_refreshInFlight = true;
        m_refreshAction->setEnabled(false);
        m_preferencesAction->setEnabled(false);
        statusBar()->showMessage(QStringLiteral("Refreshing mail from server..."));
        qInfo() << "GUI refresh requested";

        auto task = m_jmapCore.refreshFromServer(
            toLiveConnectionSettings(settings),
            [this](const QString& message)
            {
                qInfo().noquote() << "GUI refresh progress" << message;
                QMetaObject::invokeMethod(
                    this, [this, message] { statusBar()->showMessage(message); },
                    Qt::QueuedConnection);
            });
        QCoro::connect(
            std::move(task), this,
            [this](javelin::jmap::LiveRefreshResult result)
            {
                m_refreshInFlight = false;
                m_refreshAction->setEnabled(true);
                m_preferencesAction->setEnabled(true);

                if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&result))
                {
                    qWarning().noquote() << "GUI refresh failed" << error->message;
                    statusBar()->showMessage(error->message, 10000);
                    return;
                }

                const auto& summary = std::get<javelin::jmap::LiveRefreshSummary>(result);
                const auto mailboxId = currentMailboxId(*m_mailboxView).has_value()
                                           ? currentMailboxId(*m_mailboxView)
                                           : summary.selectedMailboxId;
                const auto emailId = currentEmailId(*m_messageView);
                qInfo().noquote() << "GUI refresh succeeded"
                                  << QString::fromStdString(summary.accountId)
                                  << static_cast<qulonglong>(summary.mailboxCount)
                                  << static_cast<qulonglong>(summary.emailCount);
                reloadAccounts();
                refreshViewsFromCache();
                restoreSelection(mailboxId, emailId);
                statusBar()->showMessage(
                    QStringLiteral("Synced %1 mailboxes and %2 messages for %3.")
                        .arg(summary.mailboxCount)
                        .arg(summary.emailCount)
                        .arg(QString::fromStdString(summary.accountId)),
                    10000);
            });
    }

    void MainWindow::refreshSelectedMailboxMessages(std::string accountId, std::string mailboxId)
    {
        const auto settings = javelin::gui::settings::PreferencesDialog::loadSettings();
        if (settings.sessionUrl.isEmpty() || settings.loginEmail.isEmpty() ||
            settings.apiKey.isEmpty())
        {
            return;
        }

        qInfo().noquote() << "GUI mailbox refresh requested" << QString::fromStdString(accountId)
                          << QString::fromStdString(mailboxId);
        auto task = m_jmapCore.refreshMailboxMessages(
            toLiveConnectionSettings(settings), accountId, mailboxId,
            [this](const QString& message)
            {
                qInfo().noquote() << "GUI mailbox refresh progress" << message;
                QMetaObject::invokeMethod(
                    this, [this, message] { statusBar()->showMessage(message, 5000); },
                    Qt::QueuedConnection);
            });
        QCoro::connect(
            std::move(task), this,
            [this, accountId = std::move(accountId),
             mailboxId = std::move(mailboxId)](javelin::jmap::MailboxMessagesRefreshResult result)
            {
                if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&result))
                {
                    qWarning().noquote() << "GUI mailbox refresh failed" << error->message;
                    statusBar()->showMessage(error->message, 10000);
                    return;
                }

                const auto currentAccount = currentAccountId(*m_mailboxView);
                const auto currentMailbox = currentMailboxId(*m_mailboxView);
                if (currentAccount != std::optional<std::string>{accountId} ||
                    currentMailbox != std::optional<std::string>{mailboxId})
                {
                    return;
                }

                const auto selectedEmail = currentEmailId(*m_messageView);
                m_messageModel->refresh();
                restoreSelection(std::optional<std::string>{mailboxId}, selectedEmail);
                m_messageViewContainer->refresh(m_messageViewService);
                updateEmptyStates();

                const auto& summary =
                    std::get<javelin::jmap::MailboxMessagesRefreshSummary>(result);
                qInfo().noquote() << "GUI mailbox refresh succeeded"
                                  << QString::fromStdString(summary.accountId)
                                  << QString::fromStdString(summary.mailboxId)
                                  << static_cast<qulonglong>(summary.emailCount);
                statusBar()->showMessage(
                    QStringLiteral("Loaded %1 messages for the selected mailbox.")
                        .arg(summary.emailCount),
                    5000);
            });
    }

    void MainWindow::refreshSelectedMessageContent(std::string accountId, std::string emailId)
    {
        const auto settings = javelin::gui::settings::PreferencesDialog::loadSettings();
        if (settings.sessionUrl.isEmpty() || settings.loginEmail.isEmpty() ||
            settings.apiKey.isEmpty())
        {
            return;
        }

        auto task = m_jmapCore.refreshMessageContent(
            toLiveConnectionSettings(settings), accountId, emailId,
            [this](const QString& message)
            {
                qInfo().noquote() << "GUI message content progress" << message;
                QMetaObject::invokeMethod(
                    this, [this, message] { statusBar()->showMessage(message, 5000); },
                    Qt::QueuedConnection);
            });
        QCoro::connect(
            std::move(task), this,
            [this, accountId = std::move(accountId),
             emailId = std::move(emailId)](javelin::jmap::MessageContentRefreshResult result)
            {
                if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&result))
                {
                    qWarning().noquote() << "GUI message content refresh failed" << error->message;
                    statusBar()->showMessage(error->message, 10000);
                    return;
                }

                const auto currentAccount = currentAccountId(*m_mailboxView);
                const auto selectedEmail = currentEmailId(*m_messageView);
                if (currentAccount != std::optional<std::string>{accountId} ||
                    selectedEmail != std::optional<std::string>{emailId})
                {
                    return;
                }

                m_messageViewContainer->refresh(m_messageViewService);

                const auto& summary = std::get<javelin::jmap::MessageContentRefreshSummary>(result);
                qInfo().noquote() << "GUI message content refresh succeeded"
                                  << QString::fromStdString(summary.emailId)
                                  << static_cast<qulonglong>(summary.partCount)
                                  << static_cast<qulonglong>(summary.bodyValueCount)
                                  << summary.usedCachedContent;
                if (!summary.usedCachedContent)
                {
                    statusBar()->showMessage(QStringLiteral("Loaded message content for %1.")
                                                 .arg(QString::fromStdString(summary.emailId)),
                                             5000);
                }
            });
    }

    void MainWindow::queueArchiveEmail(std::string accountId, std::string mailboxId,
                                       std::string emailId)
    {
        const auto archiveMailbox = findMailboxByRole(m_queryService, accountId, "archive");
        if (!archiveMailbox.has_value())
        {
            statusBar()->showMessage(QStringLiteral("No Archive mailbox is available."), 5000);
            return;
        }

        queueMoveEmail(std::move(accountId), std::move(mailboxId), archiveMailbox->id,
                       std::move(emailId), QStringLiteral("Queued archive."));
    }

    void MainWindow::queueDeleteEmail(std::string accountId, std::string mailboxId,
                                      std::string emailId)
    {
        const auto trashMailbox = findMailboxByRole(m_queryService, accountId, "trash");
        if (!trashMailbox.has_value())
        {
            statusBar()->showMessage(QStringLiteral("No Trash mailbox is available."), 5000);
            return;
        }

        queueMoveEmail(std::move(accountId), std::move(mailboxId), trashMailbox->id,
                       std::move(emailId), QStringLiteral("Queued delete."));
    }

    void MainWindow::queueMoveEmail(std::string accountId, std::string sourceMailboxId,
                                    std::string destinationMailboxId, std::string emailId,
                                    QString successMessage)
    {
        const auto result = m_jmapCore.queueMoveEmail(accountId, emailId, sourceMailboxId,
                                                      destinationMailboxId);
        if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&result))
        {
            statusBar()->showMessage(error->message, 10000);
            return;
        }

        const auto currentAccount = currentAccountId(*m_mailboxView);
        const auto currentMailbox = currentMailboxId(*m_mailboxView);
        const auto currentEmail = currentEmailId(*m_messageView);
        m_messageModel->refresh();
        if (currentAccount == std::optional<std::string>{accountId} &&
            currentMailbox == std::optional<std::string>{sourceMailboxId} &&
            currentEmail == std::optional<std::string>{emailId})
        {
            m_messageView->clearSelection();
            m_messageViewContainer->setSelection(m_messageViewService, currentAccount,
                                                 currentMailbox, std::nullopt);
        }
        else
        {
            m_messageViewContainer->refresh(m_messageViewService);
        }
        updateEmptyStates();
        updateMessageListHeader();
        statusBar()->showMessage(std::move(successMessage), 5000);

        const auto settings = javelin::gui::settings::PreferencesDialog::loadSettings();
        if (settings.sessionUrl.isEmpty() || settings.loginEmail.isEmpty() ||
            settings.apiKey.isEmpty())
        {
            return;
        }

        auto task = m_jmapCore.submitPendingEmailMutations(toLiveConnectionSettings(settings),
                                                           accountId);
        QCoro::connect(
            std::move(task), this,
            [this](javelin::jmap::SubmittedEmailMutationsResult submitResult)
            {
                if (const auto* error =
                        std::get_if<javelin::jmap::LiveRefreshError>(&submitResult))
                {
                    statusBar()->showMessage(error->message, 10000);
                    return;
                }

                const auto& summary =
                    std::get<javelin::jmap::SubmittedEmailMutations>(submitResult);
                if (summary.updatedEmailCount > 0)
                {
                    m_messageModel->refresh();
                    m_messageViewContainer->refresh(m_messageViewService);
                    updateEmptyStates();
                    updateMessageListHeader();
                }
            });
    }

    void MainWindow::showMessageListContextMenu(const QPoint& position)
    {
        const QModelIndex index = m_messageView->indexAt(position);
        if (!index.isValid())
        {
            return;
        }

        const auto accountId = currentAccountId(*m_mailboxView);
        const auto sourceMailboxId = currentMailboxId(*m_mailboxView);
        const auto emailId = index.data(javelin::gui::messages::MessageListModel::EmailIdRole)
                                 .toString()
                                 .toStdString();
        if (!accountId.has_value() || !sourceMailboxId.has_value() || emailId.empty())
        {
            return;
        }

        m_messageView->setCurrentIndex(index);

        QMenu menu{this};
        auto* archiveAction = menu.addAction(QStringLiteral("Archive"));
        auto* deleteAction = menu.addAction(QStringLiteral("Delete"));
        menu.addSeparator();
        auto* moveMenu = menu.addMenu(QStringLiteral("Move to"));

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
                         destinationMailboxId = mailbox.id, emailId]
                        {
                            queueMoveEmail(accountId, sourceMailboxId, destinationMailboxId, emailId,
                                           QStringLiteral("Queued move."));
                        });
            }
        }
        if (moveMenu->actions().empty())
        {
            moveMenu->setEnabled(false);
        }

        connect(archiveAction, &QAction::triggered, this,
                [this, accountId = *accountId, sourceMailboxId = *sourceMailboxId, emailId]
                { queueArchiveEmail(accountId, sourceMailboxId, emailId); });
        connect(deleteAction, &QAction::triggered, this,
                [this, accountId = *accountId, sourceMailboxId = *sourceMailboxId, emailId]
                { queueDeleteEmail(accountId, sourceMailboxId, emailId); });

        menu.exec(m_messageView->viewport()->mapToGlobal(position));
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

        settings.endGroup();
    }

    void MainWindow::savePersistentState() const
    {
        QSettings settings;
        settings.beginGroup(QLatin1StringView{windowGroup});
        settings.setValue(QLatin1StringView{geometryKey}, saveGeometry());
        settings.setValue(QLatin1StringView{splitterKey}, m_mainSplitter->saveState());
        settings.endGroup();
        settings.sync();
    }

    void MainWindow::closeEvent(QCloseEvent* event)
    {
        savePersistentState();
        KXmlGuiWindow::closeEvent(event);
    }

} // namespace javelin::gui::shell
