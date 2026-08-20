#include "gui/shell/MessageTransferDestinationPresentation.h"

#include "gui/mailboxes/MailboxPresentation.h"

#include <KLocalizedString>

#include <QStringList>

#include <algorithm>
#include <optional>
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

            struct FlatRow
            {
                const javelin::gui::mailboxes::MailboxPresentationNode* node = nullptr;
                std::size_t depth = 0;
                QString path;
            };
            std::vector<FlatRow> flattened;
            const auto append =
                [&flattened](const auto& self,
                             const javelin::gui::mailboxes::MailboxPresentationNode& node,
                             const std::size_t depth, const QString& parentPath) -> void
            {
                const auto name = QString::fromStdString(node.mailbox.name);
                const auto path =
                    parentPath.isEmpty() ? name : QStringLiteral("%1 / %2").arg(parentPath, name);
                flattened.push_back({.node = &node, .depth = depth, .path = path});
                for (const auto& child : node.children)
                    self(self, child, depth + 1, path);
            };
            for (const auto& root : presentation.roots)
                append(append, root, 0, {});

            std::vector<MessageTransferDestinationRow> result;
            bool displayedSpecialUse = false;
            bool insertedUserSeparator = false;
            for (const auto& row : flattened)
            {
                const auto& destination = *row.node;
                if (!destination.mailbox.myRights.mayAddItems)
                    continue;
                const bool separatorBefore =
                    destination.group == javelin::gui::mailboxes::MailboxPresentationGroup::User &&
                    displayedSpecialUse && !insertedUserSeparator;
                result.push_back({
                    .accountId = accountId,
                    .mailboxId = destination.mailbox.id,
                    .mailboxName = destination.mailbox.name,
                    .role = destination.mailbox.role,
                    .mailboxPath = row.path,
                    .accountLabel = {},
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

        [[nodiscard]] QString
        baseLabel(const javelin::jmap::cache::CachedAccount& account,
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

        [[nodiscard]] QString uniqueLabel(const javelin::jmap::cache::CachedAccount& account,
                                          const QString& base, const bool baseIsDuplicate,
                                          std::unordered_set<QString>& used)
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

        [[nodiscard]] QString
        fallbackCurrentAccountLabel(const std::string& accountId,
                                    const MessageTransferAccountDisplayName& configuredDisplayName)
        {
            if (configuredDisplayName)
            {
                auto configured =
                    configuredDisplayName(QString::fromStdString(accountId)).trimmed();
                if (!configured.isEmpty())
                    return configured;
            }
            return i18n("Current account");
        }

        void applyAccountLabel(std::vector<MessageTransferDestinationRow>& rows,
                               const QString& label)
        {
            for (auto& row : rows)
                row.accountLabel = label;
        }

        [[nodiscard]] QString normalized(QStringView value)
        {
            return value.toString().simplified().toCaseFolded();
        }

        [[nodiscard]] std::optional<int> fuzzyFieldScore(const QString& token, const QString& field)
        {
            if (token.isEmpty() || field.isEmpty())
                return std::nullopt;
            if (field == token)
                return 1000;
            if (field.startsWith(token))
                return 850 -
                       static_cast<int>(std::min<qsizetype>(field.size() - token.size(), 100));
            const auto containedAt = field.indexOf(token);
            if (containedAt >= 0)
                return 700 - static_cast<int>(std::min<qsizetype>(containedAt * 4, 200));

            qsizetype cursor = 0;
            qsizetype first = -1;
            qsizetype last = -1;
            int boundaryHits = 0;
            for (const auto character : token)
            {
                const auto found = field.indexOf(character, cursor);
                if (found < 0)
                    return std::nullopt;
                if (first < 0)
                    first = found;
                last = found;
                if (found == 0 || field.at(found - 1).isSpace() || field.at(found - 1) == u'/' ||
                    field.at(found - 1) == u'-' || field.at(found - 1) == u'_')
                    ++boundaryHits;
                cursor = found + 1;
            }
            const auto span = last - first + 1;
            const auto gaps = span - token.size();
            return 450 + boundaryHits * 20 - std::min(static_cast<int>(gaps * 8), 260) -
                   std::min(static_cast<int>(first * 2), 120);
        }

        [[nodiscard]] std::optional<int> searchScore(const MessageTransferDestinationRow& row,
                                                     const QStringList& tokens,
                                                     const QString& wholeQuery)
        {
            const auto name = normalized(QString::fromStdString(row.mailboxName));
            const auto path = normalized(row.mailboxPath);
            const auto account = normalized(row.accountLabel);

            int score = 0;
            for (const auto& token : tokens)
            {
                int best = -1;
                if (const auto value = fuzzyFieldScore(token, name))
                    best = std::max(best, *value * 3);
                if (const auto value = fuzzyFieldScore(token, path))
                    best = std::max(best, *value * 2);
                if (const auto value = fuzzyFieldScore(token, account))
                    best = std::max(best, *value);
                if (best < 0)
                    return std::nullopt;
                score += best;
            }

            if (name == wholeQuery)
                score += 5000;
            else if (name.startsWith(wholeQuery))
                score += 3500;
            if (path == wholeQuery)
                score += 3000;
            else if (path.contains(wholeQuery))
                score += 1800;
            if (account == wholeQuery)
                score += 1200;
            return score;
        }
    } // namespace

    MessageTransferDestinationPresentation buildMessageTransferDestinationPresentation(
        std::string currentAccountId,
        const std::vector<javelin::jmap::cache::CachedAccount>& accounts,
        const std::unordered_map<std::string, std::vector<javelin::jmap::cache::MailboxTreeItem>>&
            mailboxesByAccount,
        const MessageTransferAccountDisplayName& configuredDisplayName)
    {
        MessageTransferDestinationPresentation result;
        if (const auto current = mailboxesByAccount.find(currentAccountId);
            current != mailboxesByAccount.end())
        {
            result.currentAccountRows = writableRows(currentAccountId, current->second);
        }

        const auto currentAccount = std::ranges::find(
            accounts, currentAccountId, &javelin::jmap::cache::CachedAccount::accountId);
        const auto currentBaseLabel =
            currentAccount == accounts.end()
                ? fallbackCurrentAccountLabel(currentAccountId, configuredDisplayName)
                : baseLabel(*currentAccount, configuredDisplayName);

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
        if (!result.currentAccountRows.empty())
            ++labelCounts[currentBaseLabel.toCaseFolded()];
        for (const auto& candidate : candidates)
            ++labelCounts[candidate.baseLabel.toCaseFolded()];

        std::unordered_set<QString> usedLabels;
        if (!result.currentAccountRows.empty())
        {
            if (currentAccount != accounts.end())
            {
                result.currentAccountLabel =
                    uniqueLabel(*currentAccount, currentBaseLabel,
                                labelCounts[currentBaseLabel.toCaseFolded()] > 1, usedLabels);
            }
            else
            {
                result.currentAccountLabel = currentBaseLabel;
                usedLabels.insert(currentBaseLabel.toCaseFolded());
            }
            applyAccountLabel(result.currentAccountRows, result.currentAccountLabel);
        }

        for (auto& candidate : candidates)
        {
            const bool duplicate = labelCounts[candidate.baseLabel.toCaseFolded()] > 1;
            auto label =
                uniqueLabel(*candidate.account, candidate.baseLabel, duplicate, usedLabels);
            applyAccountLabel(candidate.rows, label);
            result.otherAccounts.push_back({
                .accountId = candidate.account->accountId,
                .label = std::move(label),
                .rows = std::move(candidate.rows),
            });
        }
        return result;
    }

    std::vector<MessageTransferDestinationSearchResult>
    searchMessageTransferDestinations(const MessageTransferDestinationPresentation& presentation,
                                      const QStringView query)
    {
        const auto wholeQuery = normalized(query);
        if (wholeQuery.isEmpty())
            return {};
        const auto tokens = wholeQuery.split(u' ', Qt::SkipEmptyParts);

        std::vector<MessageTransferDestinationSearchResult> results;
        const auto append =
            [&results, &tokens, &wholeQuery](const std::vector<MessageTransferDestinationRow>& rows)
        {
            for (const auto& row : rows)
            {
                const auto score = searchScore(row, tokens, wholeQuery);
                if (!score.has_value())
                    continue;
                results.push_back({.destination = row, .score = *score});
            }
        };
        append(presentation.currentAccountRows);
        for (const auto& account : presentation.otherAccounts)
            append(account.rows);

        std::ranges::sort(results,
                          [](const auto& left, const auto& right)
                          {
                              if (left.score != right.score)
                                  return left.score > right.score;
                              const auto pathOrder = QString::compare(left.destination.mailboxPath,
                                                                      right.destination.mailboxPath,
                                                                      Qt::CaseInsensitive);
                              if (pathOrder != 0)
                                  return pathOrder < 0;
                              return QString::compare(left.destination.accountLabel,
                                                      right.destination.accountLabel,
                                                      Qt::CaseInsensitive) < 0;
                          });
        return results;
    }

} // namespace javelin::gui::shell
