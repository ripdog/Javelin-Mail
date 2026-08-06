#include "gui/developer/DeveloperOptionsDialog.h"

#include <QCoroTask>

#include <KConfigGroup>
#include <KLocalizedString>
#include <KPageWidgetItem>
#include <KSharedConfig>

#include <QAbstractItemView>
#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHash>
#include <QHeaderView>
#include <QIcon>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStandardItemModel>
#include <QTreeView>
#include <QVBoxLayout>

#include <utility>

namespace javelin::gui::developer
{
    namespace
    {
        enum Column
        {
            MailboxColumn,
            SqliteColumn,
            BodiesColumn,
            ReclaimableColumn,
            OfflineColumn,
            MeasuredColumn,
            ColumnCount,
        };

        constexpr int mailboxIndexRole = Qt::UserRole + 1;

        [[nodiscard]] QString bytes(const std::uint64_t value)
        {
            return QLocale{}.formattedDataSize(static_cast<qint64>(value), 1,
                                               QLocale::DataSizeTraditionalFormat);
        }

        [[nodiscard]] QString display(const std::optional<QString>& value)
        {
            return value.has_value() && !value->isEmpty() ? *value : QStringLiteral("—");
        }

        [[nodiscard]] QString display(const std::optional<std::uint64_t>& value)
        {
            return value.has_value() ? QLocale{}.toString(*value) : QStringLiteral("—");
        }

        [[nodiscard]] QString yesNo(const bool value)
        {
            return value ? i18nc("developer diagnostic boolean", "Yes")
                         : i18nc("developer diagnostic boolean", "No");
        }

