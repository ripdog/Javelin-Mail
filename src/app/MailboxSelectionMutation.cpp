#include "app/MailboxSelectionMutation.h"

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
            return QStringLiteral("Mailbox %1 is not available in the local account state.")
                .arg(QString::fromStdString(std::string{mailboxId}));
        }

        [[nodiscard]] QString cannotAddTo(const Mailbox& mailbox)
        {
            return QStringLiteral("You do not have permission to add messages to %1.")
                .arg(QString::fromStdString(mailbox.name));
        }

        [[nodiscard]] QString cannotRemoveFrom(const Mailbox& mailbox)
        {
            return QStringLiteral("You do not have permission to remove messages from %1.")
                .arg(QString::fromStdString(mailbox.name));
        }
    } // namespace

    MailboxSelectionMutationPlanResult planMailboxSelectionMutation(
        const MailboxSelectionMutationIntent& intent,
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
        if (intent.operation == MailboxSelectionOperation::Archive)
        {
            destination = findMailboxByRole(mailboxes, "archive");
            if (destination == nullptr)
            {
                return QStringLiteral("No Archive mailbox is available.");
            }
            if (!intent.sourceMailboxId.has_value())
            {
                searchArchiveSource = findMailboxByRole(mailboxes, "inbox");
                if (searchArchiveSource == nullptr)
                {
                    return QStringLiteral("No Inbox mailbox is available.");
                }
            }
        }
        else
        {
            if (!intent.destinationMailboxId.has_value())
            {
                return QStringLiteral("A destination mailbox is required.");
            }
            destination = findMailboxById(mailboxesById, *intent.destinationMailboxId);
            if (destination == nullptr)
            {
                return mailboxUnavailable(*intent.destinationMailboxId);
            }
        }

        std::unordered_map<std::string_view, const javelin::jmap::domain::Email*> emailsById;
        emailsById.reserve(emails.size());
        for (const auto& email : emails)
        {
            emailsById.emplace(email.id, &email);
        }

        PlannedMailboxSelectionMutation plan;
        plan.mutations.reserve(intent.emailIds.size());
        std::unordered_set<std::string_view> seenEmailIds;
        for (const auto& emailId : intent.emailIds)
        {
            if (!seenEmailIds.insert(emailId).second)
            {
                continue;
            }

            const auto foundEmail = emailsById.find(emailId);
            if (foundEmail == emailsById.end())
            {
                return QStringLiteral("Message %1 is not available in the local cache.")
                    .arg(QString::fromStdString(emailId));
            }
            const auto& email = *foundEmail->second;
            const bool alreadyInDestination =
                std::ranges::find(email.mailboxIds, destination->id) != email.mailboxIds.end();

            std::vector<std::string> removeMailboxIds;
            if (intent.operation == MailboxSelectionOperation::Move)
            {
                if (intent.sourceMailboxId.has_value())
                {
                    if (std::ranges::find(email.mailboxIds, *intent.sourceMailboxId) ==
                        email.mailboxIds.end())
                    {
                        return QStringLiteral("Message %1 is no longer in the source mailbox.")
                            .arg(QString::fromStdString(emailId));
                    }
                    removeMailboxIds.push_back(*intent.sourceMailboxId);
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

            if (!needsDestination && removeMailboxIds.empty())
            {
                ++plan.skippedEmailCount;
                continue;
            }

            plan.mutations.push_back(javelin::jmap::EmailMailboxMutation{
                .emailId = email.id,
                .addMailboxIds = needsDestination ? std::vector<std::string>{destination->id}
                                                  : std::vector<std::string>{},
                .removeMailboxIds = std::move(removeMailboxIds),
            });
        }

        return plan;
    }

} // namespace javelin::app
