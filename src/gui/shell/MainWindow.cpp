#include "gui/shell/MainWindow.h"

#include "gui/mailboxes/MailboxTreeModel.h"
#include "gui/messages/MessageListModel.h"
#include "jmap/JmapCore.h"
#include "jmap/cache/QueryService.h"

#include <QLabel>
#include <QListView>
#include <QSplitter>
#include <QStatusBar>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWidget>

namespace javelin::gui::shell
{

    MainWindow::MainWindow(javelin::jmap::JmapCore& jmapCore,
                           javelin::jmap::cache::QueryService& queryService, QWidget* parent)
        : QMainWindow(parent), m_jmapCore(jmapCore), m_queryService(queryService)
    {
        setupUi();
        connectSelection();
    }

    void MainWindow::setupUi()
    {
        setWindowTitle(QStringLiteral("Javelin Mail"));
        resize(1440, 900);

        m_mailboxModel = new javelin::gui::mailboxes::MailboxTreeModel(m_queryService, this);
        m_mailboxModel->setAccountId(std::string{"account-1"});

        m_messageModel = new javelin::gui::messages::MessageListModel(m_queryService, this);
        m_messageModel->setMailboxContext(std::string{"account-1"}, std::nullopt);

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

        setCentralWidget(splitter);
        statusBar()->showMessage(m_jmapCore.statusSummary());
        updateEmptyStates();
    }

    void MainWindow::connectSelection()
    {
        connect(m_mailboxView->selectionModel(), &QItemSelectionModel::currentChanged, this,
                [this](const QModelIndex& current, const QModelIndex&)
                {
                    if (!current.isValid())
                    {
                        m_messageModel->setMailboxContext(std::string{"account-1"}, std::nullopt);
                        updateEmptyStates();
                        return;
                    }

                    const auto mailboxId =
                        current.data(javelin::gui::mailboxes::MailboxTreeModel::MailboxIdRole)
                            .toString();
                    if (mailboxId.isEmpty())
                    {
                        return;
                    }
                    m_messageModel->setMailboxContext(std::string{"account-1"},
                                                      mailboxId.toStdString());
                    updateEmptyStates();
                });
    }

    void MainWindow::updateEmptyStates()
    {
        const bool hasMessages = m_messageModel->rowCount() > 0;
        m_messageEmptyState->setVisible(!hasMessages);
        m_messageView->setVisible(hasMessages);
    }

} // namespace javelin::gui::shell
