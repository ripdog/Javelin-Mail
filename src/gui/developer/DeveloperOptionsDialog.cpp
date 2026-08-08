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
#include <QMessageBox>
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
        constexpr int sortValueRole = Qt::UserRole + 2;

        class MailboxSortFilterProxyModel final : public QSortFilterProxyModel
        {
          public:
            using QSortFilterProxyModel::QSortFilterProxyModel;

          protected:
            [[nodiscard]] bool lessThan(const QModelIndex& left,
                                        const QModelIndex& right) const override
            {
                if (!left.parent().isValid() && !right.parent().isValid())
                {
                    const bool sourceOrder = left.row() < right.row();
                    return sortOrder() == Qt::AscendingOrder ? sourceOrder : !sourceOrder;
                }

                if (left.column() == SqliteColumn || left.column() == BodiesColumn ||
                    left.column() == ReclaimableColumn)
                {
                    const auto leftValue = left.data(sortValueRole).toULongLong();
                    const auto rightValue = right.data(sortValueRole).toULongLong();
                    if (leftValue != rightValue)
                        return leftValue < rightValue;
                }

                const int columnComparison =
                    QString::localeAwareCompare(left.data().toString(), right.data().toString());
                if (columnComparison != 0)
                    return columnComparison < 0;

                const auto leftMailbox = left.siblingAtColumn(MailboxColumn).data().toString();
                const auto rightMailbox = right.siblingAtColumn(MailboxColumn).data().toString();
                const int mailboxComparison =
                    QString::localeAwareCompare(leftMailbox, rightMailbox);
                return mailboxComparison == 0 ? left.row() < right.row() : mailboxComparison < 0;
            }
        };

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
        javelin::app::DeveloperDiagnosticsPort& diagnostics,
        javelin::app::DeveloperMaintenancePort& maintenance, QWidget* parent)
        : KPageDialog(parent), m_diagnostics(diagnostics), m_maintenance(maintenance)
    {
        setWindowTitle(i18n("Developer Options"));
        setFaceType(KPageDialog::List);
        setStandardButtons(QDialogButtonBox::Close);
        setModal(false);
        resize(2200, 1440);

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
        m_filterModel = new MailboxSortFilterProxyModel(this);
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
        m_mailboxView->setSortingEnabled(true);
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

        auto* maintenanceLayout = new QHBoxLayout;
        maintenanceLayout->addStretch(1);
        m_clearSqliteButton =
            new QPushButton(QIcon::fromTheme(QStringLiteral("edit-clear-history")),
                            i18n("Clear SQLite + Bodies…"), page);
        m_clearSqliteButton->setObjectName(QStringLiteral("developerClearSqliteButton"));
        m_clearBodiesButton = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-delete")),
                                              i18n("Clear Body Cache…"), page);
        m_clearBodiesButton->setObjectName(QStringLiteral("developerClearBodiesButton"));
        m_clearSqliteButton->setEnabled(false);
        m_clearBodiesButton->setEnabled(false);
        maintenanceLayout->addWidget(m_clearSqliteButton);
        maintenanceLayout->addWidget(m_clearBodiesButton);
        pageLayout->addLayout(maintenanceLayout);

        auto* statusLayout = new QHBoxLayout;
        m_progress = new QProgressBar(page);
        m_progress->setProperty("_kde_no_animations", true);
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
        connect(m_clearSqliteButton, &QPushButton::clicked, this,
                &DeveloperOptionsDialog::clearSelectedSqlite);
        connect(m_clearBodiesButton, &QPushButton::clicked, this,
                &DeveloperOptionsDialog::clearSelectedBodies);
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
            auto* sqliteUsage = new QStandardItem(bytes(mailbox.usage.sqliteEstimatedBytes));
            sqliteUsage->setData(static_cast<qulonglong>(mailbox.usage.sqliteEstimatedBytes),
                                 sortValueRole);
            row.push_back(sqliteUsage);
            auto* bodyUsage = new QStandardItem(bytes(mailbox.usage.logicalBodyBytes));
            bodyUsage->setData(static_cast<qulonglong>(mailbox.usage.logicalBodyBytes),
                               sortValueRole);
            row.push_back(bodyUsage);
            auto* reclaimableUsage = new QStandardItem(bytes(mailbox.usage.reclaimableBodyBytes));
            reclaimableUsage->setData(static_cast<qulonglong>(mailbox.usage.reclaimableBodyBytes),
                                      sortValueRole);
            row.push_back(reclaimableUsage);
            row.push_back(new QStandardItem(mailbox.offlineDesired
                                                ? display(mailbox.offlineStatus)
                                                : i18nc("offline mailbox status", "No")));
            row.push_back(new QStandardItem(mailbox.measuredAt));
            for (auto* item : row)
                item->setEditable(false);
            accountItem->appendRow(row);
        }

        m_mailboxView->expandAll();
        if (!m_postRefreshStatus.isEmpty())
        {
            m_status->setText(std::exchange(m_postRefreshStatus, {}));
        }
        else
        {
            m_status->setText(i18np("Measured %1 cached mailbox.", "Measured %1 cached mailboxes.",
                                    m_mailboxes.size()));
        }
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
        {
            m_clearSqliteButton->setEnabled(false);
            m_clearBodiesButton->setEnabled(false);
            return;
        }
        const QModelIndex source = m_filterModel->mapToSource(current.siblingAtColumn(0));
        const QVariant value = source.data(mailboxIndexRole);
        if (!value.isValid())
        {
            m_details->clear();
            m_clearSqliteButton->setEnabled(false);
            m_clearBodiesButton->setEnabled(false);
            return;
        }
        const auto index = static_cast<std::size_t>(value.toULongLong());
        if (index >= m_mailboxes.size())
            return;
        m_details->setPlainText(detailsFor(m_mailboxes[index]));
        m_clearSqliteButton->setEnabled(!m_loading);
        m_clearBodiesButton->setEnabled(!m_loading);
    }

    const javelin::app::DeveloperMailboxRecord* DeveloperOptionsDialog::selectedMailbox() const
    {
        const QModelIndex current = m_mailboxView->currentIndex();
        if (!current.isValid())
            return nullptr;
        const QModelIndex source = m_filterModel->mapToSource(current.siblingAtColumn(0));
        const QVariant value = source.data(mailboxIndexRole);
        if (!value.isValid())
            return nullptr;
        const auto index = static_cast<std::size_t>(value.toULongLong());
        return index < m_mailboxes.size() ? &m_mailboxes[index] : nullptr;
    }

    void DeveloperOptionsDialog::clearSelectedSqlite()
    {
        const auto* mailbox = selectedMailbox();
        if (mailbox == nullptr)
            return;

        QMessageBox confirmation{
            QMessageBox::Warning, i18n("Clear SQLite and Body Cache"),
            i18n("Clear cached mailbox data and message bodies for %1?", mailbox->mailboxName),
            QMessageBox::Cancel, this};
        confirmation.setInformativeText(i18n(
            "This discards about %1 of SQLite-backed list and metadata state for %2 on %3 and "
            "removes its cached message bodies. Up to %4 of body storage can be reclaimed; %5 is "
            "shared with other mailboxes and remains available there. The mailbox, account "
            "settings, and active optimistic changes are preserved. The mailbox will be loaded "
            "again when it is next viewed.",
            bytes(mailbox->usage.sqliteEstimatedBytes), mailbox->mailboxName, mailbox->accountName,
            bytes(mailbox->usage.reclaimableBodyBytes), bytes(mailbox->usage.sharedBodyBytes)));
        auto* clearButton =
            confirmation.addButton(i18n("Clear SQLite and Bodies"), QMessageBox::DestructiveRole);
        confirmation.setDefaultButton(QMessageBox::Cancel);
        confirmation.exec();
        if (confirmation.clickedButton() != clearButton)
            return;

        runClear({.accountId = mailbox->accountId,
                  .mailboxId = mailbox->mailboxId,
                  .kind = javelin::app::DeveloperMailboxCacheKind::SqliteAndBodies,
                  .offlinePolicy = javelin::app::DeveloperOfflineClearPolicy::Preserve});
    }

    void DeveloperOptionsDialog::clearSelectedBodies()
    {
        const auto* mailbox = selectedMailbox();
        if (mailbox == nullptr)
            return;

        QMessageBox confirmation{QMessageBox::Warning, i18n("Clear Body Cache"),
                                 i18n("Clear cached message bodies for %1?", mailbox->mailboxName),
                                 QMessageBox::Cancel, this};
        confirmation.setInformativeText(
            i18n("%1 of message bodies are associated with this mailbox. About %2 is expected to "
                 "become reclaimable; %3 is shared with other mailboxes.",
                 bytes(mailbox->usage.logicalBodyBytes), bytes(mailbox->usage.reclaimableBodyBytes),
                 bytes(mailbox->usage.sharedBodyBytes)));

        QPushButton* disableAndClear = nullptr;
        QPushButton* clearAndRedownload = nullptr;
        if (mailbox->offlineDesired)
        {
            confirmation.setDetailedText(i18n(
                "%1 is configured for complete offline storage. Disabling offline storage avoids "
                "immediately downloading the bodies again.",
                mailbox->mailboxName));
            disableAndClear = confirmation.addButton(i18n("Disable Offline Storage and Clear"),
                                                     QMessageBox::AcceptRole);
            clearAndRedownload = confirmation.addButton(i18n("Clear and Allow Redownload"),
                                                        QMessageBox::DestructiveRole);
        }
        else
        {
            disableAndClear =
                confirmation.addButton(i18n("Clear Body Cache"), QMessageBox::DestructiveRole);
        }
        confirmation.setDefaultButton(QMessageBox::Cancel);
        confirmation.exec();
        if (confirmation.clickedButton() != disableAndClear &&
            confirmation.clickedButton() != clearAndRedownload)
            return;

        const auto policy =
            mailbox->offlineDesired && confirmation.clickedButton() == disableAndClear
                ? javelin::app::DeveloperOfflineClearPolicy::Disable
                : javelin::app::DeveloperOfflineClearPolicy::Preserve;
        runClear({.accountId = mailbox->accountId,
                  .mailboxId = mailbox->mailboxId,
                  .kind = javelin::app::DeveloperMailboxCacheKind::Bodies,
                  .offlinePolicy = policy});
    }

    void DeveloperOptionsDialog::runClear(javelin::app::DeveloperMailboxClearCommand command)
    {
        if (m_loading)
            return;
        setLoading(true);
        switch (command.kind)
        {
        case javelin::app::DeveloperMailboxCacheKind::Sqlite:
            m_status->setText(i18n("Clearing mailbox SQLite cache…"));
            break;
        case javelin::app::DeveloperMailboxCacheKind::Bodies:
            m_status->setText(i18n("Clearing mailbox body cache…"));
            break;
        case javelin::app::DeveloperMailboxCacheKind::SqliteAndBodies:
            m_status->setText(i18n("Clearing mailbox SQLite and body caches…"));
            break;
        }
        auto task = m_maintenance.clearMailboxCache(std::move(command));
        QCoro::connect(std::move(task), this,
                       [this](javelin::app::DeveloperMailboxClearResult result)
                       { applyClearResult(std::move(result)); });
    }

    void DeveloperOptionsDialog::applyClearResult(javelin::app::DeveloperMailboxClearResult result)
    {
        setLoading(false);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
        {
            m_status->setText(i18n("Mailbox cache clear failed: %1", error->message));
            return;
        }

        const auto& summary = std::get<javelin::app::DeveloperMailboxClearSummary>(result);
        if (summary.kind == javelin::app::DeveloperMailboxCacheKind::Sqlite)
        {
            m_postRefreshStatus = i18np("Discarded %1 cached SQLite row.",
                                        "Discarded %1 cached SQLite rows.", summary.rowsDiscarded);
        }
        else if (summary.kind == javelin::app::DeveloperMailboxCacheKind::Bodies)
        {
            m_postRefreshStatus = i18n(
                "Removed %1 mailbox projections and reclaimed %2. %3 remains deferred while in "
                "use.",
                summary.projectionsRemoved, bytes(summary.reclaimedBytes),
                bytes(summary.deferredBytes));
        }
        else
        {
            m_postRefreshStatus = i18n(
                "Cleared cached mailbox state and bodies, discarding %1 cache rows and reclaiming "
                "%2. %3 remains deferred while in use.",
                summary.rowsDiscarded, bytes(summary.reclaimedBytes), bytes(summary.deferredBytes));
        }
        refresh();
    }

    void DeveloperOptionsDialog::setLoading(const bool loading)
    {
        m_loading = loading;
        m_progress->setVisible(loading);
        m_refreshButton->setEnabled(!loading);
        const bool hasMailbox = selectedMailbox() != nullptr;
        m_clearSqliteButton->setEnabled(!loading && hasMailbox);
        m_clearBodiesButton->setEnabled(!loading && hasMailbox);
        if (loading)
            m_status->setText(i18n("Calculating mailbox cache usage…"));
    }

    void DeveloperOptionsDialog::restoreUiState()
    {
        const KConfigGroup settings{KSharedConfig::openConfig(),
                                    QStringLiteral("DeveloperOptions")};
        const QByteArray geometry = settings.readEntry(QStringLiteral("GeometryV2"), QByteArray{});
        if (!geometry.isEmpty())
            restoreGeometry(geometry);
        const QByteArray header = settings.readEntry(QStringLiteral("MailboxHeader"), QByteArray{});
        if (!header.isEmpty())
            m_mailboxView->header()->restoreState(header);
        const int sortColumn = settings.readEntry(QStringLiteral("MailboxSortColumn"),
                                                  static_cast<int>(MailboxColumn));
        const auto sortOrder = static_cast<Qt::SortOrder>(settings.readEntry(
            QStringLiteral("MailboxSortOrder"), static_cast<int>(Qt::AscendingOrder)));
        m_mailboxView->sortByColumn(
            sortColumn >= MailboxColumn && sortColumn < ColumnCount ? sortColumn : MailboxColumn,
            sortOrder == Qt::DescendingOrder ? Qt::DescendingOrder : Qt::AscendingOrder);
    }

    void DeveloperOptionsDialog::saveUiState() const
    {
        KConfigGroup settings{KSharedConfig::openConfig(), QStringLiteral("DeveloperOptions")};
        settings.writeEntry(QStringLiteral("GeometryV2"), saveGeometry());
        settings.writeEntry(QStringLiteral("MailboxHeader"), m_mailboxView->header()->saveState());
        settings.writeEntry(QStringLiteral("MailboxSortColumn"),
                            m_mailboxView->header()->sortIndicatorSection());
        settings.writeEntry(QStringLiteral("MailboxSortOrder"),
                            static_cast<int>(m_mailboxView->header()->sortIndicatorOrder()));
        settings.sync();
    }

    void DeveloperOptionsDialog::closeEvent(QCloseEvent* event)
    {
        saveUiState();
        KPageDialog::closeEvent(event);
    }

} // namespace javelin::gui::developer
