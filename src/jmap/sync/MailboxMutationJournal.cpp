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
        constexpr std::string_view subscriptionMutationKind{"mailbox_subscription"};
        constexpr std::string_view destroyMutationKind{"mailbox_destroy"};
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
                .mutationKind = std::string{subscriptionMutationKind},
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
            if (record.mutationKind != subscriptionMutationKind || !document.isObject())
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

        [[nodiscard]] QJsonObject mailboxJson(const javelin::jmap::domain::Mailbox& mailbox)
        {
            const auto& rights = mailbox.myRights;
            return {
                {QStringLiteral("id"), QString::fromStdString(mailbox.id)},
                {QStringLiteral("name"), QString::fromStdString(mailbox.name)},
                {QStringLiteral("parentId"),
                 mailbox.parentId.has_value()
                     ? QJsonValue{QString::fromStdString(*mailbox.parentId)}
                     : QJsonValue{QJsonValue::Null}},
                {QStringLiteral("role"), mailbox.role.has_value()
                                             ? QJsonValue{QString::fromStdString(*mailbox.role)}
                                             : QJsonValue{QJsonValue::Null}},
                {QStringLiteral("sortOrder"), QString::number(mailbox.sortOrder)},
                {QStringLiteral("totalEmails"), QString::number(mailbox.totalEmails)},
                {QStringLiteral("unreadEmails"), QString::number(mailbox.unreadEmails)},
                {QStringLiteral("totalThreads"), QString::number(mailbox.totalThreads)},
                {QStringLiteral("unreadThreads"), QString::number(mailbox.unreadThreads)},
                {QStringLiteral("isSubscribed"), mailbox.isSubscribed},
                {QStringLiteral("myRights"),
                 QJsonObject{{QStringLiteral("mayReadItems"), rights.mayReadItems},
                             {QStringLiteral("mayAddItems"), rights.mayAddItems},
                             {QStringLiteral("mayRemoveItems"), rights.mayRemoveItems},
                             {QStringLiteral("maySetSeen"), rights.maySetSeen},
                             {QStringLiteral("maySetKeywords"), rights.maySetKeywords},
                             {QStringLiteral("mayCreateChild"), rights.mayCreateChild},
                             {QStringLiteral("mayRename"), rights.mayRename},
                             {QStringLiteral("mayDelete"), rights.mayDelete},
                             {QStringLiteral("maySubmit"), rights.maySubmit}}},
            };
        }

        [[nodiscard]] std::optional<javelin::jmap::domain::Mailbox>
        mailboxFromJson(const QJsonObject& object)
        {
            const auto id = object.value(QStringLiteral("id"));
            const auto name = object.value(QStringLiteral("name"));
            const auto rightsValue = object.value(QStringLiteral("myRights"));
            if (!id.isString() || !name.isString() || !rightsValue.isObject())
                return std::nullopt;
            const auto rights = rightsValue.toObject();
            return javelin::jmap::domain::Mailbox{
                .id = id.toString().toStdString(),
                .name = name.toString().toStdString(),
                .parentId =
                    object.value(QStringLiteral("parentId")).isString()
                        ? std::optional<std::string>{object.value(QStringLiteral("parentId"))
                                                         .toString()
                                                         .toStdString()}
                        : std::nullopt,
                .role = object.value(QStringLiteral("role")).isString()
                            ? std::optional<std::string>{object.value(QStringLiteral("role"))
                                                             .toString()
                                                             .toStdString()}
                            : std::nullopt,
                .sortOrder = object.value(QStringLiteral("sortOrder")).toString().toULongLong(),
                .totalEmails = object.value(QStringLiteral("totalEmails")).toString().toULongLong(),
                .unreadEmails =
                    object.value(QStringLiteral("unreadEmails")).toString().toULongLong(),
                .totalThreads =
                    object.value(QStringLiteral("totalThreads")).toString().toULongLong(),
                .unreadThreads =
                    object.value(QStringLiteral("unreadThreads")).toString().toULongLong(),
                .isSubscribed = object.value(QStringLiteral("isSubscribed")).toBool(),
                .myRights =
                    {
                        .mayReadItems = rights.value(QStringLiteral("mayReadItems")).toBool(),
                        .mayAddItems = rights.value(QStringLiteral("mayAddItems")).toBool(),
                        .mayRemoveItems = rights.value(QStringLiteral("mayRemoveItems")).toBool(),
                        .maySetSeen = rights.value(QStringLiteral("maySetSeen")).toBool(),
                        .maySetKeywords = rights.value(QStringLiteral("maySetKeywords")).toBool(),
                        .mayCreateChild = rights.value(QStringLiteral("mayCreateChild")).toBool(),
                        .mayRename = rights.value(QStringLiteral("mayRename")).toBool(),
                        .mayDelete = rights.value(QStringLiteral("mayDelete")).toBool(),
                        .maySubmit = rights.value(QStringLiteral("maySubmit")).toBool(),
                    },
            };
        }

        [[nodiscard]] MutationRecord genericRecord(const MailboxDestroyMutationRecord& record)
        {
            const QJsonObject payload{
                {QStringLiteral("beforeMailbox"), mailboxJson(record.beforeMailbox)}};
            return MutationRecord{
                .mutationId = record.mutationId,
                .operationGroupId = record.operationGroupId,
                .domain = {.accountId = record.accountId, .dataType = std::string{dataType}},
                .objectId = record.mailboxId,
                .mutationKind = std::string{destroyMutationKind},
                .status = record.status,
                .payloadJson = QJsonDocument{payload}.toJson(QJsonDocument::Compact).toStdString(),
                .baseState = record.baseState,
                .acceptedState = record.acceptedState,
                .errorJson = record.errorJson,
            };
        }

        [[nodiscard]] std::variant<MailboxDestroyMutationRecord,
                                   javelin::jmap::cache::DatabaseError>
        typedDestroyRecord(MutationRecord record)
        {
            const auto document =
                QJsonDocument::fromJson(QByteArray::fromStdString(record.payloadJson));
            if (record.mutationKind != destroyMutationKind || !document.isObject())
            {
                return javelin::jmap::cache::DatabaseError{
                    .code = javelin::jmap::cache::DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("Invalid Mailbox destroy journal payload."),
                };
            }
            const auto before = mailboxFromJson(
                document.object().value(QStringLiteral("beforeMailbox")).toObject());
            if (!before.has_value())
            {
                return javelin::jmap::cache::DatabaseError{
                    .code = javelin::jmap::cache::DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("Incomplete Mailbox destroy journal payload."),
                };
            }
            return MailboxDestroyMutationRecord{
                .mutationId = std::move(record.mutationId),
                .operationGroupId = std::move(record.operationGroupId),
                .accountId = std::move(record.domain.accountId),
                .mailboxId = std::move(record.objectId),
                .status = record.status,
                .beforeMailbox = *before,
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
    MailboxMutationJournal::queue(const MailboxDestroyMutationRecord& record)
    {
        auto transactionResult = MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Queue Mailbox destroy mutation"));
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<MutationProjectionTransaction>(std::move(transactionResult));
        if (const auto error = transaction.append(genericRecord(record)))
            return error;
        const std::array mailboxIds{record.mailboxId};
        if (const auto error = m_repository.removeMany(transaction.cacheTransaction(),
                                                       record.accountId, mailboxIds))
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
    MailboxMutationJournal::transition(const MailboxDestroyMutationRecord& record,
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
    MailboxMutationJournal::reject(const MailboxDestroyMutationRecord& record,
                                   const std::optional<std::string_view> acceptedState,
                                   const std::optional<std::string_view> errorJson,
                                   std::optional<javelin::jmap::domain::Mailbox> confirmedMailbox)
    {
        auto transactionResult = MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Reject Mailbox destroy mutation"));
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<MutationProjectionTransaction>(std::move(transactionResult));
        if (const auto error = transaction.transition(record.mutationId, MutationStatus::Rejected,
                                                      acceptedState, errorJson))
            return error;
        const auto& mailbox =
            confirmedMailbox.has_value() ? *confirmedMailbox : record.beforeMailbox;
        if (const auto error = m_repository.upsertMany(transaction.cacheTransaction(),
                                                       record.accountId, {mailbox}))
            return error;
        const std::array domains{domain(record.accountId)};
        if (const auto error = transaction.advance(domains))
            return error;
        return transaction.commit();
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MailboxMutationJournal::accept(const MailboxDestroyMutationRecord& record,
                                   const std::string_view state)
    {
        auto transactionResult = MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Accept Mailbox destroy mutation"));
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<MutationProjectionTransaction>(std::move(transactionResult));
        if (const auto error =
                transaction.transition(record.mutationId, MutationStatus::Accepted, state))
            return error;
        const std::array mailboxIds{record.mailboxId};
        if (const auto error = m_repository.removeMany(transaction.cacheTransaction(),
                                                       record.accountId, mailboxIds))
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
                .message = QStringLiteral("Mailbox state changed before destroy acceptance."),
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
    MailboxMutationJournal::reconcileDestroyed(const MailboxDestroyMutationRecord& record,
                                               const std::string_view state)
    {
        auto transactionResult = MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Reconcile destroyed Mailbox mutation"));
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<MutationProjectionTransaction>(std::move(transactionResult));
        if (const auto error =
                transaction.transition(record.mutationId, MutationStatus::Accepted, state))
            return error;
        const std::array mailboxIds{record.mailboxId};
        if (const auto error = m_repository.removeMany(transaction.cacheTransaction(),
                                                       record.accountId, mailboxIds))
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

    std::optional<javelin::jmap::cache::DatabaseError>
    MailboxMutationJournal::reconcilePresent(const MailboxDestroyMutationRecord& record,
                                             const javelin::jmap::domain::Mailbox& mailbox,
                                             const std::string_view state)
    {
        auto transactionResult = MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Reconcile rejected Mailbox destroy mutation"));
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<MutationProjectionTransaction>(std::move(transactionResult));
        if (const auto error =
                transaction.transition(record.mutationId, MutationStatus::Rejected, state))
            return error;
        if (const auto error = m_repository.upsertMany(transaction.cacheTransaction(),
                                                       record.accountId, {mailbox}))
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
            if (generic.mutationKind != subscriptionMutationKind)
                continue;
            auto typed = typedRecord(std::move(generic));
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&typed))
                return *error;
            records.push_back(std::get<MailboxSubscriptionMutationRecord>(std::move(typed)));
        }
        return records;
    }

    std::variant<std::vector<MailboxDestroyMutationRecord>, javelin::jmap::cache::DatabaseError>
    MailboxMutationJournal::listActiveDestroys(const std::string_view accountId) const
    {
        MutationJournalRepository journal{m_connection};
        auto result = journal.listActive(domain(accountId));
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
            return *error;
        std::vector<MailboxDestroyMutationRecord> records;
        for (auto& generic : std::get<std::vector<MutationRecord>>(result))
        {
            if (generic.mutationKind != destroyMutationKind)
                continue;
            auto typed = typedDestroyRecord(std::move(generic));
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&typed))
                return *error;
            records.push_back(std::get<MailboxDestroyMutationRecord>(std::move(typed)));
        }
        return records;
    }

    std::variant<bool, javelin::jmap::cache::DatabaseError>
    MailboxMutationJournal::hasActive(const std::string_view accountId) const
    {
        MutationJournalRepository journal{m_connection};
        auto result = journal.listActive(domain(accountId));
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
            return *error;
        return !std::get<std::vector<MutationRecord>>(result).empty();
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MailboxMutationJournal::rebase(javelin::jmap::cache::DatabaseTransaction& transaction,
                                   const std::string_view accountId) const
    {
        MutationJournalRepository journal{m_connection};
        auto activeResult = journal.listActive(domain(accountId));
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&activeResult))
            return *error;
        for (auto& generic : std::get<std::vector<MutationRecord>>(activeResult))
        {
            if (!projectsOptimistically(generic.status))
                continue;
            if (generic.mutationKind == subscriptionMutationKind)
            {
                auto typed = typedRecord(std::move(generic));
                if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&typed))
                    return *error;
                const auto& record = std::get<MailboxSubscriptionMutationRecord>(typed);
                const auto mailbox = m_repository.find(record.accountId, record.mailboxId);
                if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&mailbox))
                    return *error;
                if (!std::get<std::optional<javelin::jmap::domain::Mailbox>>(mailbox).has_value())
                    continue;
                if (const auto error = m_repository.setSubscribed(
                        transaction, record.accountId, record.mailboxId, record.afterSubscribed))
                    return error;
            }
            else if (generic.mutationKind == destroyMutationKind)
            {
                auto typed = typedDestroyRecord(std::move(generic));
                if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&typed))
                    return *error;
                const auto& record = std::get<MailboxDestroyMutationRecord>(typed);
                const std::array mailboxIds{record.mailboxId};
                if (const auto error =
                        m_repository.removeMany(transaction, record.accountId, mailboxIds))
                    return error;
            }
        }
        return std::nullopt;
    }
} // namespace javelin::jmap::sync
