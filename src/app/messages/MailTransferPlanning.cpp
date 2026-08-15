#include "app/MailTransferPlanning.h"

#include <KLocalizedString>

#include <algorithm>
#include <ranges>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace javelin::app
{
    namespace
    {
        using Mailbox = javelin::jmap::cache::MailboxTreeItem;

        [[nodiscard]] const Mailbox*
        findMailbox(const std::unordered_map<std::string_view, const Mailbox*>& mailboxes,
                    const std::string_view mailboxId)
        {
            const auto found = mailboxes.find(mailboxId);
            return found == mailboxes.end() ? nullptr : found->second;
        }

        [[nodiscard]] QString mailboxUnavailable(const std::string_view mailboxId)
        {
            return i18n("Mailbox %1 is not available in the local account state.",
                        QString::fromStdString(std::string{mailboxId}));
        }

        [[nodiscard]] QString cannotRemoveFrom(const Mailbox& mailbox)
        {
            return i18n("You do not have permission to remove messages from %1.",
                        QString::fromStdString(mailbox.name));
        }

        [[nodiscard]] QString cannotPreserveKeywords(const Mailbox& mailbox)
        {
            return i18n("You do not have permission to preserve all message flags and tags in %1.",
                        QString::fromStdString(mailbox.name));
        }

        [[nodiscard]] std::unordered_map<std::string_view, const Mailbox*>
        indexMailboxes(const std::vector<Mailbox>& mailboxes)
        {
            std::unordered_map<std::string_view, const Mailbox*> result;
            result.reserve(mailboxes.size());
            for (const auto& mailbox : mailboxes)
                result.emplace(mailbox.id, &mailbox);
            return result;
        }
    } // namespace

    MailTransferPlanResult
    planMailTransfer(const MailTransferIntent& intent, const std::vector<std::string>& emailIds,
                     const std::vector<javelin::jmap::domain::Email>& emails,
                     const std::vector<javelin::jmap::cache::MailboxTreeItem>& sourceMailboxes,
                     const std::vector<javelin::jmap::cache::MailboxTreeItem>& destinationMailboxes,
                     const javelin::jmap::cache::CachedAccount& sourceAccount,
                     const javelin::jmap::cache::CachedAccount& destinationAccount,
                     const std::span<const MailTransferSourceCleanupOverride> cleanupOverrides)
    {
        if (intent.sourceAccountId.empty() || intent.destinationAccountId.empty() ||
            intent.destinationMailboxId.empty())
            return i18n("The mail transfer is missing an account or destination mailbox.");
        if (intent.sourceAccountId == intent.destinationAccountId)
            return i18n("Transfers within one account must use the normal mailbox move/copy path.");
        if (sourceAccount.accountId != intent.sourceAccountId ||
            destinationAccount.accountId != intent.destinationAccountId)
            return i18n("The mail transfer account state is inconsistent.");
        if (!sourceAccount.hasMailCapability || !destinationAccount.hasMailCapability)
            return i18n("Both accounts must support JMAP Mail.");
        if (sourceAccount.connectionId.empty() || sourceAccount.remoteAccountId.empty() ||
            destinationAccount.connectionId.empty() || destinationAccount.remoteAccountId.empty())
            return i18n("One of the mail accounts does not have a complete server identity.");
        if (destinationAccount.isReadOnly)
            return i18n("The destination account is read-only.");

        const auto sourceById = indexMailboxes(sourceMailboxes);
        const auto destinationById = indexMailboxes(destinationMailboxes);
        std::unordered_map<std::string_view, const MailTransferSourceCleanupOverride*>
            cleanupByEmail;
        cleanupByEmail.reserve(cleanupOverrides.size());
        for (const auto& cleanup : cleanupOverrides)
        {
            if (cleanup.emailId.empty() ||
                !cleanupByEmail.emplace(cleanup.emailId, &cleanup).second)
                return i18n("The exact source cleanup plan is invalid.");
        }
        if (!cleanupOverrides.empty() && intent.operation != MailTransferOperation::Move)
            return i18n("Exact source cleanup is only valid for Move operations.");
        const auto* destination = findMailbox(destinationById, intent.destinationMailboxId);
        if (destination == nullptr)
            return mailboxUnavailable(intent.destinationMailboxId);
        if (!destination->myRights.mayAddItems)
        {
            return i18n("You do not have permission to add messages to %1.",
                        QString::fromStdString(destination->name));
        }

        std::unordered_map<std::string_view, const javelin::jmap::domain::Email*> emailsById;
        emailsById.reserve(emails.size());
        for (const auto& email : emails)
            emailsById.emplace(email.id, &email);

        PlannedMailTransfer plan{
            .topology = sourceAccount.connectionId == destinationAccount.connectionId
                            ? MailTransferTopology::SameSessionCopy
                            : MailTransferTopology::CrossServerImport,
            .items = {},
        };
        plan.items.reserve(emailIds.size());
        std::unordered_set<std::string_view> seen;
        for (const auto& emailId : emailIds)
        {
            if (!seen.insert(emailId).second)
                continue;
            const auto found = emailsById.find(emailId);
            if (found == emailsById.end())
            {
                return i18n("Message %1 is not available in the local cache.",
                            QString::fromStdString(emailId));
            }
            const auto& email = *found->second;
            if (email.blobId.empty())
            {
                return i18n("Message %1 does not have a downloadable source blob.",
                            QString::fromStdString(email.id));
            }
            if (email.mailboxIds.empty())
            {
                return i18n("Message %1 has no mailbox membership in the local cache.",
                            QString::fromStdString(email.id));
            }

            const bool hasSeen = std::ranges::contains(email.keywords, std::string{"$seen"});
            const bool hasOtherKeywords = std::ranges::any_of(
                email.keywords, [](const std::string& keyword) { return keyword != "$seen"; });
            if ((hasSeen && !destination->myRights.maySetSeen) ||
                (hasOtherKeywords && !destination->myRights.maySetKeywords))
                return cannotPreserveKeywords(*destination);

            std::vector<std::string> removeMailboxIds;
            if (intent.operation == MailTransferOperation::Move)
            {
                if (!cleanupOverrides.empty())
                {
                    const auto cleanup = cleanupByEmail.find(email.id);
                    if (cleanup == cleanupByEmail.end())
                        return i18n("The exact source cleanup plan does not cover every message.");
                    removeMailboxIds = cleanup->second->removeMailboxIds;
                    std::ranges::sort(removeMailboxIds);
                    removeMailboxIds.erase(
                        std::unique(removeMailboxIds.begin(), removeMailboxIds.end()),
                        removeMailboxIds.end());
                    if (removeMailboxIds.empty() ||
                        std::ranges::any_of(
                            removeMailboxIds, [&](const auto& mailboxId)
                            { return !std::ranges::contains(email.mailboxIds, mailboxId); }))
                        return i18n("A message no longer has the mailbox memberships required by "
                                    "the exact source cleanup plan.");
                }
                else if (intent.sourceMailboxId.has_value() &&
                         std::ranges::contains(email.mailboxIds, *intent.sourceMailboxId))
                {
                    removeMailboxIds.push_back(*intent.sourceMailboxId);
                }
                else
                {
                    removeMailboxIds = email.mailboxIds;
                }

                for (const auto& mailboxId : removeMailboxIds)
                {
                    const auto* source = findMailbox(sourceById, mailboxId);
                    if (source == nullptr)
                        return mailboxUnavailable(mailboxId);
                    if (!source->myRights.mayRemoveItems)
                        return cannotRemoveFrom(*source);
                }
            }

            const bool destroysSource =
                intent.operation == MailTransferOperation::Move &&
                removeMailboxIds.size() == email.mailboxIds.size() &&
                std::ranges::all_of(email.mailboxIds, [&](const auto& mailboxId)
                                    { return std::ranges::contains(removeMailboxIds, mailboxId); });
            plan.items.push_back({
                .sourceEmailId = email.id,
                .sourceBlobId = email.blobId,
                .sourceMailboxIds = email.mailboxIds,
                .sourceKeywords = email.keywords,
                .sourceMessageIds = email.messageId,
                .sourceReceivedAt = email.receivedAt,
                .sourceSize = email.size,
                .sourceRemoveMailboxIds = std::move(removeMailboxIds),
                .sourceDestroy = destroysSource,
            });
        }

        if (plan.items.empty())
            return i18n("No messages were selected for transfer.");
        if (!cleanupOverrides.empty() && cleanupByEmail.size() != plan.items.size())
            return i18n("The exact source cleanup plan contains an unrelated message.");
        return plan;
    }

} // namespace javelin::app
