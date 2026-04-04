#include "gui/shell/MainWindow.h"

#include "gui/mailboxes/MailboxTreeModel.h"
#include "gui/messages/MessageListModel.h"
#include "gui/messageview/MessageViewContainer.h"
#include "gui/settings/PreferencesDialog.h"
#include "jmap/JmapCore.h"
#include "jmap/cache/AccountRepository.h"
#include "jmap/cache/MessageViewService.h"
#include "jmap/cache/QueryService.h"

#include <QCoroTask>

#include <QAction>
#include <QCloseEvent>
#include <QComboBox>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWidget>

namespace javelin::gui::shell
{
    namespace
    {
        constexpr auto windowGroup = "mainWindow";
        constexpr auto geometryKey = "geometry";
        constexpr auto splitterKey = "splitterState";
        constexpr auto accountIdKey = "selectedAccountId";

        [[nodiscard]] std::optional<std::string> currentAccountId(const QComboBox& accountCombo)
        {
            const auto accountId = accountCombo.currentData().toString();
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

        [[nodiscard]] javelin::jmap::LiveConnectionSettings
        toLiveConnectionSettings(const javelin::gui::settings::ConnectionSettings& settings)
        {
            return javelin::jmap::LiveConnectionSettings{
                .sessionUrl = settings.sessionUrl.toStdString(),
                .loginEmail = settings.loginEmail.toStdString(),
                .apiKey = settings.apiKey.toStdString(),
            };
        }

    } // namespace

    MainWindow::MainWindow(javelin::jmap::JmapCore& jmapCore,
                           javelin::jmap::cache::AccountRepository& accountRepository,
                           javelin::jmap::cache::MessageViewService& messageViewService,
                           javelin::jmap::cache::QueryService& queryService, QWidget* parent)
        : QMainWindow(parent), m_jmapCore(jmapCore), m_accountRepository(accountRepository),
          m_messageViewService(messageViewService), m_queryService(queryService)
    {
        setupUi();
        createMenus();
        connectSelection();
        restorePersistentState();
    }

    void MainWindow::createMenus()
    {
        auto* accountMenu = menuBar()->addMenu(QStringLiteral("&Account"));
        m_refreshAction = accountMenu->addAction(QStringLiteral("Refresh From Server"));
        connect(m_refreshAction, &QAction::triggered, this, &MainWindow::refreshFromServer);

        auto* settingsMenu = menuBar()->addMenu(QStringLiteral("&Settings"));
        m_preferencesAction = settingsMenu->addAction(QStringLiteral("Preferences..."));
        connect(m_preferencesAction, &QAction::triggered, this, &MainWindow::openPreferences);
    }

    void MainWindow::setupUi()
    {
        setWindowTitle(QStringLiteral("Javelin Mail"));
        resize(1440, 900);

        m_mailboxModel = new javelin::gui::mailboxes::MailboxTreeModel(m_queryService, this);
        m_messageModel = new javelin::gui::messages::MessageListModel(m_queryService, this);

        m_accountCombo = new QComboBox(this);

        m_mailboxView = new QTreeView(this);
        m_mailboxView->setModel(m_mailboxModel);
        m_mailboxView->setHeaderHidden(true);

        m_messageView = new QListView(this);
        m_messageView->setModel(m_messageModel);

        auto* messagePane = new QWidget(this);
        auto* messageLayout = new QVBoxLayout(messagePane);
        messageLayout->setContentsMargins(0, 0, 0, 0);
        messageLayout->setSpacing(8);
        m_messageEmptyState = new QLabel(
            QStringLiteral("No messages are available for the selected mailbox yet."), messagePane);
        m_messageEmptyState->setWordWrap(true);
        messageLayout->addWidget(m_messageEmptyState);
        messageLayout->addWidget(m_messageView);

        m_messageViewContainer = new javelin::gui::messageview::MessageViewContainer(this);

        m_mainSplitter = new QSplitter(Qt::Horizontal, this);
        m_mainSplitter->addWidget(m_mailboxView);
        m_mainSplitter->addWidget(messagePane);
        m_mainSplitter->addWidget(m_messageViewContainer);
        m_mainSplitter->setStretchFactor(0, 1);
        m_mainSplitter->setStretchFactor(1, 2);
        m_mainSplitter->setStretchFactor(2, 3);
        m_mainSplitter->setSizes({240, 420, 780});

        auto* central = new QWidget(this);
        auto* centralLayout = new QVBoxLayout(central);
        centralLayout->setContentsMargins(0, 0, 0, 0);
        centralLayout->setSpacing(8);
        centralLayout->addWidget(m_accountCombo);
        centralLayout->addWidget(m_mainSplitter);

        setCentralWidget(central);
        reloadAccounts();
        statusBar()->showMessage(m_jmapCore.statusSummary());
        updateEmptyStates();
    }

