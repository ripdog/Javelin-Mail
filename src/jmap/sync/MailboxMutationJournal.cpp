#include "jmap/sync/MailboxMutationJournal.h"

#include "jmap/cache/MailboxRepository.h"
#include "jmap/cache/SyncStateRepository.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <array>
#include <utility>

namespace javelin::jmap::sync
{
    namespace
    {
        constexpr std::string_view mutationKind{"mailbox_subscription"};
        constexpr std::string_view dataType{"Mailbox"};

        [[nodiscard]] std::variant<MutationRecord, javelin::jmap::cache::DatabaseError>
        genericRecord(const MailboxSubscriptionMutationRecord& record)
        {
            const QJsonObject payload{
                {QStringLiteral("beforeSubscribed"), record.beforeSubscribed},
                {QStringLiteral("afterSubscribed"), record.afterSubscribed},
            };
            return MutationRecord{
                .mutationId = record.mutationId,
                .operationGroupId = record.operationGroupId,
                .domain = {.accountId = record.accountId, .dataType = std::string{dataType}},
                .objectId = record.mailboxId,
                .mutationKind = std::string{mutationKind},
                .status = record.status,
                .payloadJson = QJsonDocument{payload}.toJson(QJsonDocument::Compact).toStdString(),
                .baseState = record.baseState,
                .acceptedState = record.acceptedState,
                .errorJson = record.errorJson,
            };
        }

        [[nodiscard]] std::variant<MailboxSubscriptionMutationRecord,
                                   javelin::jmap::cache::DatabaseError>
        typedRecord(MutationRecord record)
        {
            const auto document =
                QJsonDocument::fromJson(QByteArray::fromStdString(record.payloadJson));
            if (record.mutationKind != mutationKind || !document.isObject())
            {
                return javelin::jmap::cache::DatabaseError{
                    .code = javelin::jmap::cache::DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("Invalid Mailbox mutation journal payload."),
                };
            }
            const auto object = document.object();
            if (!object.contains(QStringLiteral("beforeSubscribed")) ||
                !object.contains(QStringLiteral("afterSubscribed")))
            {
                return javelin::jmap::cache::DatabaseError{
                    .code = javelin::jmap::cache::DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("Incomplete Mailbox subscription mutation payload."),
                };
            }
            return MailboxSubscriptionMutationRecord{
                .mutationId = std::move(record.mutationId),
                .operationGroupId = std::move(record.operationGroupId),
                .accountId = std::move(record.domain.accountId),
                .mailboxId = std::move(record.objectId),
                .status = record.status,
                .beforeSubscribed = object.value(QStringLiteral("beforeSubscribed")).toBool(),
                .afterSubscribed = object.value(QStringLiteral("afterSubscribed")).toBool(),
                .baseState = std::move(record.baseState),
                .acceptedState = std::move(record.acceptedState),
                .errorJson = std::move(record.errorJson),
            };
        }

        [[nodiscard]] ConsistencyDomain domain(const std::string_view accountId)
        {
            return {.accountId = std::string{accountId}, .dataType = std::string{dataType}};
        }

        [[nodiscard]] javelin::jmap::cache::SyncStateKey stateKey(const std::string_view accountId)
        {
            return {.accountId = std::string{accountId},
                    .objectType = std::string{dataType},
                    .queryKey = {}};
        }
    } // namespace

