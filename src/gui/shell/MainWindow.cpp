#include "gui/shell/MainWindow.h"

#include "gui/mailboxes/MailboxTreeModel.h"
#include "gui/messages/MessageListModel.h"
#include "jmap/JmapCore.h"
#include "jmap/cache/AccountRepository.h"
#include "jmap/cache/QueryService.h"

#include <QComboBox>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListView>
#include <QSplitter>
#include <QStatusBar>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWidget>

namespace javelin::gui::shell
{
    namespace
    {

        [[nodiscard]] std::optional<std::string> currentAccountId(const QComboBox& accountCombo)
        {
            const auto accountId = accountCombo.currentData().toString();
            return accountId.isEmpty() ? std::optional<std::string>{std::nullopt}
                                       : std::optional<std::string>{accountId.toStdString()};
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

        auto* messageViewPane = new QLabel(
            QStringLiteral("Message view services will attach selected-message loading here."),
            this);
        messageViewPane->setWordWrap(true);

        auto* splitter = new QSplitter(Qt::Horizontal, this);
        splitter->addWidget(m_mailboxView);
        splitter->addWidget(messagePane);
        splitter->addWidget(messageViewPane);
        splitter->setStretchFactor(0, 1);
        splitter->setStretchFactor(1, 2);
        splitter->setStretchFactor(2, 3);

        auto* central = new QWidget(this);
        auto* centralLayout = new QVBoxLayout(central);
        centralLayout->setContentsMargins(0, 0, 0, 0);
        centralLayout->setSpacing(8);
        centralLayout->addWidget(m_accountCombo);
        centralLayout->addWidget(splitter);

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
                    m_mailboxModel->setAccountId(account);
                    m_messageModel->setMailboxContext(account, std::nullopt);
                    updateEmptyStates();
                });

        connect(m_mailboxView->selectionModel(), &QItemSelectionModel::currentChanged, this,
                [this](const QModelIndex& current, const QModelIndex&)
                {
                    const auto accountId = currentAccountId(*m_accountCombo);
                    if (!current.isValid())
                    {
                        m_messageModel->setMailboxContext(accountId, std::nullopt);
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

                    m_messageModel->setMailboxContext(*accountId, mailboxId.toStdString());
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
        }
    }

    void MainWindow::updateEmptyStates()
    {
        const bool hasMessages = m_messageModel->rowCount() > 0;
        m_messageEmptyState->setVisible(!hasMessages);
        m_messageView->setVisible(hasMessages);
    }

} // namespace javelin::gui::shell