    void MainWindow::connectSelection()
    {
        connect(m_accountCombo, &QComboBox::currentIndexChanged, this,
                [this](const int index)
                {
                    const auto accountId = m_accountCombo->itemData(index).toString();
                    const auto account = accountId.isEmpty()
                                             ? std::optional<std::string>{std::nullopt}
                                             : std::optional<std::string>{accountId.toStdString()};
                    m_mailboxView->clearSelection();
                    m_messageView->clearSelection();
                    m_mailboxModel->setAccountId(account);
                    m_messageModel->setMailboxContext(account, std::nullopt);
                    m_messageViewContainer->setSelection(m_messageViewService, account,
                                                         std::nullopt, std::nullopt);
                    updateEmptyStates();
                });

        connect(m_mailboxView->selectionModel(), &QItemSelectionModel::currentChanged, this,
                [this](const QModelIndex& current, const QModelIndex&)
                {
                    const auto accountId = currentAccountId(*m_accountCombo);
                    if (!current.isValid())
                    {
                        m_messageView->clearSelection();
                        m_messageModel->setMailboxContext(accountId, std::nullopt);
                        m_messageViewContainer->setSelection(m_messageViewService, accountId,
                                                             std::nullopt, std::nullopt);
                        updateEmptyStates();
                        return;
                    }

                    const auto mailboxId =
                        current.data(javelin::gui::mailboxes::MailboxTreeModel::MailboxIdRole)
                            .toString();
                    if (mailboxId.isEmpty() || !accountId.has_value())
                    {
                        return;
                    }

                    m_messageView->clearSelection();
                    m_messageModel->setMailboxContext(*accountId, mailboxId.toStdString());
                    m_messageViewContainer->setSelection(m_messageViewService, accountId,
                                                         mailboxId.toStdString(), std::nullopt);
                    updateEmptyStates();
                });

        connect(
            m_messageView->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex& current, const QModelIndex&)
            {
                const auto accountId = currentAccountId(*m_accountCombo);
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
                if (!emailId.isEmpty())
                {
                    refreshSelectedMessageContent(*accountId, emailId.toStdString());
                }
            });
    }

    void MainWindow::reloadAccounts()
    {
        const auto selectedAccountId = m_accountCombo->currentData().toString();
        m_accountCombo->clear();

        const auto result = m_accountRepository.listAll();
        if (const auto* accounts =
                std::get_if<std::vector<javelin::jmap::cache::CachedAccount>>(&result))
        {
            for (const auto& account : *accounts)
            {
                const auto label = account.name.empty() ? account.accountId : account.name;
                m_accountCombo->addItem(QString::fromStdString(label),
                                        QString::fromStdString(account.accountId));
            }
        }

        if (m_accountCombo->count() > 0)
        {
            int accountIndex = 0;
            if (!selectedAccountId.isEmpty())
            {
                const int persistedIndex = m_accountCombo->findData(selectedAccountId);
                if (persistedIndex >= 0)
                {
                    accountIndex = persistedIndex;
                }
            }

            m_accountCombo->setCurrentIndex(accountIndex);
            const auto accountId = m_accountCombo->currentData().toString().toStdString();
            m_mailboxModel->setAccountId(accountId);
            m_messageModel->setMailboxContext(accountId, std::nullopt);
            m_messageViewContainer->setSelection(m_messageViewService, accountId, std::nullopt,
                                                 std::nullopt);
            return;
        }

        m_messageViewContainer->setSelection(m_messageViewService, std::nullopt, std::nullopt,
                                             std::nullopt);
    }