    MailboxMutationJournal::MailboxMutationJournal(
        javelin::jmap::cache::DatabaseConnection& connection,
        javelin::jmap::cache::MailboxRepository& repository)
        : m_connection(connection), m_repository(repository)
    {
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MailboxMutationJournal::queue(const MailboxSubscriptionMutationRecord& record)
    {
        auto transactionResult = MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Queue Mailbox subscription mutation"));
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<MutationProjectionTransaction>(std::move(transactionResult));
        const auto generic = genericRecord(record);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&generic))
            return *error;
        if (const auto error = transaction.append(std::get<MutationRecord>(generic)))
            return error;
        if (const auto error =
                m_repository.setSubscribed(transaction.cacheTransaction(), record.accountId,
                                           record.mailboxId, record.afterSubscribed))
            return error;
        const std::array domains{domain(record.accountId)};
        if (const auto error = transaction.advance(domains))
            return error;
        return transaction.commit();
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MailboxMutationJournal::transition(const MailboxSubscriptionMutationRecord& record,
                                       const MutationStatus status,
                                       const std::optional<std::string_view> acceptedState,
                                       const std::optional<std::string_view> errorJson)
    {
        MutationJournalRepository journal{m_connection};
        return journal.transition(record.mutationId, status, acceptedState, errorJson);
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MailboxMutationJournal::reject(const MailboxSubscriptionMutationRecord& record,
                                   const std::optional<std::string_view> acceptedState,
                                   const std::optional<std::string_view> errorJson)
    {
        auto transactionResult = MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Reject Mailbox subscription mutation"));
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<MutationProjectionTransaction>(std::move(transactionResult));
        if (const auto error = transaction.transition(record.mutationId, MutationStatus::Rejected,
                                                      acceptedState, errorJson))
            return error;
        if (const auto error =
                m_repository.setSubscribed(transaction.cacheTransaction(), record.accountId,
                                           record.mailboxId, record.beforeSubscribed))
            return error;
        const std::array domains{domain(record.accountId)};
        if (const auto error = transaction.advance(domains))
            return error;
        return transaction.commit();
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MailboxMutationJournal::accept(const MailboxSubscriptionMutationRecord& record,
                                   const std::string_view state)
    {
        auto transactionResult = MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Accept Mailbox subscription mutation"));
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<MutationProjectionTransaction>(std::move(transactionResult));
        if (const auto error =
                transaction.transition(record.mutationId, MutationStatus::Accepted, state))
            return error;
        if (const auto error =
                m_repository.setSubscribed(transaction.cacheTransaction(), record.accountId,
                                           record.mailboxId, record.afterSubscribed))
            return error;
        javelin::jmap::cache::SyncStateRepository states{m_connection};
        const auto expected = record.baseState.has_value()
                                  ? std::optional<std::string_view>{*record.baseState}
                                  : std::nullopt;
        const auto advanced = states.replaceIfCurrent(transaction.cacheTransaction(),
                                                      stateKey(record.accountId), expected, state);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&advanced))
            return *error;
        if (!std::get<bool>(advanced))
        {
            return javelin::jmap::cache::DatabaseError{
                .code = javelin::jmap::cache::DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Mailbox state changed before mutation acceptance."),
            };
        }
        const std::array domains{domain(record.accountId)};
        if (const auto error = transaction.advance(domains))
            return error;
        if (const auto error = transaction.remove(record.mutationId))
            return error;
        return transaction.commit();
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MailboxMutationJournal::reconcile(const MailboxSubscriptionMutationRecord& record,
                                      const bool serverSubscribed, const std::string_view state)
    {
        auto transactionResult = MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Reconcile Mailbox subscription mutation"));
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<MutationProjectionTransaction>(std::move(transactionResult));
        const auto status = serverSubscribed == record.afterSubscribed ? MutationStatus::Accepted
                                                                       : MutationStatus::Rejected;
        if (const auto error = transaction.transition(record.mutationId, status, state))
            return error;
        if (const auto error =
                m_repository.setSubscribed(transaction.cacheTransaction(), record.accountId,
                                           record.mailboxId, serverSubscribed))
            return error;
        javelin::jmap::cache::SyncStateRepository states{m_connection};
        if (const auto error =
                states.upsert(transaction.cacheTransaction(), stateKey(record.accountId), state))
            return error;
        const std::array domains{domain(record.accountId)};
        if (const auto error = transaction.advance(domains))
            return error;
        if (const auto error = transaction.remove(record.mutationId))
            return error;
        return transaction.commit();
    }

    std::variant<std::vector<MailboxSubscriptionMutationRecord>,
                 javelin::jmap::cache::DatabaseError>
    MailboxMutationJournal::listActive(const std::string_view accountId) const
    {
        MutationJournalRepository journal{m_connection};
        auto result = journal.listActive(domain(accountId));
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
            return *error;
        std::vector<MailboxSubscriptionMutationRecord> records;
        for (auto& generic : std::get<std::vector<MutationRecord>>(result))
        {
            if (generic.mutationKind != mutationKind)
                continue;
            auto typed = typedRecord(std::move(generic));
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&typed))
                return *error;
            records.push_back(std::get<MailboxSubscriptionMutationRecord>(std::move(typed)));
        }
        return records;
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MailboxMutationJournal::rebase(javelin::jmap::cache::DatabaseTransaction& transaction,
                                   const std::string_view accountId) const
    {
        const auto active = listActive(accountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&active))
            return *error;
        for (const auto& record : std::get<std::vector<MailboxSubscriptionMutationRecord>>(active))
        {
            if (!projectsOptimistically(record.status))
                continue;
            const auto mailbox = m_repository.find(record.accountId, record.mailboxId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&mailbox))
                return *error;
            if (!std::get<std::optional<javelin::jmap::domain::Mailbox>>(mailbox).has_value())
                continue;
            if (const auto error = m_repository.setSubscribed(
                    transaction, record.accountId, record.mailboxId, record.afterSubscribed))
                return error;
        }
        return std::nullopt;
    }
} // namespace javelin::jmap::sync
