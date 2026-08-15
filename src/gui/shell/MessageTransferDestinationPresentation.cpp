#include "gui/shell/MessageTransferDestinationPresentation.h"

#include "gui/mailboxes/MailboxPresentation.h"

#include <KLocalizedString>

#include <algorithm>
#include <ranges>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace javelin::gui::shell
{
    namespace
    {
        [[nodiscard]] std::vector<MessageTransferDestinationRow>
        writableRows(const std::string& accountId,
                     const std::vector<javelin::jmap::cache::MailboxTreeItem>& mailboxes)
        {
            const auto presentation =
                javelin::gui::mailboxes::buildMailboxPresentation(accountId, mailboxes);
            std::vector<MessageTransferDestinationRow> result;
            bool displayedSpecialUse = false;
            bool insertedUserSeparator = false;
            for (const auto& row :
                 javelin::gui::mailboxes::flattenMailboxPresentation(presentation))
            {
                const auto& destination = *row.node;
                if (!destination.mailbox.myRights.mayAddItems)
                    continue;
                const bool separatorBefore =
                    destination.group ==
                            javelin::gui::mailboxes::MailboxPresentationGroup::User &&
                    displayedSpecialUse && !insertedUserSeparator;
                result.push_back({
                    .accountId = accountId,
                    .mailboxId = destination.mailbox.id,
                    .mailboxName = destination.mailbox.name,
                    .role = destination.mailbox.role,
                    .depth = row.depth,
                    .separatorBefore = separatorBefore,
                });
                insertedUserSeparator = insertedUserSeparator || separatorBefore;
                displayedSpecialUse =
                    displayedSpecialUse ||
                    destination.group ==
                        javelin::gui::mailboxes::MailboxPresentationGroup::SpecialUse;
            }
            return result;
        }

        [[nodiscard]] QString baseLabel(
            const javelin::jmap::cache::CachedAccount& account,
            const MessageTransferAccountDisplayName& configuredDisplayName)
        {
            if (configuredDisplayName)
            {
                auto configured =
                    configuredDisplayName(QString::fromStdString(account.accountId)).trimmed();
                if (!configured.isEmpty())
                    return configured;
            }
            if (!account.name.empty())
                return QString::fromStdString(account.name);
            return i18n("Unnamed account");
        }

        [[nodiscard]] QString shortLocalId(const std::string& accountId)
        {
            const QString id = QString::fromStdString(accountId);
            return id.size() <= 8 ? id : id.left(8);
        }

        [[nodiscard]] QString uniqueLabel(
            const javelin::jmap::cache::CachedAccount& account, const QString& base,
            const bool baseIsDuplicate, std::unordered_set<QString>& used)
        {
            const auto available = [&used](const QString& candidate)
            {
                const auto key = candidate.toCaseFolded();
                if (used.contains(key))
                    return false;
                used.insert(key);
                return true;
            };
            if (!baseIsDuplicate && available(base))
                return base;

            const QString accountName = QString::fromStdString(account.name).trimmed();
            if (!accountName.isEmpty() && accountName.compare(base, Qt::CaseInsensitive) != 0)
            {
                const QString candidate = QStringLiteral("%1 — %2").arg(base, accountName);
                if (available(candidate))
                    return candidate;
            }

            const QString remoteId = QString::fromStdString(account.remoteAccountId).trimmed();
            if (!remoteId.isEmpty())
            {
                const QString candidate = QStringLiteral("%1 — %2").arg(base, remoteId);
                if (available(candidate))
                    return candidate;
            }

            const QString shortId = shortLocalId(account.accountId);
            const QString shortCandidate = QStringLiteral("%1 — %2").arg(base, shortId);
            if (available(shortCandidate))
                return shortCandidate;

            const QString fullCandidate =
                QStringLiteral("%1 — %2").arg(base, QString::fromStdString(account.accountId));
            static_cast<void>(available(fullCandidate));
            return fullCandidate;
        }
    } // namespace

    MessageTransferDestinationPresentation buildMessageTransferDestinationPresentation(
        std::string currentAccountId,
        const std::vector<javelin::jmap::cache::CachedAccount>& accounts,
        const std::unordered_map<
            std::string, std::vector<javelin::jmap::cache::MailboxTreeItem>>& mailboxesByAccount,
        const MessageTransferAccountDisplayName& configuredDisplayName)
    {
        MessageTransferDestinationPresentation result;
        if (const auto current = mailboxesByAccount.find(currentAccountId);
            current != mailboxesByAccount.end())
        {
            result.currentAccountRows = writableRows(currentAccountId, current->second);
        }

        struct Candidate
        {
            const javelin::jmap::cache::CachedAccount* account = nullptr;
            QString baseLabel;
            std::vector<MessageTransferDestinationRow> rows;
        };
        std::vector<Candidate> candidates;
        for (const auto& account : accounts)
        {
            if (account.accountId == currentAccountId || !account.hasMailCapability ||
                account.isReadOnly)
                continue;
            const auto mailboxes = mailboxesByAccount.find(account.accountId);
            if (mailboxes == mailboxesByAccount.end())
                continue;
            auto rows = writableRows(account.accountId, mailboxes->second);
            if (rows.empty())
                continue;
            candidates.push_back({
                .account = &account,
                .baseLabel = baseLabel(account, configuredDisplayName),
                .rows = std::move(rows),
            });
        }
        std::ranges::sort(candidates,
                          [](const Candidate& left, const Candidate& right)
                          {
                              const int labelOrder = QString::compare(
                                  left.baseLabel, right.baseLabel, Qt::CaseInsensitive);
                              if (labelOrder != 0)
                                  return labelOrder < 0;
                              return left.account->accountId < right.account->accountId;
                          });

        std::unordered_map<QString, std::size_t> labelCounts;
        for (const auto& candidate : candidates)
            ++labelCounts[candidate.baseLabel.toCaseFolded()];
        std::unordered_set<QString> usedLabels;
        for (auto& candidate : candidates)
        {
            const bool duplicate = labelCounts[candidate.baseLabel.toCaseFolded()] > 1;
            result.otherAccounts.push_back({
                .accountId = candidate.account->accountId,
                .label = uniqueLabel(*candidate.account, candidate.baseLabel, duplicate, usedLabels),
                .rows = std::move(candidate.rows),
            });
        }
        return result;
    }

} // namespace javelin::gui::shell
