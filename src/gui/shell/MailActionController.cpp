#include "gui/shell/MailActionController.h"

#include "app/MailApplicationPorts.h"
#include "gui/IconUtils.h"
#include "gui/messages/MessageListModel.h"
#include "gui/shell/EmailContextMenuLayout.h"
#include "gui/shell/MessageActionPolicy.h"
#include "gui/shell/MessageCommandController.h"
#include "gui/shell/MessageSelectionController.h"
#include "gui/shell/QuickFilterController.h"
#include "jmap/cache/MailTagReadRepository.h"
#include "jmap/cache/MailboxReadRepository.h"

#include <KLocalizedString>

#include <QCoroTask>

#include <QAction>
#include <QColorDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPalette>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace javelin::gui::shell
{
    namespace
    {
        [[nodiscard]] std::optional<javelin::jmap::cache::MailboxTreeItem>
        findMailboxByRole(javelin::jmap::cache::MailboxReader& mailboxReader,
                          const std::string_view accountId, const std::string_view role)
        {
            const auto result = mailboxReader.listMailboxTree(accountId);
            const auto* mailboxes =
                std::get_if<std::vector<javelin::jmap::cache::MailboxTreeItem>>(&result);
            if (mailboxes == nullptr)
                return std::nullopt;
            const auto mailbox = std::ranges::find_if(
                *mailboxes, [role](const auto& item)
                { return item.role == std::optional<std::string>{std::string{role}}; });
            return mailbox == mailboxes->end() ? std::nullopt : std::optional{*mailbox};
        }

        [[nodiscard]] bool indexIsUnread(const QModelIndex& index)
        {
            return index.isValid() &&
                   index.data(javelin::gui::messages::MessageListModel::IsUnreadRole).toBool();
        }

        [[nodiscard]] QIcon tagColorIcon(const QStringView colorName)
        {
            const QColor color{colorName.toString()};
            if (!color.isValid())
                return {};
            QPixmap swatch{14, 14};
            swatch.fill(color);
            return QIcon{swatch};
        }

        [[nodiscard]] std::optional<std::vector<std::string>>
        explicitSelectionEmailIds(const javelin::app::MessageSelection& selection)
        {
            std::vector<std::string> emailIds;
            std::unordered_set<std::string> seen;
            const auto append = [&emailIds, &seen](const std::string_view emailId)
            {
                if (!emailId.empty() && seen.emplace(emailId).second)
                    emailIds.emplace_back(emailId);
            };
            for (const auto& item : selection)
            {
                if (const auto* email = std::get_if<javelin::app::SelectedEmail>(&item))
                {
                    append(email->emailId);
                    continue;
                }
                return std::nullopt;
            }
            return emailIds;
        }
    } // namespace

    MailActionController::MailActionController(
        javelin::jmap::cache::MailboxReader& mailboxReader,
        javelin::jmap::cache::MailTagReader& mailTagReader,
        javelin::app::MailCommandPort& mailCommandPort,
        MessageSelectionController& selectionController,
        MessageCommandController& commandController, QuickFilterController& quickFilterController,
        QListView& messageView, QMenu& tagsMenu, QWidget& parentWidget, MailActions actions,
        std::function<void(QString, int)> showStatus,
        std::function<void(const javelin::jmap::OperationError&)> showError,
        std::function<void()> refreshMessageList, QObject* parent)
        : QObject(parent), m_mailboxReader(mailboxReader), m_mailTagReader(mailTagReader),
          m_mailCommandPort(mailCommandPort), m_selectionController(selectionController),
          m_commandController(commandController), m_quickFilterController(quickFilterController),
          m_messageView(messageView), m_tagsMenu(tagsMenu), m_parentWidget(parentWidget),
          m_actions(actions), m_showStatus(std::move(showStatus)),
          m_showError(std::move(showError)), m_refreshMessageList(std::move(refreshMessageList))
    {
        connect(&m_actions.archive, &QAction::triggered, this,
                [this]
                {
                    m_commandController.archiveSelection(activeAccountId(), activeMailboxId(),
                                                         activeTabIsSearch());
                });
        connect(&m_actions.markUnread, &QAction::triggered, this, [this]
                { m_commandController.markSelectionUnread(activeAccountId(), activeMailboxId()); });
        connect(&m_actions.star, &QAction::triggered, this,
                [this]
                {
                    m_commandController.setSelectionFlagged(activeAccountId(), activeMailboxId(),
                                                            !selectedMessagesAreStarred());
                });
        connect(&m_actions.junk, &QAction::triggered, this,
                [this]
                {
                    m_commandController.setSelectionJunk(activeAccountId(), activeMailboxId(),
                                                         !selectedMessagesAreJunk());
                });
        connect(&m_actions.deleteFromMailbox, &QAction::triggered, this, [this]
                { m_commandController.deleteSelection(activeAccountId(), activeMailboxId()); });
        connect(&m_actions.permanentDelete, &QAction::triggered, this,
                [this]
                {
                    m_commandController.permanentlyDeleteSelection(activeAccountId(),
                                                                   activeMailboxId());
                });
        if (auto* moveMenu = m_actions.move.menu(); moveMenu != nullptr)
        {
            connect(moveMenu, &QMenu::aboutToShow, this,
                    [this, moveMenu] { rebuildTransferMenu(*moveMenu, true); });
        }
        if (auto* copyMenu = m_actions.copy.menu(); copyMenu != nullptr)
        {
            connect(copyMenu, &QMenu::aboutToShow, this,
                    [this, copyMenu] { rebuildTransferMenu(*copyMenu, false); });
        }
        connect(&m_tagsMenu, &QMenu::aboutToShow, this, &MailActionController::rebuildTagsMenu);
    }

    std::optional<std::string> MailActionController::activeAccountId() const
    {
        return m_activeTab == nullptr ? std::nullopt : tabAccountId(*m_activeTab);
    }

    std::optional<std::string> MailActionController::activeMailboxId() const
    {
        return m_activeTab == nullptr ? std::nullopt : tabMailboxId(*m_activeTab);
    }

    bool MailActionController::activeTabIsSearch() const
    {
        return m_activeTab != nullptr && tabKind(*m_activeTab) == TabKind::Search;
    }

    void MailActionController::activate(const TabState* tab)
    {
        m_activeTab = tab;
        update();
    }

    void MailActionController::update()
    {
        const auto selectedIds = m_selectionController.selectedEmailIds();
        const auto selection = m_commandController.selectedActionItems();
        const auto accountId = activeAccountId();
        const auto mailboxId = activeMailboxId();
        const auto draftsMailbox =
            accountId.has_value()
                ? findMailboxByRole(m_mailboxReader, *accountId, "drafts")
                : std::optional<javelin::jmap::cache::MailboxTreeItem>{std::nullopt};
        const auto* selectionModel = m_messageView.selectionModel();
        const bool hasReadSelection =
            selectionModel != nullptr &&
            std::ranges::any_of(selectionModel->selectedRows(),
                                [](const QModelIndex& index) { return !indexIsUnread(index); });
        const auto actions = messageActionAvailability({
            .tabKind = m_activeTab == nullptr ? std::optional<TabKind>{std::nullopt}
                                              : std::optional<TabKind>{tabKind(*m_activeTab)},
            .hasAccount = accountId.has_value(),
            .hasMailbox = mailboxId.has_value(),
            .selectedCount = selectedIds.size(),
            .activeMailboxIsDrafts = mailboxId.has_value() && draftsMailbox.has_value() &&
                                     *mailboxId == draftsMailbox->id,
            .hasReadSelection = hasReadSelection,
        });

        m_actions.newMessage.setEnabled(actions.newMessage);
        m_actions.reply.setEnabled(actions.reply);
        m_actions.replyAll.setEnabled(actions.replyAll);
        m_actions.forward.setEnabled(actions.forward);
        m_actions.editDraft.setEnabled(actions.editDraft);
        m_actions.archive.setEnabled(actions.archive);
        m_actions.markUnread.setEnabled(actions.markUnread);
        m_actions.star.setEnabled(actions.star);
        m_actions.star.setText(selectedMessagesAreStarred() ? i18nc("@action", "&Unstar")
                                                            : i18nc("@action", "&Star"));
        m_actions.junk.setEnabled(actions.junk);
        m_actions.junk.setText(selectedMessagesAreJunk() ? i18nc("@action", "Not &Junk")
                                                         : i18nc("@action", "&Junk"));
        const bool messageListContext =
            m_activeTab != nullptr &&
            (tabKind(*m_activeTab) == TabKind::Mailbox || tabKind(*m_activeTab) == TabKind::Search);
        m_actions.tags.setEnabled(accountId.has_value() && messageListContext);
        m_actions.deleteFromMailbox.setEnabled(actions.deleteFromMailbox);
        m_actions.permanentDelete.setEnabled(actions.permanentDelete);
        m_actions.move.setEnabled(actions.move);
        m_actions.copy.setEnabled(actions.copy);
        const bool oneExplicitEmail =
            selection.size() == 1 &&
            std::holds_alternative<javelin::app::SelectedEmail>(selection[0]);
        m_actions.save.setEnabled(accountId.has_value() && !selection.empty());
        m_actions.save.setText(oneExplicitEmail ? i18nc("@action", "Save Message As…")
                                                : i18nc("@action", "Save Messages…"));
        m_actions.viewSource.setEnabled(actions.viewSource);
        const auto currentIndex = m_messageView.currentIndex();
        const auto senderEmail =
            currentIndex.data(javelin::gui::messages::MessageListModel::SenderEmailRole)
                .toString()
                .trimmed();
        m_actions.findSender.setEnabled(accountId.has_value() && messageListContext &&
                                        currentIndex.isValid() && !senderEmail.isEmpty());

        // Transfer-menu actions capture the current selection. KCommandBar indexes submenu actions
        // without emitting QMenu::aboutToShow, so never leave an old selection's actions available
        // for it to discover. The ordinary menu-opening path rebuilds these on demand.
        if (auto* menu = m_actions.move.menu(); menu != nullptr)
            menu->clear();
        if (auto* menu = m_actions.copy.menu(); menu != nullptr)
            menu->clear();
        m_tagsMenu.clear();
    }

    void MailActionController::rebuildTransferMenu(QMenu& menu, const bool move)
    {
        menu.clear();
        const auto accountId = activeAccountId();
        const auto sourceMailboxId =
            activeTabIsSearch() ? std::optional<std::string>{std::nullopt} : activeMailboxId();
        if (!accountId.has_value() || (!sourceMailboxId.has_value() && !activeTabIsSearch()))
        {
            auto* unavailable = menu.addAction(i18n("No transfer destinations available"));
            unavailable->setEnabled(false);
            return;
        }

        const auto selection = m_commandController.selectedActionItems();
        if (selection.empty())
        {
            auto* unavailable = menu.addAction(i18n("No messages selected"));
            unavailable->setEnabled(false);
            return;
        }

        const bool hasDestinations =
            move ? m_commandController.populateDestinationMenus(&menu, nullptr, *accountId,
                                                                sourceMailboxId, selection)
                 : m_commandController.populateDestinationMenus(nullptr, &menu, *accountId,
                                                                sourceMailboxId, selection);
        if (!hasDestinations)
        {
            auto* unavailable = menu.addAction(i18n("No writable mailboxes available"));
            unavailable->setEnabled(false);
        }
    }

    void MailActionController::configureContextMenu(
        QMenu& menu, std::function<std::vector<QString>()> configuredLayout,
        std::function<void(const QList<QAction*>&)> replaceActionList)
    {
        m_contextMenu = &menu;
        m_configuredContextMenuLayout = std::move(configuredLayout);
        m_replaceContextMenuActionList = std::move(replaceActionList);
    }

    void MailActionController::showContextMenu(
        const QPoint& position, std::function<void(QModelIndex)> findConversationsWithSender)
    {
        const QModelIndex index = m_messageView.indexAt(position);
        if (!index.isValid())
            return;

        const auto accountId = activeAccountId();
        const auto sourceMailboxId = activeMailboxId();
        const auto clickedEmailId =
            index.data(javelin::gui::messages::MessageListModel::EmailIdRole)
                .toString()
                .toStdString();
        if (!accountId.has_value() || clickedEmailId.empty())
            return;

        if (!m_messageView.selectionModel()->isSelected(index))
        {
            m_messageView.selectionModel()->select(index, QItemSelectionModel::ClearAndSelect |
                                                              QItemSelectionModel::Rows);
            m_messageView.setCurrentIndex(index);
        }
        update();
        const auto selection = m_commandController.selectedActionItems();
        const auto draftsMailbox = findMailboxByRole(m_mailboxReader, *accountId, "drafts");
        const bool activeMailboxIsDrafts = sourceMailboxId.has_value() &&
                                           draftsMailbox.has_value() &&
                                           *sourceMailboxId == draftsMailbox->id;

        const auto senderEmail =
            index.data(javelin::gui::messages::MessageListModel::SenderEmailRole)
                .toString()
                .trimmed();
        if (m_contextMenu == nullptr || !m_configuredContextMenuLayout ||
            !m_replaceContextMenuActionList)
            return;

        m_replaceContextMenuActionList({});
        qDeleteAll(m_contextMenuObjects);
        m_contextMenuObjects.clear();

        QMenu* moveMenu = nullptr;
        QMenu* copyMenu = nullptr;
        const auto destinationMenu = [this, &moveMenu, &copyMenu, &selection, &accountId,
                                      &sourceMailboxId](const bool move) -> QMenu*
        {
            auto*& target = move ? moveMenu : copyMenu;
            if (target != nullptr)
                return target;
            target = new QMenu(move ? i18n("Move to") : i18n("Copy to"), m_contextMenu);
            target->setIcon(move ? QIcon::fromTheme(QStringLiteral("mail-move"))
                                 : QIcon::fromTheme(QStringLiteral("edit-copy")));
            m_contextMenuObjects.push_back(target);
            const bool hasDestinations =
                move ? m_commandController.populateDestinationMenus(target, nullptr, *accountId,
                                                                    sourceMailboxId, selection)
                     : m_commandController.populateDestinationMenus(nullptr, target, *accountId,
                                                                    sourceMailboxId, selection);
            target->setProperty("javelinHasTransferDestinations", hasDestinations);
            return target;
        };

        const auto actionForId = [&](const QString& id) -> QAction*
        {
            if (id == QStringLiteral("compose_edit_draft"))
                return activeMailboxIsDrafts ? &m_actions.editDraft : nullptr;
            if (id == QStringLiteral("compose_reply"))
                return &m_actions.reply;
            if (id == QStringLiteral("compose_reply_all"))
                return &m_actions.replyAll;
            if (id == QStringLiteral("compose_forward"))
                return &m_actions.forward;
            if (id == QStringLiteral("archive_email"))
                return &m_actions.archive;
            if (id == QStringLiteral("delete_email"))
                return sourceMailboxId.has_value() ? &m_actions.deleteFromMailbox : nullptr;
            if (id == QStringLiteral("mark_email_unread"))
                return &m_actions.markUnread;
            if (id == QStringLiteral("toggle_email_starred"))
                return &m_actions.star;
            if (id == QStringLiteral("tag_email"))
                return &m_actions.tags;
            if (id == QStringLiteral("move_email"))
            {
                if (!sourceMailboxId.has_value() && !activeTabIsSearch())
                    return nullptr;
                auto* menu = destinationMenu(true);
                return menu->property("javelinHasTransferDestinations").toBool()
                           ? menu->menuAction()
                           : nullptr;
            }
            if (id == QStringLiteral("copy_email"))
            {
                if (!sourceMailboxId.has_value() && !activeTabIsSearch())
                    return nullptr;
                auto* menu = destinationMenu(false);
                return menu->property("javelinHasTransferDestinations").toBool()
                           ? menu->menuAction()
                           : nullptr;
            }
            if (id == QStringLiteral("toggle_email_junk"))
                return &m_actions.junk;
            if (id == QStringLiteral("find_conversations_with_sender"))
            {
                if (senderEmail.isEmpty())
                    return nullptr;
                auto* action =
                    new QAction(i18n("Find all conversations with %1", senderEmail), m_contextMenu);
                m_contextMenuObjects.push_back(action);
                connect(action, &QAction::triggered, this, [index, findConversationsWithSender]
                        { findConversationsWithSender(index); });
                return action;
            }
            if (id == QStringLiteral("save_message"))
                return &m_actions.save;
            if (id == QStringLiteral("view_message_source"))
                return &m_actions.viewSource;
            if (id == QStringLiteral("permanently_delete_email"))
                return &m_actions.permanentDelete;
            return nullptr;
        };

        QList<QAction*> contextActions;
        bool separatorPending = false;
        for (const auto& id : effectiveEmailContextMenuLayout(m_configuredContextMenuLayout()))
        {
            if (id == emailContextMenuSeparatorId())
            {
                separatorPending = !contextActions.empty();
                continue;
            }
            auto* action = actionForId(id);
            if (action == nullptr)
                continue;
            if (separatorPending)
            {
                auto* separator = new QAction(m_contextMenu);
                separator->setSeparator(true);
                m_contextMenuObjects.push_back(separator);
                contextActions.push_back(separator);
                separatorPending = false;
            }
            contextActions.push_back(action);
        }
        m_replaceContextMenuActionList(contextActions);

        m_contextMenu->exec(m_messageView.viewport()->mapToGlobal(position));
    }

    void MailActionController::rebuildTagsMenu()
    {
        m_tagsMenu.clear();
        const auto accountId = activeAccountId();
        if (!accountId.has_value())
        {
            auto* unavailable = m_tagsMenu.addAction(i18n("No mail account selected"));
            unavailable->setEnabled(false);
            return;
        }

        std::vector<std::string> selectedEmailIds;
        const auto selection = m_commandController.selectedActionItems();
        if (!selection.empty())
        {
            if (auto explicitIds = explicitSelectionEmailIds(selection))
                selectedEmailIds = std::move(*explicitIds);
        }

        std::unordered_map<std::string, std::size_t> selectedKeywordCounts;
        if (!selectedEmailIds.empty())
        {
            const auto memberships =
                m_mailTagReader.listEmailKeywordMemberships(*accountId, selectedEmailIds);
            if (const auto* values =
                    std::get_if<std::vector<javelin::jmap::cache::EmailKeywordMembership>>(
                        &memberships))
            {
                for (const auto& membership : *values)
                    for (const auto& keyword : membership.keywords)
                        ++selectedKeywordCounts[keyword];
            }
        }

        const auto definitions = m_mailTagReader.listTagDefinitions(*accountId);
        const auto* tags =
            std::get_if<std::vector<javelin::jmap::cache::TagDefinition>>(&definitions);
        if (tags == nullptr)
        {
            auto* errorAction = m_tagsMenu.addAction(i18n("Unable to load tags"));
            errorAction->setEnabled(false);
        }
        else if (tags->empty())
        {
            auto* emptyAction = m_tagsMenu.addAction(i18n("No tags yet"));
            emptyAction->setEnabled(false);
        }
        else
        {
            for (const auto& tag : *tags)
            {
                const auto count = selectedKeywordCounts[tag.keyword];
                const bool allSelected =
                    !selectedEmailIds.empty() && count == selectedEmailIds.size();
                const bool someSelected = count > 0 && !allSelected;
                auto label = tag.displayName;
                if (someSelected)
                    label += i18nc("tag is present on only part of a selection", " (some)");
                auto* action = m_tagsMenu.addAction(tagColorIcon(tag.color), label);
                action->setCheckable(true);
                action->setChecked(allSelected);
                action->setEnabled(!selectedEmailIds.empty());
                connect(action, &QAction::triggered, this,
                        [this, keyword = tag.keyword, allSelected]
                        {
                            m_commandController.setSelectionTag(
                                activeAccountId(), activeMailboxId(), keyword, !allSelected);
                        });
            }
        }

        m_tagsMenu.addSeparator();
        auto* newTag =
            m_tagsMenu.addAction(QIcon::fromTheme(QStringLiteral("list-add")), i18n("New Tag…"));
        connect(newTag, &QAction::triggered, this, [this] { createTag(true); });
        auto* manageTags = m_tagsMenu.addAction(QIcon::fromTheme(QStringLiteral("configure")),
                                                i18n("Manage Tags…"));
        connect(manageTags, &QAction::triggered, this, &MailActionController::showTagManager);
    }

    void MailActionController::createTag(const bool applyToSelection)
    {
        const auto accountId = activeAccountId();
        if (!accountId.has_value())
            return;
        bool accepted = false;
        const auto name = QInputDialog::getText(&m_parentWidget, i18n("New Tag"), i18n("Tag name:"),
                                                QLineEdit::Normal, {}, &accepted)
                              .trimmed();
        if (!accepted || name.isEmpty())
            return;
        const auto color = QColorDialog::getColor(
            m_parentWidget.palette().color(QPalette::Active, QPalette::Highlight), &m_parentWidget,
            i18n("Tag Colour"));
        if (!color.isValid())
            return;

        auto task = m_mailCommandPort.saveTagDefinition(javelin::app::SaveMailTagDefinition{
            .accountId = *accountId,
            .keyword = std::nullopt,
            .displayName = name.toStdString(),
            .color = color.name(QColor::HexRgb).toStdString(),
        });
        QCoro::connect(
            std::move(task), this,
            [this, accountId = *accountId,
             applyToSelection](javelin::app::SaveMailTagDefinitionResult result)
            {
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    m_showError(*error);
                    return;
                }
                const auto& tag = std::get<javelin::app::MailTagDefinition>(result);
                m_showStatus(i18n("Created tag %1", QString::fromStdString(tag.displayName)), 5000);
                m_quickFilterController.rebuildTagsMenu();
                m_refreshMessageList();
                if (applyToSelection && activeAccountId() == std::optional<std::string>{accountId})
                {
                    m_commandController.setSelectionTag(accountId, activeMailboxId(), tag.keyword,
                                                        true);
                }
            });
    }

    void MailActionController::showTagManager()
    {
        const auto accountId = activeAccountId();
        if (!accountId.has_value())
            return;

        QDialog dialog{&m_parentWidget};
        dialog.setWindowTitle(i18n("Manage Tags"));
        dialog.resize(480, 360);
        auto* layout = new QVBoxLayout(&dialog);
        auto* list = new QListWidget(&dialog);
        list->setSelectionMode(QAbstractItemView::SingleSelection);
        layout->addWidget(list, 1);

        auto* controls = new QHBoxLayout;
        auto* addButton =
            new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), i18n("New…"), &dialog);
        auto* importButton = new QPushButton(QIcon::fromTheme(QStringLiteral("document-import")),
                                             i18n("Import Keyword…"), &dialog);
        auto* renameButton = new QPushButton(i18n("Rename…"), &dialog);
        auto* colorButton = new QPushButton(i18n("Colour…"), &dialog);
        auto* deleteButton = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-delete")),
                                             i18n("Delete"), &dialog);
        controls->addWidget(addButton);
        controls->addWidget(importButton);
        controls->addWidget(renameButton);
        controls->addWidget(colorButton);
        controls->addWidget(deleteButton);
        controls->addStretch(1);
        layout->addLayout(controls);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);

        const QPointer<QListWidget> guardedList{list};
        const auto reload = [this, guardedList, accountId = *accountId]()
        {
            if (guardedList == nullptr)
                return;
            const auto previous = guardedList->currentItem() != nullptr
                                      ? guardedList->currentItem()->data(Qt::UserRole).toString()
                                      : QString{};
            guardedList->clear();
            const auto definitions = m_mailTagReader.listTagDefinitions(accountId);
            const auto* tags =
                std::get_if<std::vector<javelin::jmap::cache::TagDefinition>>(&definitions);
            if (tags == nullptr)
                return;
            for (const auto& tag : *tags)
            {
                auto* item =
                    new QListWidgetItem(tagColorIcon(tag.color), tag.displayName, guardedList);
                item->setData(Qt::UserRole, QString::fromStdString(tag.keyword));
                item->setData(Qt::UserRole + 1, tag.color);
                if (!previous.isEmpty() && item->data(Qt::UserRole).toString() == previous)
                    guardedList->setCurrentItem(item);
            }
            if (guardedList->currentItem() == nullptr && guardedList->count() > 0)
                guardedList->setCurrentRow(0);
        };
        reload();

        const auto save = [this, guardedList, reload, accountId = *accountId](
                              std::optional<std::string> keyword, QString name, QString color)
        {
            auto task = m_mailCommandPort.saveTagDefinition(javelin::app::SaveMailTagDefinition{
                .accountId = accountId,
                .keyword = std::move(keyword),
                .displayName = name.toStdString(),
                .color = color.toStdString(),
            });
            QCoro::connect(
                std::move(task), this,
                [this, guardedList, reload](javelin::app::SaveMailTagDefinitionResult result)
                {
                    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                    {
                        m_showError(*error);
                        return;
                    }
                    if (guardedList != nullptr)
                        reload();
                    m_quickFilterController.rebuildTagsMenu();
                    m_refreshMessageList();
                });
        };

        connect(addButton, &QPushButton::clicked, &dialog,
                [this, save]
                {
                    bool accepted = false;
                    const auto name =
                        QInputDialog::getText(&m_parentWidget, i18n("New Tag"), i18n("Tag name:"),
                                              QLineEdit::Normal, {}, &accepted)
                            .trimmed();
                    if (!accepted || name.isEmpty())
                        return;
                    const auto color = QColorDialog::getColor(
                        m_parentWidget.palette().color(QPalette::Active, QPalette::Highlight),
                        &m_parentWidget, i18n("Tag Colour"));
                    if (!color.isValid())
                        return;
                    save(std::nullopt, name, color.name(QColor::HexRgb));
                });
        connect(importButton, &QPushButton::clicked, &dialog,
                [this, save, accountId = *accountId]
                {
                    const auto rawKeywords = m_mailTagReader.listUserKeywords(accountId);
                    const auto definedKeywords = m_mailTagReader.listTagKeywords(accountId);
                    const auto* raw = std::get_if<std::vector<std::string>>(&rawKeywords);
                    const auto* defined = std::get_if<std::vector<std::string>>(&definedKeywords);
                    if (raw == nullptr || defined == nullptr)
                    {
                        m_showStatus(i18n("Could not load existing message keywords."), 5000);
                        return;
                    }

                    QStringList candidates;
                    for (const auto& keyword : *raw)
                    {
                        if (!std::ranges::contains(*defined, keyword))
                            candidates.push_back(QString::fromStdString(keyword));
                    }
                    if (candidates.empty())
                    {
                        QMessageBox::information(&m_parentWidget, i18n("Import Keyword"),
                                                 i18n("No unclaimed message keywords were found."));
                        return;
                    }

                    bool accepted = false;
                    const auto keyword = QInputDialog::getItem(
                        &m_parentWidget, i18n("Import Keyword"), i18n("Existing keyword:"),
                        candidates, 0, false, &accepted);
                    if (!accepted || keyword.isEmpty())
                        return;
                    const auto name = QInputDialog::getText(&m_parentWidget, i18n("Import Keyword"),
                                                            i18n("Tag name:"), QLineEdit::Normal,
                                                            keyword, &accepted)
                                          .trimmed();
                    if (!accepted || name.isEmpty())
                        return;
                    const auto color = QColorDialog::getColor(
                        m_parentWidget.palette().color(QPalette::Active, QPalette::Highlight),
                        &m_parentWidget, i18n("Tag Colour"));
                    if (!color.isValid())
                        return;
                    save(keyword.toStdString(), name, color.name(QColor::HexRgb));
                });
        connect(renameButton, &QPushButton::clicked, &dialog,
                [this, list, save]
                {
                    auto* item = list->currentItem();
                    if (item == nullptr)
                        return;
                    bool accepted = false;
                    const auto name = QInputDialog::getText(&m_parentWidget, i18n("Rename Tag"),
                                                            i18n("Tag name:"), QLineEdit::Normal,
                                                            item->text(), &accepted)
                                          .trimmed();
                    if (!accepted || name.isEmpty())
                        return;
                    save(item->data(Qt::UserRole).toString().toStdString(), name,
                         item->data(Qt::UserRole + 1).toString());
                });
        connect(colorButton, &QPushButton::clicked, &dialog,
                [this, list, save]
                {
                    auto* item = list->currentItem();
                    if (item == nullptr)
                        return;
                    QColor current{item->data(Qt::UserRole + 1).toString()};
                    if (!current.isValid())
                        current =
                            m_parentWidget.palette().color(QPalette::Active, QPalette::Highlight);
                    const auto color =
                        QColorDialog::getColor(current, &m_parentWidget, i18n("Tag Colour"));
                    if (!color.isValid())
                        return;
                    save(item->data(Qt::UserRole).toString().toStdString(), item->text(),
                         color.name(QColor::HexRgb));
                });
        connect(deleteButton, &QPushButton::clicked, &dialog,
                [this, list, accountId = *accountId]
                {
                    auto* item = list->currentItem();
                    if (item == nullptr)
                        return;
                    QMessageBox confirmation{QMessageBox::Question, i18n("Delete Tag"),
                                             i18n("Delete “%1”?\n\nThis will remove the tag from "
                                                  "all messages and delete it from your tag list.",
                                                  item->text()),
                                             QMessageBox::NoButton, &m_parentWidget};
                    auto* deleteTagButton =
                        confirmation.addButton(i18n("Delete"), QMessageBox::DestructiveRole);
                    confirmation.addButton(QMessageBox::Cancel);
                    confirmation.exec();
                    if (confirmation.clickedButton() != deleteTagButton)
                        return;
                    const auto keyword = item->data(Qt::UserRole).toString().toStdString();
                    auto task = m_mailCommandPort.deleteTag(accountId, keyword);
                    QCoro::connect(std::move(task), this,
                                   [this, keyword](javelin::app::QueuedMailTagDeletionResult result)
                                   {
                                       if (const auto* error =
                                               std::get_if<javelin::jmap::OperationError>(&result))
                                       {
                                           m_showError(*error);
                                           return;
                                       }
                                       m_quickFilterController.removeTag(keyword);
                                       m_refreshMessageList();
                                       m_showStatus(
                                           i18n("Tag deletion queued in Background Tasks."), 5000);
                                   });
                    delete list->takeItem(list->row(item));
                });
        dialog.exec();
    }

    bool MailActionController::selectedMessagesAreStarred() const
    {
        const auto* selectionModel = m_messageView.selectionModel();
        if (selectionModel == nullptr)
            return false;
        auto rows = selectionModel->selectedRows();
        if (rows.empty() && m_messageView.currentIndex().isValid())
            rows.push_back(m_messageView.currentIndex());
        return !rows.empty() &&
               std::ranges::all_of(
                   rows,
                   [](const QModelIndex& index)
                   {
                       return index.data(javelin::gui::messages::MessageListModel::IsFlaggedRole)
                           .toBool();
                   });
    }

    bool MailActionController::selectedMessagesAreJunk() const
    {
        const auto* selectionModel = m_messageView.selectionModel();
        if (selectionModel == nullptr)
            return false;
        auto rows = selectionModel->selectedRows();
        if (rows.empty() && m_messageView.currentIndex().isValid())
            rows.push_back(m_messageView.currentIndex());
        return !rows.empty() &&
               std::ranges::all_of(
                   rows,
                   [](const QModelIndex& index)
                   {
                       return index.data(javelin::gui::messages::MessageListModel::IsJunkRole)
                           .toBool();
                   });
    }
} // namespace javelin::gui::shell