    void MainWindow::refreshViewsFromCache()
    {
        m_mailboxModel->refresh();
        m_messageModel->refresh();
        m_messageViewContainer->refresh(m_messageViewService);
        updateEmptyStates();
    }

    void MainWindow::updateEmptyStates()
    {
        const bool hasMessages = m_messageModel->rowCount() > 0;
        m_messageEmptyState->setVisible(!hasMessages);
        m_messageView->setVisible(hasMessages);
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

        auto task = m_jmapCore.refreshFromServer(toLiveConnectionSettings(settings));
        QCoro::connect(
            std::move(task), this,
            [this](javelin::jmap::LiveRefreshResult result)
            {
                m_refreshInFlight = false;
                m_refreshAction->setEnabled(true);
                m_preferencesAction->setEnabled(true);

                if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&result))
                {
                    statusBar()->showMessage(error->message, 10000);
                    return;
                }

                const auto& summary = std::get<javelin::jmap::LiveRefreshSummary>(result);
                reloadAccounts();
                refreshViewsFromCache();
                statusBar()->showMessage(
                    QStringLiteral("Synced %1 mailboxes and %2 messages for %3.")
                        .arg(summary.mailboxCount)
                        .arg(summary.emailCount)
                        .arg(QString::fromStdString(summary.accountId)),
                    10000);
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

        auto task = m_jmapCore.refreshMessageContent(toLiveConnectionSettings(settings), accountId,
                                                     emailId);
        QCoro::connect(
            std::move(task), this,
            [this, accountId = std::move(accountId),
             emailId = std::move(emailId)](javelin::jmap::MessageContentRefreshResult result)
            {
                if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&result))
                {
                    statusBar()->showMessage(error->message, 10000);
                    return;
                }

                const auto currentAccount = currentAccountId(*m_accountCombo);
                const auto selectedEmail = currentEmailId(*m_messageView);
                if (currentAccount != std::optional<std::string>{accountId} ||
                    selectedEmail != std::optional<std::string>{emailId})
                {
                    return;
                }

                m_messageViewContainer->refresh(m_messageViewService);

                const auto& summary = std::get<javelin::jmap::MessageContentRefreshSummary>(result);
                if (!summary.usedCachedContent)
                {
                    statusBar()->showMessage(QStringLiteral("Loaded message content for %1.")
                                                 .arg(QString::fromStdString(summary.emailId)),
                                             5000);
                }
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

        const auto selectedAccountId = settings.value(QLatin1StringView{accountIdKey}).toString();
        settings.endGroup();

        if (selectedAccountId.isEmpty())
        {
            return;
        }

        const int accountIndex = m_accountCombo->findData(selectedAccountId);
        if (accountIndex >= 0)
        {
            m_accountCombo->setCurrentIndex(accountIndex);
        }
    }

    void MainWindow::savePersistentState() const
    {
        QSettings settings;
        settings.beginGroup(QLatin1StringView{windowGroup});
        settings.setValue(QLatin1StringView{geometryKey}, saveGeometry());
        settings.setValue(QLatin1StringView{splitterKey}, m_mainSplitter->saveState());
        settings.setValue(QLatin1StringView{accountIdKey}, m_accountCombo->currentData());
        settings.endGroup();
        settings.sync();
    }

    void MainWindow::closeEvent(QCloseEvent* event)
    {
        savePersistentState();
        QMainWindow::closeEvent(event);
    }

} // namespace javelin::gui::shell