        [[nodiscard]] QString detailsFor(const javelin::app::DeveloperMailboxRecord& mailbox)
        {
            QStringList lines;
            const auto add = [&lines](const QString& label, const QString& value)
            { lines.push_back(QStringLiteral("%1: %2").arg(label, value)); };

            lines << i18n("Identity") << QStringLiteral("────────");
            add(i18n("Account"), mailbox.accountName);
            add(i18n("Account email"), mailbox.accountEmailAddress);
            add(i18n("JMAP account ID"), mailbox.accountId);
            add(i18n("Mailbox"), mailbox.mailboxName);
            add(i18n("JMAP mailbox ID"), mailbox.mailboxId);
            add(i18n("Parent mailbox"), display(mailbox.parentMailboxName));
            add(i18n("Parent mailbox ID"), display(mailbox.parentMailboxId));
            add(i18n("Role"), display(mailbox.role));
            add(i18n("Sort order"), QLocale{}.toString(mailbox.sortOrder));
            add(i18n("Subscribed"), yesNo(mailbox.isSubscribed));
            add(i18n("Total emails"), QLocale{}.toString(mailbox.totalEmails));
            add(i18n("Unread emails"), QLocale{}.toString(mailbox.unreadEmails));
            add(i18n("Total threads"), QLocale{}.toString(mailbox.totalThreads));
            add(i18n("Unread threads"), QLocale{}.toString(mailbox.unreadThreads));
            add(i18n("Mailbox state"), display(mailbox.mailboxState));

            lines << QString{} << i18n("Rights") << QStringLiteral("──────");
            add(QStringLiteral("mayReadItems"), yesNo(mailbox.mayReadItems));
            add(QStringLiteral("mayAddItems"), yesNo(mailbox.mayAddItems));
            add(QStringLiteral("mayRemoveItems"), yesNo(mailbox.mayRemoveItems));
            add(QStringLiteral("maySetSeen"), yesNo(mailbox.maySetSeen));
            add(QStringLiteral("maySetKeywords"), yesNo(mailbox.maySetKeywords));
            add(QStringLiteral("mayCreateChild"), yesNo(mailbox.mayCreateChild));
            add(QStringLiteral("mayRename"), yesNo(mailbox.mayRename));
            add(QStringLiteral("mayDelete"), yesNo(mailbox.mayDelete));
            add(QStringLiteral("maySubmit"), yesNo(mailbox.maySubmit));
            add(i18n("Raw rights JSON"), mailbox.rawRightsJson);

            lines << QString{} << i18n("Stored state") << QStringLiteral("────────────");
            add(i18n("Account Mailbox state"), display(mailbox.accountMailboxState));
            add(i18n("Account Email state"), display(mailbox.accountEmailState));
            add(i18n("Cached memberships"), QLocale{}.toString(mailbox.cachedMembershipCount));
            add(i18n("Query windows"), QLocale{}.toString(mailbox.queryWindowCount));
            add(i18n("Query-window items"), QLocale{}.toString(mailbox.queryWindowItemCount));
            add(i18n("Query coverage"), mailbox.queryCoverageSummary.isEmpty()
                                            ? QStringLiteral("—")
                                            : mailbox.queryCoverageSummary);
            add(i18n("Query materialization"), mailbox.queryMaterializationSummary.isEmpty()
                                                   ? QStringLiteral("—")
                                                   : mailbox.queryMaterializationSummary);
            add(i18n("Oldest cached message"), display(mailbox.oldestCachedMessage));
            add(i18n("Newest cached message"), display(mailbox.newestCachedMessage));
            add(i18n("Active optimistic mutations"),
                QLocale{}.toString(mailbox.activeMutationCount));

            lines << QString{} << i18n("Offline mirror") << QStringLiteral("──────────────");
            add(i18n("Selected for offline storage"), yesNo(mailbox.offlineDesired));
            add(i18n("Status"), display(mailbox.offlineStatus));
            add(i18n("Query state"), display(mailbox.offlineQueryState));
            add(i18n("Email state"), display(mailbox.offlineEmailState));
            add(i18n("Expected items"), display(mailbox.offlineExpectedTotal));
            add(i18n("Completed items"), QLocale{}.toString(mailbox.offlineCompletedTotal));
            add(i18n("Completed bytes"), bytes(mailbox.offlineCompletedBytes));
            add(i18n("Estimated bytes"), mailbox.offlineEstimatedBytes.has_value()
                                             ? bytes(*mailbox.offlineEstimatedBytes)
                                             : QStringLiteral("—"));
            add(i18n("Generation"), QLocale{}.toString(mailbox.offlineGeneration));
            add(i18n("Completed generation"), display(mailbox.offlineCompletedGeneration));
            add(i18n("Anchor email ID"), display(mailbox.offlineAnchorEmailId));
            add(i18n("Last error"), display(mailbox.offlineLatestError));
            add(i18n("Updated"), display(mailbox.offlineUpdatedAt));

            lines << QString{} << i18n("Mail vault") << QStringLiteral("──────────");
            add(i18n("References"), QLocale{}.toString(mailbox.vaultReferenceCount));
            add(i18n("Pending projection jobs"),
                QLocale{}.toString(mailbox.pendingVaultProjectionCount));
            add(i18n("Failed projection jobs"),
                QLocale{}.toString(mailbox.failedVaultProjectionCount));
            add(i18n("Completed projection jobs"),
                QLocale{}.toString(mailbox.completeVaultProjectionCount));
            add(i18n("Missing body objects"), QLocale{}.toString(mailbox.usage.missingBodyObjects));
            add(i18n("Active body leases"), QLocale{}.toString(mailbox.usage.activeBodyLeases));

            lines << QString{} << i18n("Cache usage") << QStringLiteral("───────────");
            add(i18n("Estimated SQLite data"), bytes(mailbox.usage.sqliteEstimatedBytes));
            add(i18n("Logical message bodies"), bytes(mailbox.usage.logicalBodyBytes));
            add(i18n("Shared message bodies"), bytes(mailbox.usage.sharedBodyBytes));
            add(i18n("Reclaimable message bodies"), bytes(mailbox.usage.reclaimableBodyBytes));
            add(i18n("Allocated body storage"), bytes(mailbox.usage.allocatedBodyBytes));
            add(i18n("Measured"), mailbox.measuredAt);
            return lines.join(QLatin1Char('\n'));
        }
    } // namespace

