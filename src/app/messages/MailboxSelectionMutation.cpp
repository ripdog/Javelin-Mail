#include "app/MailboxSelectionMutation.h"

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
        findMailboxById(const std::unordered_map<std::string_view, const Mailbox*>& mailboxes,
                        const std::string_view mailboxId)
        {
            const auto found = mailboxes.find(mailboxId);
            return found == mailboxes.end() ? nullptr : found->second;
        }

        [[nodiscard]] const Mailbox* findMailboxByRole(const std::vector<Mailbox>& mailboxes,
                                                       const std::string_view role)
        {
            const auto found =
                std::ranges::find_if(mailboxes, [role](const Mailbox& mailbox)
                                     { return mailbox.role == std::optional<std::string>{role}; });
            return found == mailboxes.end() ? nullptr : &*found;
        }

        [[nodiscard]] QString mailboxUnavailable(const std::string_view mailboxId)
        {
            return i18n("Mailbox %1 is not available in the local account state.",
                        QString::fromStdString(std::string{mailboxId}));
        }

        [[nodiscard]] QString cannotAddTo(const Mailbox& mailbox)
        {
            return i18n("You do not have permission to add messages to %1.",
                        QString::fromStdString(mailbox.name));
        }

        [[nodiscard]] QString cannotRemoveFrom(const Mailbox& mailbox)
        {
            return i18n("You do not have permission to remove messages from %1.",
                        QString::fromStdString(mailbox.name));
        }

        [[nodiscard]] QString cannotSetKeywordsIn(const Mailbox& mailbox)
        {
            return i18n("You do not have permission to change message keywords in %1.",
                        QString::fromStdString(mailbox.name));
        }
    } // namespace

    MailboxSelectionMutationPlanResult planMailboxSelectionMutation(
        const MailboxSelectionMutationIntent& intent, const std::vector<std::string>& emailIds,
        const std::vector<javelin::jmap::domain::Email>& emails,
        const std::vector<javelin::jmap::cache::MailboxTreeItem>& mailboxes)
    {
        std::unordered_map<std::string_view, const Mailbox*> mailboxesById;
        mailboxesById.reserve(mailboxes.size());
        for (const auto& mailbox : mailboxes)
        {
            mailboxesById.emplace(mailbox.id, &mailbox);
        }

        const Mailbox* destination = nullptr;
        const Mailbox* searchArchiveSource = nullptr;
        const Mailbox* junkMailbox = findMailboxByRole(mailboxes, "junk");
        switch (intent.operation)
        {
        case MailboxSelectionOperation::Archive:
            destination = findMailboxByRole(mailboxes, "archive");
            if (destination == nullptr)
            {
                return i18n("No Archive mailbox is available.");
            }
            if (!intent.sourceMailboxId.has_value())
            {
                searchArchiveSource = findMailboxByRole(mailboxes, "inbox");
                if (searchArchiveSource == nullptr)
                {
                    return i18n("No Inbox mailbox is available.");
                }
            }
            break;
        case MailboxSelectionOperation::Junk:
            destination = junkMailbox;
            if (destination == nullptr)
            {
                return i18n("No Junk mailbox is available.");
            }
            break;
        case MailboxSelectionOperation::NotJunk:
            destination = findMailboxByRole(mailboxes, "inbox");
            if (destination == nullptr)
            {
                return i18n("No Inbox mailbox is available.");
            }
            break;
        case MailboxSelectionOperation::Move:
        case MailboxSelectionOperation::Copy:
            if (!intent.destinationMailboxId.has_value())
            {
                return i18n("A destination mailbox is required.");
            }
            destination = findMailboxById(mailboxesById, *intent.destinationMailboxId);
            if (destination == nullptr)
            {
                return mailboxUnavailable(*intent.destinationMailboxId);
            }
            break;
        default:
            return i18n("The requested mailbox operation is not supported.");
        }

        std::unordered_map<std::string_view, const javelin::jmap::domain::Email*> emailsById;
        emailsById.reserve(emails.size());
        for (const auto& email : emails)
        {
            emailsById.emplace(email.id, &email);
        }

        PlannedMailboxSelectionMutation plan;
        plan.mutations.reserve(emailIds.size());
        std::unordered_set<std::string_view> seenEmailIds;
        for (const auto& emailId : emailIds)
        {
            if (!seenEmailIds.insert(emailId).second)
            {
                continue;
            }

            const auto foundEmail = emailsById.find(emailId);
            if (foundEmail == emailsById.end())
            {
                return i18n("Message %1 is not available in the local cache.",
                            QString::fromStdString(emailId));
            }
            const auto& email = *foundEmail->second;
            const bool alreadyInDestination =
                std::ranges::find(email.mailboxIds, destination->id) != email.mailboxIds.end();

            std::vector<std::string> removeMailboxIds;
            if (intent.operation == MailboxSelectionOperation::Move ||
                intent.operation == MailboxSelectionOperation::Junk)
            {
                if (intent.sourceMailboxId.has_value())
                {
                    if (std::ranges::find(email.mailboxIds, *intent.sourceMailboxId) ==
                        email.mailboxIds.end())
                    {
                        return i18n("Message %1 is no longer in the source mailbox.",
                                    QString::fromStdString(emailId));
                    }
                    if (*intent.sourceMailboxId != destination->id)
                    {
                        removeMailboxIds.push_back(*intent.sourceMailboxId);
                    }
                }
                else
                {
                    for (const auto& mailboxId : email.mailboxIds)
                    {
                        if (mailboxId != destination->id)
                        {
                            removeMailboxIds.push_back(mailboxId);
                        }
                    }
                }
            }
            else if (intent.operation == MailboxSelectionOperation::Archive)
            {
                const auto& sourceMailboxId = intent.sourceMailboxId.has_value()
                                                  ? *intent.sourceMailboxId
                                                  : searchArchiveSource->id;
                if (sourceMailboxId == destination->id)
                {
                    ++plan.skippedEmailCount;
                    continue;
                }
                if (std::ranges::find(email.mailboxIds, sourceMailboxId) == email.mailboxIds.end())
                {
                    ++plan.skippedEmailCount;
                    continue;
                }
                removeMailboxIds.push_back(sourceMailboxId);
            }
            else if (intent.operation == MailboxSelectionOperation::NotJunk &&
                     junkMailbox != nullptr && junkMailbox->id != destination->id &&
                     std::ranges::contains(email.mailboxIds, junkMailbox->id))
            {
                removeMailboxIds.push_back(junkMailbox->id);
            }

            std::vector<std::string> addKeywords;
            std::vector<std::string> removeKeywords;
            if (intent.operation == MailboxSelectionOperation::Junk)
            {
                if (!std::ranges::contains(email.keywords, std::string{"$junk"}))
                {
                    addKeywords.push_back("$junk");
                }
                if (std::ranges::contains(email.keywords, std::string{"$notjunk"}))
                {
                    removeKeywords.push_back("$notjunk");
                }
            }
            else if (intent.operation == MailboxSelectionOperation::NotJunk)
            {
                if (!std::ranges::contains(email.keywords, std::string{"$notjunk"}))
                {
                    addKeywords.push_back("$notjunk");
                }
                if (std::ranges::contains(email.keywords, std::string{"$junk"}))
                {
                    removeKeywords.push_back("$junk");
                }
            }

            const bool changesKeywords = !addKeywords.empty() || !removeKeywords.empty();
            if (changesKeywords)
            {
                for (const auto& mailboxId : email.mailboxIds)
                {
                    const auto* mailbox = findMailboxById(mailboxesById, mailboxId);
                    if (mailbox == nullptr)
                    {
                        return mailboxUnavailable(mailboxId);
                    }
                    if (!mailbox->myRights.maySetKeywords)
                    {
                        return cannotSetKeywordsIn(*mailbox);
                    }
                }
            }

            const bool needsDestination = !alreadyInDestination;
            if (needsDestination && !destination->myRights.mayAddItems)
            {
                return cannotAddTo(*destination);
            }
            for (const auto& mailboxId : removeMailboxIds)
            {
                const auto* source = findMailboxById(mailboxesById, mailboxId);
                if (source == nullptr)
                {
                    return mailboxUnavailable(mailboxId);
                }
                if (!source->myRights.mayRemoveItems)
                {
                    return cannotRemoveFrom(*source);
                }
            }

            if (!needsDestination && removeMailboxIds.empty() && !changesKeywords)
            {
                ++plan.skippedEmailCount;
                continue;
            }

            if (changesKeywords && needsDestination && !destination->myRights.maySetKeywords)
            {
                return cannotSetKeywordsIn(*destination);
            }

            plan.mutations.push_back(javelin::jmap::EmailMailboxMutation{
                .emailId = email.id,
                .addMailboxIds = needsDestination ? std::vector<std::string>{destination->id}
                                                  : std::vector<std::string>{},
                .removeMailboxIds = std::move(removeMailboxIds),
                .addKeywords = std::move(addKeywords),
                .removeKeywords = std::move(removeKeywords),
                .operationGroupId = std::nullopt,
                .ifInState = std::nullopt,
                .authoritativeMailboxIds = std::nullopt,
                .authoritativeKeywords = std::nullopt,
            });
        }

        return plan;
    }

} // namespace javelin::app
