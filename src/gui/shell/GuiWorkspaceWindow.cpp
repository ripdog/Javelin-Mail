#include "gui/shell/GuiWorkspaceWindow.h"

#include "app/GuiDaemonSession.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFont>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTextBrowser>
#include <QToolBar>
#include <QTreeView>
#include <QUuid>
#include <QtConcurrentRun>

#include <algorithm>
#include <type_traits>
#include <utility>

namespace javelin::gui::shell
{
    namespace
    {
        constexpr int accountIdRole = Qt::UserRole + 1;
        constexpr int mailboxIdRole = Qt::UserRole + 2;
        constexpr int emailIdRole = Qt::UserRole + 3;

        [[nodiscard]] QStandardItem* firstMailbox(QStandardItemModel& model)
        {
            for (int accountRow = 0; accountRow < model.rowCount(); ++accountRow)
            {
                auto* account = model.item(accountRow);
                if (account != nullptr && account->rowCount() > 0)
                    return account->child(0);
            }
            return nullptr;
        }

    } // namespace

    GuiWorkspaceWindow::GuiWorkspaceWindow(javelin::app::GuiDaemonSession& session, QWidget* parent)
        : QMainWindow(parent), m_session(session), m_cacheReader(session.databasePath())
    {
        setWindowTitle(QStringLiteral("Javelin Mail"));
        resize(1280, 760);

        auto* toolbar = addToolBar(QStringLiteral("Mail"));
        toolbar->setMovable(false);
        const auto refreshAction = toolbar->addAction(QStringLiteral("Refresh"));
        const auto settingsAction = toolbar->addAction(QStringLiteral("Account settings"));
        connect(refreshAction, &QAction::triggered, this,
                &GuiWorkspaceWindow::requestRemoteRefresh);
        connect(settingsAction, &QAction::triggered, this,
                &GuiWorkspaceWindow::showAccountSettings);

        auto* splitter = new QSplitter(Qt::Horizontal, this);
        m_mailboxView = new QTreeView(splitter);
        m_messageView = new QListView(splitter);
        m_messageBody = new QTextBrowser(splitter);
        m_mailboxModel = new QStandardItemModel(m_mailboxView);
        m_messageModel = new QStandardItemModel(m_messageView);
        m_mailboxView->setModel(m_mailboxModel);
        m_messageView->setModel(m_messageModel);
        m_mailboxView->header()->hide();
        m_messageView->setAlternatingRowColors(true);
        m_messageBody->setOpenExternalLinks(false);
        splitter->setStretchFactor(0, 1);
        splitter->setStretchFactor(1, 2);
        splitter->setStretchFactor(2, 3);
        setCentralWidget(splitter);

        m_statusLabel = new QLabel(this);
        statusBar()->addPermanentWidget(m_statusLabel, 1);
        connect(m_mailboxView->selectionModel(), &QItemSelectionModel::currentChanged, this,
                [this](const QModelIndex& current, const QModelIndex&)
                {
                    const auto accountId = current.data(accountIdRole).toString();
                    const auto mailboxId = current.data(mailboxIdRole).toString();
                    if (!accountId.isEmpty() && !mailboxId.isEmpty())
                        loadMessages(accountId, mailboxId);
                });
        connect(m_messageView->selectionModel(), &QItemSelectionModel::currentChanged, this,
                [this](const QModelIndex& current, const QModelIndex&)
                { showSelectedMessage(current); });
        connect(&m_snapshotWatcher,
                &QFutureWatcher<javelin::app::GuiCacheReader::SnapshotResult>::finished, this,
                [this] { populateCache(m_snapshotWatcher.result()); });
        connect(&m_messageWatcher,
                &QFutureWatcher<javelin::app::GuiCacheReader::MessageListResult>::finished, this,
                [this] { populateMessages(m_messageWatcher.result()); });
        connect(&m_detailWatcher,
                &QFutureWatcher<javelin::app::GuiCacheReader::MessageDetailResult>::finished, this,
                [this] { populateMessage(m_detailWatcher.result()); });
        connect(&m_session, &javelin::app::GuiDaemonSession::cacheChanged, this,
                [this]
                {
                    if (!m_snapshotWatcher.isRunning())
                        refresh();
                    else
                        m_refreshPending = true;
                });
        connect(&m_session, &javelin::app::GuiDaemonSession::settingsChanged, this,
                [this] { requestRemoteRefresh(); });

        refresh();
    }