    DeveloperOptionsDialog::DeveloperOptionsDialog(
        javelin::app::DeveloperDiagnosticsPort& diagnostics, QWidget* parent)
        : KPageDialog(parent), m_diagnostics(diagnostics)
    {
        setWindowTitle(i18n("Developer Options"));
        setFaceType(KPageDialog::List);
        setStandardButtons(QDialogButtonBox::Close);
        setModal(false);
        resize(1100, 720);

        auto* page = new QWidget(this);
        auto* pageLayout = new QVBoxLayout(page);
        auto* controls = new QHBoxLayout;
        m_filterEdit = new QLineEdit(page);
        m_filterEdit->setPlaceholderText(i18n("Filter mailboxes or raw IDs"));
        m_refreshButton = new QPushButton(QIcon::fromTheme(QStringLiteral("view-refresh")),
                                          i18n("Recalculate"), page);
        controls->addWidget(m_filterEdit, 1);
        controls->addWidget(m_refreshButton);
        pageLayout->addLayout(controls);

        auto* splitter = new QSplitter(Qt::Horizontal, page);
        m_mailboxModel = new QStandardItemModel(this);
        m_mailboxModel->setColumnCount(ColumnCount);
        m_mailboxModel->setHorizontalHeaderLabels({i18n("Mailbox"), i18n("SQLite"), i18n("Bodies"),
                                                   i18n("Reclaimable"), i18n("Offline"),
                                                   i18n("Measured")});
        m_filterModel = new QSortFilterProxyModel(this);
        m_filterModel->setSourceModel(m_mailboxModel);
        m_filterModel->setFilterKeyColumn(-1);
        m_filterModel->setFilterCaseSensitivity(Qt::CaseInsensitive);
        m_filterModel->setRecursiveFilteringEnabled(true);

        m_mailboxView = new QTreeView(splitter);
        m_mailboxView->setModel(m_filterModel);
        m_mailboxView->setAlternatingRowColors(true);
        m_mailboxView->setUniformRowHeights(true);
        m_mailboxView->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_mailboxView->setSelectionMode(QAbstractItemView::SingleSelection);
        m_mailboxView->header()->setStretchLastSection(false);
        m_mailboxView->header()->setSectionResizeMode(MailboxColumn, QHeaderView::Stretch);
        for (int column = SqliteColumn; column < ColumnCount; ++column)
            m_mailboxView->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);

        m_details = new QPlainTextEdit(splitter);
        m_details->setReadOnly(true);
        m_details->setLineWrapMode(QPlainTextEdit::NoWrap);
        m_details->setPlaceholderText(i18n("Select a mailbox to inspect its cached state."));
        splitter->addWidget(m_mailboxView);
        splitter->addWidget(m_details);
        splitter->setStretchFactor(0, 3);
        splitter->setStretchFactor(1, 4);
        pageLayout->addWidget(splitter, 1);

        auto* statusLayout = new QHBoxLayout;
        m_progress = new QProgressBar(page);
        m_progress->setRange(0, 0);
        m_progress->setVisible(false);
        m_status = new QLabel(page);
        statusLayout->addWidget(m_progress);
        statusLayout->addWidget(m_status, 1);
        pageLayout->addLayout(statusLayout);

        auto* mailboxPage = addPage(page, i18n("Mailbox Caches"));
        mailboxPage->setIcon(QIcon::fromTheme(QStringLiteral("mail-folder-inbox")));

        connect(m_filterEdit, &QLineEdit::textChanged, m_filterModel,
                &QSortFilterProxyModel::setFilterFixedString);
        connect(m_refreshButton, &QPushButton::clicked, this, &DeveloperOptionsDialog::refresh);
        connect(m_mailboxView->selectionModel(), &QItemSelectionModel::currentChanged, this,
                [this](const QModelIndex& current) { updateDetails(current); });
        connect(buttonBox(), &QDialogButtonBox::rejected, this, &QDialog::close);

