#include "jmap/sync/EmailMutationQueue.h"

#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/RawMessageSourceRepository.h"
#include "jmap/sync/EmailMutationJournal.h"

#include <QDebug>
#include <QUuid>

#include <algorithm>
#include <ranges>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace javelin::jmap::sync
{
    namespace
    {
        struct EmailMutationBase
        {
            std::vector<std::string> mailboxIds;
            std::vector<std::string> keywords;
        };

        struct PreparedEmailMutation
        {
            EmailMutationRecord record;
            QueuedEmailMutation queued;
        };

        using PreparedEmailMutationResult = std::variant<PreparedEmailMutation, OperationError>;

        [[nodiscard]] std::variant<EmailMutationBase, cache::DatabaseError>
        mutationBase(EmailMutationJournal& journal, const std::string_view accountId,
                     const domain::Email& email)
        {
            const auto recordsResult = journal.listForEmail(accountId, email.id);
            if (const auto* error = std::get_if<cache::DatabaseError>(&recordsResult))
                return *error;
            for (const auto& record : std::get<std::vector<EmailMutationRecord>>(recordsResult))
            {
                if (projectsOptimistically(record.status) && record.baseMailboxIds.has_value() &&
                    record.baseKeywords.has_value())
                {
                    return EmailMutationBase{
                        .mailboxIds = *record.baseMailboxIds,
                        .keywords = *record.baseKeywords,
                    };
                }
            }
            return EmailMutationBase{.mailboxIds = email.mailboxIds, .keywords = email.keywords};
        }

        [[nodiscard]] std::optional<OperationError>
        validateMutation(const EmailMailboxMutation& mutation)
        {
            if (mutation.emailId.empty())
                return OperationError{
                    .message = QStringLiteral("An email id is required for an email patch."),
                };
            if (mutation.destroy &&
                (!mutation.addMailboxIds.empty() || !mutation.removeMailboxIds.empty() ||
                 !mutation.addKeywords.empty() || !mutation.removeKeywords.empty()))
                return OperationError{
                    .message = QStringLiteral("A permanent deletion cannot include other patches."),
                };
            if (!mutation.destroy && mutation.addMailboxIds.empty() &&
                mutation.removeMailboxIds.empty() && mutation.addKeywords.empty() &&
                mutation.removeKeywords.empty())
                return OperationError{
                    .message = QStringLiteral("An email mutation must change a property."),
                };
            for (const auto& mailboxId : mutation.addMailboxIds)
            {
                if (mailboxId.empty() ||
                    std::ranges::contains(mutation.removeMailboxIds, mailboxId))
                {
                    return OperationError{
                        .message = QStringLiteral(
                            "Email mailbox additions and removals must be non-empty and disjoint."),
                    };
                }
            }
            if (std::ranges::any_of(mutation.removeMailboxIds,
                                    [](const auto& mailboxId) { return mailboxId.empty(); }))
                return OperationError{
                    .message = QStringLiteral("Email mailbox removals must be non-empty."),
                };
            for (const auto& keyword : mutation.addKeywords)
            {
                if (keyword.empty() || std::ranges::contains(mutation.removeKeywords, keyword))
                {
                    return OperationError{
                        .message = QStringLiteral(
                            "Email keyword additions and removals must be non-empty and disjoint."),
                    };
                }
            }
            if (std::ranges::any_of(mutation.removeKeywords,
                                    [](const auto& keyword) { return keyword.empty(); }))
                return OperationError{
                    .message = QStringLiteral("Email keyword removals must be non-empty."),
                };
            if (mutation.authoritativeMailboxIds.has_value() !=
                mutation.authoritativeKeywords.has_value())
            {
                return OperationError{
                    .message = QStringLiteral("An authoritative email mutation base must include "
                                              "mailboxes and keywords."),
                };
            }
            return std::nullopt;
        }

        [[nodiscard]] PreparedEmailMutationResult
        prepareMutation(cache::DatabaseConnection& connection, const std::string& accountId,
                        EmailMailboxMutation mutation,
                        std::unordered_map<std::string, domain::Email>& effectiveEmails,
                        std::unordered_map<std::string, EmailMutationBase>& mutationBases,
                        std::vector<std::string>& emailOrder,
                        std::unordered_set<std::string>& uncachedEmailIds)
        {
            if (const auto error = validateMutation(mutation))
                return *error;

            auto effective = effectiveEmails.find(mutation.emailId);
            if (effective == effectiveEmails.end())
            {
                cache::EmailRepository emails{connection};
                const auto found = emails.find(accountId, mutation.emailId);
                if (const auto* error = std::get_if<cache::DatabaseError>(&found))
                    return operationError(*error);
                const auto& email = std::get<std::optional<domain::Email>>(found);
                domain::Email initial;
                if (email.has_value())
                {
                    initial = *email;
                }
                else if (mutation.authoritativeMailboxIds.has_value())
                {
                    initial.id = mutation.emailId;
                    initial.mailboxIds = *mutation.authoritativeMailboxIds;
                    initial.keywords = *mutation.authoritativeKeywords;
                    uncachedEmailIds.insert(mutation.emailId);
                }
                else
                {
                    return OperationError{
                        .message = QStringLiteral("The selected message is not cached locally."),
                    };
                }
                if (mutation.authoritativeMailboxIds.has_value())
                {
                    initial.mailboxIds = *mutation.authoritativeMailboxIds;
                    initial.keywords = *mutation.authoritativeKeywords;
                }
                effective = effectiveEmails.emplace(mutation.emailId, std::move(initial)).first;
                emailOrder.push_back(mutation.emailId);
            }
            else if (mutation.authoritativeMailboxIds.has_value())
            {
                return OperationError{
                    .message = QStringLiteral(
                        "Only the first mutation for an email may provide an authoritative base."),
                };
            }

            auto base = mutationBases.find(mutation.emailId);
            if (base == mutationBases.end())
            {
                if (mutation.authoritativeMailboxIds.has_value())
                {
                    base = mutationBases
                               .emplace(mutation.emailId,
                                        EmailMutationBase{
                                            .mailboxIds = *mutation.authoritativeMailboxIds,
                                            .keywords = *mutation.authoritativeKeywords,
                                        })
                               .first;
                }
                else
                {
                    EmailMutationJournal journal{connection};
                    const auto result = mutationBase(journal, accountId, effective->second);
                    if (const auto* error = std::get_if<cache::DatabaseError>(&result))
                        return operationError(*error);
                    base =
                        mutationBases.emplace(mutation.emailId, std::get<EmailMutationBase>(result))
                            .first;
                }
            }

            if (mutation.destroy)
                mutation.removeMailboxIds = effective->second.mailboxIds;
            auto resultingMailboxIds = effective->second.mailboxIds;
            std::erase_if(resultingMailboxIds, [&](const auto& mailboxId)
                          { return std::ranges::contains(mutation.removeMailboxIds, mailboxId); });
            resultingMailboxIds.insert(resultingMailboxIds.end(), mutation.addMailboxIds.begin(),
                                       mutation.addMailboxIds.end());
            if (!mutation.destroy && resultingMailboxIds.empty())
                return OperationError{
                    .message = QStringLiteral("An email must remain in at least one mailbox."),
                };

            const auto mutationId =
                QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
            EmailMutationRecord record{
                .mutationId = mutationId,
                .operationGroupId = mutation.operationGroupId,
                .accountId = accountId,
                .status = MutationStatus::Pending,
                .patch =
                    {
                        .emailId = mutation.emailId,
                        .addMailboxIds = mutation.addMailboxIds,
                        .removeMailboxIds = mutation.removeMailboxIds,
                        .addKeywords = mutation.addKeywords,
                        .removeKeywords = mutation.removeKeywords,
                        .destroy = mutation.destroy,
                    },
                .baseMailboxIds = base->second.mailboxIds,
                .baseKeywords = base->second.keywords,
                .baseState = mutation.ifInState,
                .acceptedState = std::nullopt,
                .errorJson = std::nullopt,
            };
            effective->second = projectEmailMutations(effective->second, {record});
            return PreparedEmailMutation{
                .record = std::move(record),
                .queued =
                    {
                        .mutationId = mutationId,
                        .accountId = accountId,
                        .emailId = mutation.emailId,
                        .patch = std::move(mutation),
                    },
            };
        }
    } // namespace

    QueuedEmailMutationsResult queueEmailMutations(cache::DatabaseConnection& connection,
                                                   std::string accountId,
                                                   std::vector<EmailMailboxMutation> mutations)
    {
        std::unordered_map<std::string, domain::Email> effectiveEmails;
        std::unordered_map<std::string, EmailMutationBase> mutationBases;
        std::vector<std::string> emailOrder;
        std::unordered_set<std::string> uncachedEmailIds;
        std::vector<EmailMutationRecord> records;
        std::vector<QueuedEmailMutation> queued;
        records.reserve(mutations.size());
        queued.reserve(mutations.size());
        for (auto& mutation : mutations)
        {
            auto prepared =
                prepareMutation(connection, accountId, std::move(mutation), effectiveEmails,
                                mutationBases, emailOrder, uncachedEmailIds);
            if (const auto* error = std::get_if<OperationError>(&prepared))
                return *error;
            auto value = std::get<PreparedEmailMutation>(std::move(prepared));
            records.push_back(std::move(value.record));
            queued.push_back(std::move(value.queued));
        }

        std::vector<domain::Email> projections;
        projections.reserve(emailOrder.size());
        for (const auto& emailId : emailOrder)
        {
            if (!uncachedEmailIds.contains(emailId))
                projections.push_back(std::move(effectiveEmails.at(emailId)));
        }
        EmailMutationJournal journal{connection};
        if (const auto error = journal.queueGroup(records, projections))
            return operationError(*error);

        cache::RawMessageSourceRepository sources{connection};
        if (const auto projectionError = sources.replayProjectionJobs())
            qWarning().noquote() << "Mail vault mailbox projection deferred"
                                 << projectionError->message;
        return queued;
    }

    QueuedEmailMutationResult queueEmailMutation(cache::DatabaseConnection& connection,
                                                 std::string accountId,
                                                 EmailMailboxMutation mutation)
    {
        auto result = queueEmailMutations(connection, std::move(accountId), {std::move(mutation)});
        if (const auto* error = std::get_if<OperationError>(&result))
            return *error;
        auto queued = std::get<std::vector<QueuedEmailMutation>>(std::move(result));
        return std::move(queued.front());
    }

    QueuedEmailMutationResult queueMailboxEmailMutation(cache::DatabaseConnection& connection,
                                                        std::string accountId, std::string emailId,
                                                        std::string sourceMailboxId,
                                                        std::string destinationMailboxId,
                                                        const bool removeSourceMailbox)
    {
        if (sourceMailboxId.empty() || destinationMailboxId.empty())
            return OperationError{
                .message = QStringLiteral("Source and destination mailbox ids are required."),
            };
        if (sourceMailboxId == destinationMailboxId)
            return OperationError{
                .message = QStringLiteral("Source and destination mailboxes must be different."),
            };
        return queueEmailMutation(
            connection, std::move(accountId),
            EmailMailboxMutation{
                .emailId = std::move(emailId),
                .addMailboxIds = {std::move(destinationMailboxId)},
                .removeMailboxIds = removeSourceMailbox
                                        ? std::vector<std::string>{std::move(sourceMailboxId)}
                                        : std::vector<std::string>{},
            });
    }

    QueuedEmailMutationResult queueDestroyEmailMutation(cache::DatabaseConnection& connection,
                                                        std::string accountId, std::string emailId,
                                                        std::optional<std::string> operationGroupId)
    {
        return queueEmailMutation(connection, std::move(accountId),
                                  EmailMailboxMutation{
                                      .emailId = std::move(emailId),
                                      .operationGroupId = std::move(operationGroupId),
                                      .destroy = true,
                                  });
    }

    QueuedEmailMutationResult queueEmailKeywordMutation(cache::DatabaseConnection& connection,
                                                        std::string accountId, std::string emailId,
                                                        std::string keyword, const bool enabled)
    {
        return queueEmailMutation(connection, std::move(accountId),
                                  EmailMailboxMutation{
                                      .emailId = std::move(emailId),
                                      .addKeywords = enabled ? std::vector<std::string>{keyword}
                                                             : std::vector<std::string>{},
                                      .removeKeywords = enabled ? std::vector<std::string>{}
                                                                : std::vector<std::string>{keyword},
                                  });
    }

} // namespace javelin::jmap::sync