    GuiWorkspaceWindow::~GuiWorkspaceWindow() = default;

    void GuiWorkspaceWindow::refresh()
    {
        if (m_snapshotWatcher.isRunning())
        {
            m_refreshPending = true;
            return;
        }
        m_statusLabel->setText(QStringLiteral("Loading local mail cache…"));
        const auto reader = m_cacheReader;
        m_snapshotWatcher.setFuture(QtConcurrent::run([reader] { return reader.loadSnapshot(); }));
    }

    void GuiWorkspaceWindow::populateCache(javelin::app::GuiCacheReader::SnapshotResult result)
    {
        m_mailboxModel->clear();
        if (const auto* error = std::get_if<QString>(&result))
        {
            showError(*error);
            return;
        }

        const auto& snapshot = std::get<javelin::app::GuiCacheReader::CacheSnapshot>(result);
        for (const auto& account : snapshot.accounts)
        {
            auto* accountItem = new QStandardItem(
                account.address.isEmpty()
                    ? account.accountId
                    : QStringLiteral("%1 (%2)").arg(account.address, account.accountId));
            accountItem->setData(account.accountId, accountIdRole);
            for (const auto& mailbox : snapshot.mailboxes)
            {
                if (mailbox.accountId != account.accountId)
                    continue;
                auto* mailboxItem = new QStandardItem(QStringLiteral("%1  (%2/%3)")
                                                          .arg(mailbox.name)
                                                          .arg(mailbox.unreadEmails)
                                                          .arg(mailbox.totalEmails));
                mailboxItem->setData(mailbox.accountId, accountIdRole);
                mailboxItem->setData(mailbox.mailboxId, mailboxIdRole);
                accountItem->appendRow(mailboxItem);
            }
            m_mailboxModel->appendRow(accountItem);
        }
        m_mailboxView->expandAll();
        if (auto* mailbox = firstMailbox(*m_mailboxModel); mailbox != nullptr)
            m_mailboxView->setCurrentIndex(mailbox->index());
        else
            m_statusLabel->setText(
                QStringLiteral("No cached mail yet — configure an account or refresh."));

        if (m_refreshPending)
        {
            m_refreshPending = false;
            refresh();
        }
    }

    void GuiWorkspaceWindow::loadMessages(const QString& accountId, const QString& mailboxId)
    {
        m_selectedAccountId = accountId;
        m_selectedMailboxId = mailboxId;
        m_selectedEmailId.clear();
        m_messageModel->clear();
        m_messageBody->clear();
        if (const auto error = m_session.requestMailboxWindow(accountId, mailboxId))
        {
            showError(error->detail);
            return;
        }
        m_statusLabel->setText(QStringLiteral("Loading messages…"));
        const auto reader = m_cacheReader;
        m_messageWatcher.setFuture(QtConcurrent::run(
            [reader, accountId, mailboxId] { return reader.loadMessages(accountId, mailboxId); }));
    }

    void
    GuiWorkspaceWindow::populateMessages(javelin::app::GuiCacheReader::MessageListResult result)
    {
        m_messageModel->clear();
        if (const auto* error = std::get_if<QString>(&result))
        {
            showError(*error);
            return;
        }

        for (const auto& message :
             std::get<std::vector<javelin::app::GuiCacheReader::MessageSummary>>(result))
        {
            const auto subject =
                message.subject.isEmpty() ? QStringLiteral("(no subject)") : message.subject;
            auto* item = new QStandardItem(
                QStringLiteral("%1 — %2\n%3")
                    .arg(message.unread ? QStringLiteral("● ") : QString{}, subject,
                         message.sender.isEmpty() ? message.preview : message.sender));
            if (message.unread)
            {
                auto font = item->font();
                font.setBold(true);
                item->setFont(font);
            }
            item->setData(message.emailId, emailIdRole);
            item->setData(message.accountId, accountIdRole);
            m_messageModel->appendRow(item);
        }
        m_statusLabel->setText(
            QStringLiteral("%1 cached message%2")
                .arg(m_messageModel->rowCount())
                .arg(m_messageModel->rowCount() == 1 ? QString{} : QStringLiteral("s")));
    }