        restoreUiState();
        refresh();
    }

    DeveloperOptionsDialog::~DeveloperOptionsDialog() = default;

    void DeveloperOptionsDialog::refresh()
    {
        if (m_loading)
            return;
        setLoading(true);
        auto task = m_diagnostics.snapshot();
        QCoro::connect(std::move(task), this,
                       [this](javelin::app::DeveloperDiagnosticsResult result)
                       { applySnapshot(std::move(result)); });
    }

    void DeveloperOptionsDialog::applySnapshot(javelin::app::DeveloperDiagnosticsResult result)
    {
        setLoading(false);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
        {
            m_status->setText(i18n("Mailbox cache inspection failed: %1", error->message));
            m_details->setPlainText(error->message);
            return;
        }

        auto snapshot = std::get<javelin::app::DeveloperDiagnosticsSnapshot>(std::move(result));
        m_mailboxes = std::move(snapshot.mailboxes);
        m_mailboxModel->removeRows(0, m_mailboxModel->rowCount());

        QHash<QString, QStandardItem*> accounts;
        for (std::size_t index = 0; index < m_mailboxes.size(); ++index)
        {
            const auto& mailbox = m_mailboxes[index];
            QStandardItem* accountItem = accounts.value(mailbox.accountId, nullptr);
            if (accountItem == nullptr)
            {
                accountItem = new QStandardItem(QStringLiteral("%1 — %2").arg(
                    mailbox.accountName, mailbox.accountEmailAddress));
                accountItem->setEditable(false);
                accountItem->setToolTip(mailbox.accountId);
                QList<QStandardItem*> accountRow{accountItem};
                while (accountRow.size() < ColumnCount)
                    accountRow.push_back(new QStandardItem);
                m_mailboxModel->appendRow(accountRow);
                accounts.insert(mailbox.accountId, accountItem);
            }

            QList<QStandardItem*> row;
            row.reserve(ColumnCount);
            auto* name = new QStandardItem(mailbox.mailboxName);
            name->setData(static_cast<qulonglong>(index), mailboxIndexRole);
            name->setToolTip(QStringLiteral("%1\n%2").arg(mailbox.mailboxId, mailbox.accountId));
            row.push_back(name);
            row.push_back(new QStandardItem(bytes(mailbox.usage.sqliteEstimatedBytes)));
            row.push_back(new QStandardItem(bytes(mailbox.usage.logicalBodyBytes)));
            row.push_back(new QStandardItem(bytes(mailbox.usage.reclaimableBodyBytes)));
            row.push_back(new QStandardItem(mailbox.offlineDesired
                                                ? display(mailbox.offlineStatus)
                                                : i18nc("offline mailbox status", "No")));
            row.push_back(new QStandardItem(mailbox.measuredAt));
            for (auto* item : row)
                item->setEditable(false);
            accountItem->appendRow(row);
        }

        m_mailboxView->expandAll();
        m_status->setText(i18np("Measured %1 cached mailbox.", "Measured %1 cached mailboxes.",
                                m_mailboxes.size()));
        if (m_filterModel->rowCount() > 0)
        {
            const auto firstAccount = m_filterModel->index(0, 0);
            if (m_filterModel->rowCount(firstAccount) > 0)
                m_mailboxView->setCurrentIndex(m_filterModel->index(0, 0, firstAccount));
        }
    }

    void DeveloperOptionsDialog::updateDetails(const QModelIndex& current)
    {
        if (!current.isValid())
            return;
        const QModelIndex source = m_filterModel->mapToSource(current.siblingAtColumn(0));
        const QVariant value = source.data(mailboxIndexRole);
        if (!value.isValid())
        {
            m_details->clear();
            return;
        }
        const auto index = static_cast<std::size_t>(value.toULongLong());
        if (index >= m_mailboxes.size())
            return;
        m_details->setPlainText(detailsFor(m_mailboxes[index]));
    }

    void DeveloperOptionsDialog::setLoading(const bool loading)
    {
        m_loading = loading;
        m_progress->setVisible(loading);
        m_refreshButton->setEnabled(!loading);
        if (loading)
            m_status->setText(i18n("Calculating mailbox cache usage…"));
    }

    void DeveloperOptionsDialog::restoreUiState()
    {
        const KConfigGroup settings{KSharedConfig::openConfig(),
                                    QStringLiteral("DeveloperOptions")};
        const QByteArray geometry = settings.readEntry(QStringLiteral("Geometry"), QByteArray{});
        if (!geometry.isEmpty())
            restoreGeometry(geometry);
        const QByteArray header = settings.readEntry(QStringLiteral("MailboxHeader"), QByteArray{});
        if (!header.isEmpty())
            m_mailboxView->header()->restoreState(header);
    }

    void DeveloperOptionsDialog::saveUiState() const
    {
        KConfigGroup settings{KSharedConfig::openConfig(), QStringLiteral("DeveloperOptions")};
        settings.writeEntry(QStringLiteral("Geometry"), saveGeometry());
        settings.writeEntry(QStringLiteral("MailboxHeader"), m_mailboxView->header()->saveState());
        settings.sync();
    }

    void DeveloperOptionsDialog::closeEvent(QCloseEvent* event)
    {
        saveUiState();
        KPageDialog::closeEvent(event);
    }

} // namespace javelin::gui::developer
