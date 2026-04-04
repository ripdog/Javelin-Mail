#include "gui/shell/MainWindow.h"

#include "gui/mailboxes/MailboxTreeModel.h"
#include "gui/messages/MessageListModel.h"
#include "gui/messageview/MessageViewContainer.h"
#include "jmap/JmapCore.h"
#include "jmap/cache/AccountRepository.h"
#include "jmap/cache/QueryService.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListView>
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

    } // namespace

    MainWindow::MainWindow(javelin::jmap::JmapCore& jmapCore,
                           javelin::jmap::cache::AccountRepository& accountRepository,
                           javelin::jmap::cache::QueryService& queryService, QWidget* parent)
        : QMainWindow(parent), m_jmapCore(jmapCore), m_accountRepository(accountRepository),
          m_queryService(queryService)
    {
        setupUi();
        connectSelection();
        restorePersistentState();
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
                    m_messageViewContainer->setSelection(account, std::nullopt, std::nullopt);
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
                        m_messageViewContainer->setSelection(accountId, std::nullopt, std::nullopt);
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
                    m_messageViewContainer->setSelection(accountId, mailboxId.toStdString(),
                                                         std::nullopt);
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
                    m_messageViewContainer->setSelection(accountId, mailboxId, std::nullopt);
                    return;
                }

                if (!current.isValid())
                {
                    m_messageViewContainer->setSelection(accountId, mailboxId, std::nullopt);
                    return;
                }

                const auto emailId =
                    current.data(javelin::gui::messages::MessageListModel::EmailIdRole).toString();
                m_messageViewContainer->setSelection(
                    accountId, mailboxId,
                    emailId.isEmpty() ? std::optional<std::string>{std::nullopt}
                                      : std::optional<std::string>{emailId.toStdString()});
                updateEmptyStates();
            });
    }

    void MainWindow::reloadAccounts()
    {
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
            const auto accountId = m_accountCombo->currentData().toString().toStdString();
            m_mailboxModel->setAccountId(accountId);
            m_messageModel->setMailboxContext(accountId, std::nullopt);
            m_messageViewContainer->setSelection(accountId, std::nullopt, std::nullopt);
            return;
        }

        m_messageViewContainer->setSelection(std::nullopt, std::nullopt, std::nullopt);
    }

    void MainWindow::updateEmptyStates()
    {
        const bool hasMessages = m_messageModel->rowCount() > 0;
        m_messageEmptyState->setVisible(!hasMessages);
        m_messageView->setVisible(hasMessages);
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