    void GuiWorkspaceWindow::showSelectedMessage(const QModelIndex& index)
    {
        const auto emailId = index.data(emailIdRole).toString();
        const auto accountId = index.data(accountIdRole).toString();
        if (emailId.isEmpty() || accountId.isEmpty())
            return;
        loadMessage(accountId, emailId);
    }

    void GuiWorkspaceWindow::loadMessage(const QString& accountId, const QString& emailId)
    {
        m_selectedEmailId = emailId;
        const auto reader = m_cacheReader;
        m_detailWatcher.setFuture(QtConcurrent::run(
            [reader, accountId, emailId] { return reader.loadMessage(accountId, emailId); }));
    }

    void
    GuiWorkspaceWindow::populateMessage(javelin::app::GuiCacheReader::MessageDetailResult result)
    {
        if (const auto* error = std::get_if<QString>(&result))
        {
            showError(*error);
            return;
        }
        const auto& message = std::get<javelin::app::GuiCacheReader::MessageDetail>(result);
        const auto header =
            QStringLiteral("%1\nFrom: %2\nReceived: %3\n\n")
                .arg(message.subject.isEmpty() ? QStringLiteral("(no subject)") : message.subject,
                     message.sender, message.receivedAt);
        if (message.bodyIsHtml)
            m_messageBody->setHtml(header.toHtmlEscaped() + message.body);
        else
            m_messageBody->setPlainText(header + message.body);
    }

    void GuiWorkspaceWindow::requestRemoteRefresh()
    {
        const auto& settings = m_session.settings();
        if (settings.accounts.empty())
        {
            showAccountSettings();
            return;
        }

        bool accepted = false;
        for (const auto& account : settings.accounts)
        {
            const auto accountId =
                account.cachedAccountIds.empty() ? account.id : account.cachedAccountIds.front();
            if (accountId.isEmpty())
                continue;
            if (!m_session.requestAccountRefresh(accountId))
                accepted = true;
        }
        m_statusLabel->setText(accepted ? QStringLiteral("Refresh requested…")
                                        : QStringLiteral("No usable account configuration"));
    }

    void GuiWorkspaceWindow::showAccountSettings()
    {
        QDialog dialog{this};
        dialog.setWindowTitle(QStringLiteral("Account settings"));
        auto* form = new QFormLayout(&dialog);
        auto* displayName = new QLineEdit(&dialog);
        auto* sessionUrl = new QLineEdit(&dialog);
        auto* loginEmail = new QLineEdit(&dialog);
        auto* apiKey = new QLineEdit(&dialog);
        apiKey->setEchoMode(QLineEdit::Password);
        const auto& settings = m_session.settings();
        protocol::AccountSettings account;
        if (!settings.accounts.empty())
            account = settings.accounts.front();
        account.id =
            account.id.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : account.id;
        displayName->setText(account.displayName);
        sessionUrl->setText(account.sessionUrl);
        loginEmail->setText(account.loginEmail);
        apiKey->setText(account.apiKey);
        form->addRow(QStringLiteral("Name"), displayName);
        form->addRow(QStringLiteral("JMAP session URL"), sessionUrl);
        form->addRow(QStringLiteral("Email"), loginEmail);
        form->addRow(QStringLiteral("API key"), apiKey);
        auto* buttons =
            new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
        form->addRow(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted)
            return;

        account.displayName = displayName->text().trimmed();
        account.sessionUrl = sessionUrl->text().trimmed();
        account.loginEmail = loginEmail->text().trimmed();
        account.apiKey = apiKey->text().trimmed();
        account.revision = std::max<std::uint64_t>(account.revision + 1, 1);
        auto accounts = settings.accounts;
        if (accounts.empty())
            accounts.push_back(std::move(account));
        else
            accounts.front() = std::move(account);
        protocol::SettingsUpdate update;
        update.accounts = std::move(accounts);
        if (const auto error = m_session.updateSettings(std::move(update)))
        {
            QMessageBox::critical(this, QStringLiteral("Could not save account"), error->detail);
            return;
        }
        m_statusLabel->setText(QStringLiteral("Account saved; refresh requested…"));
    }

    void GuiWorkspaceWindow::showError(const QString& message)
    {
        m_statusLabel->setText(QStringLiteral("Cache error: %1").arg(message));
    }
} // namespace javelin::gui::shell
