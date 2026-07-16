#include "jmap/contacts/ContactMutationJournal.h"

#include "jmap/contacts/ContactTypes.h"

#include <glaze/glaze.hpp>

#include <algorithm>

namespace
{
    struct RawContactMutation
    {
        std::string kind;
        std::string requestedDocument;
        std::optional<std::string> baseDocument;
        std::optional<std::string> projectedDocument;
    };
} // namespace

template <> struct glz::meta<RawContactMutation>
{
    using T = RawContactMutation;
    static constexpr auto value =
        glz::object("kind", &T::kind, "requestedDocument", &T::requestedDocument, "baseDocument",
                    &T::baseDocument, "projectedDocument", &T::projectedDocument);
};

namespace javelin::jmap::contacts
{
    namespace
    {
        [[nodiscard]] std::string_view kindName(const ContactMutationKind kind)
        {
            switch (kind)
            {
            case ContactMutationKind::Create:
                return "create";
            case ContactMutationKind::Update:
                return "update";
            case ContactMutationKind::Destroy:
                return "destroy";
            }
            return "update";
        }

        [[nodiscard]] std::optional<ContactMutationKind> parseKind(const std::string_view kind)
        {
            if (kind == "create")
                return ContactMutationKind::Create;
            if (kind == "update")
                return ContactMutationKind::Update;
            if (kind == "destroy")
                return ContactMutationKind::Destroy;
            return std::nullopt;
        }

        [[nodiscard]] std::variant<sync::MutationRecord, cache::DatabaseError>
        genericRecord(const ContactMutationRecord& record)
        {
            std::string payload;
            if (glz::write_json(
                    RawContactMutation{
                        .kind = std::string{kindName(record.kind)},
                        .requestedDocument = record.requestedDocument,
                        .baseDocument = record.baseDocument,
                        .projectedDocument = record.projectedDocument,
                    },
                    payload))
            {
                return cache::DatabaseError{
                    .code = cache::DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("Unable to serialize a ContactCard mutation."),
                };
            }
            return sync::MutationRecord{
                .mutationId = record.mutationId,
                .operationGroupId = record.operationGroupId,
                .domain = {.accountId = record.accountId, .dataType = "ContactCard"},
                .objectId = record.objectId,
                .mutationKind = "contact_card_patch",
                .status = record.status,
                .payloadJson = std::move(payload),
                .baseState = record.baseState,
                .acceptedState = record.acceptedState,
                .errorJson = record.errorJson,
            };
        }

        [[nodiscard]] std::variant<ContactMutationRecord, cache::DatabaseError>
        typedRecord(sync::MutationRecord record)
        {
            RawContactMutation payload;
            if (record.mutationKind != "contact_card_patch" ||
                glz::read<glz::opts{.error_on_unknown_keys = false}>(payload, record.payloadJson))
            {
                return cache::DatabaseError{
                    .code = cache::DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("Invalid ContactCard mutation journal payload."),
                };
            }
            const auto kind = parseKind(payload.kind);
            if (!kind.has_value())
            {
                return cache::DatabaseError{
                    .code = cache::DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("Invalid ContactCard mutation kind."),
                };
            }
            return ContactMutationRecord{
                .mutationId = std::move(record.mutationId),
                .operationGroupId = std::move(record.operationGroupId),
                .accountId = std::move(record.domain.accountId),
                .objectId = std::move(record.objectId),
                .kind = *kind,
                .status = record.status,
                .requestedDocument = std::move(payload.requestedDocument),
                .baseDocument = std::move(payload.baseDocument),
                .projectedDocument = std::move(payload.projectedDocument),
                .baseState = std::move(record.baseState),
                .acceptedState = std::move(record.acceptedState),
                .errorJson = std::move(record.errorJson),
            };
        }
    } // namespace

    ContactMutationJournal::ContactMutationJournal(cache::DatabaseConnection& connection,
                                                   cache::ContactRepository& contacts)
        : m_connection(connection), m_contacts(contacts), m_journal(connection)
    {
    }

    std::optional<cache::DatabaseError>
    ContactMutationJournal::queue(const std::vector<ContactMutationRecord>& records,
                                  const std::vector<ContactSummary>& projectedContacts,
                                  const std::span<const std::string> destroyedIds)
    {
        if (records.empty())
        {
            return cache::DatabaseError{
                .code = cache::DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("A ContactCard projection requires mutation records."),
            };
        }
        const auto& accountId = records.front().accountId;
        if (accountId.empty() ||
            std::ranges::any_of(records, [&accountId](const ContactMutationRecord& record)
                                { return record.accountId != accountId; }))
        {
            return cache::DatabaseError{
                .code = cache::DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("ContactCard projections must belong to one account."),
            };
        }
        auto transactionResult = sync::MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Project ContactCard mutations"));
        if (const auto* error = std::get_if<cache::DatabaseError>(&transactionResult))
        {
            return *error;
        }
        auto transaction =
            std::get<sync::MutationProjectionTransaction>(std::move(transactionResult));
        for (const auto& record : records)
        {
            const auto generic = genericRecord(record);
            if (const auto* error = std::get_if<cache::DatabaseError>(&generic))
            {
                return *error;
            }
            if (const auto error = transaction.append(std::get<sync::MutationRecord>(generic)))
            {
                return error;
            }
        }
        if (const auto error = m_contacts.projectContacts(transaction.cacheTransaction(), accountId,
                                                          projectedContacts, destroyedIds))
        {
            return error;
        }
        if (const auto error = transaction.commit())
        {
            return error;
        }
        m_contacts.notifyChanged(accountId);
        return std::nullopt;
    }

    std::variant<std::vector<ContactMutationRecord>, cache::DatabaseError>
    ContactMutationJournal::listForContact(const std::string_view accountId,
                                           const std::string_view contactId) const
    {
        auto result = m_journal.listForObject(
            {.accountId = std::string{accountId}, .dataType = "ContactCard"}, contactId);
        if (const auto* error = std::get_if<cache::DatabaseError>(&result))
        {
            return *error;
        }
        std::vector<ContactMutationRecord> records;
        for (auto& generic : std::get<std::vector<sync::MutationRecord>>(result))
        {
            auto typed = typedRecord(std::move(generic));
            if (const auto* error = std::get_if<cache::DatabaseError>(&typed))
            {
                return *error;
            }
            records.push_back(std::get<ContactMutationRecord>(std::move(typed)));
        }
        return records;
    }

    std::optional<cache::DatabaseError>
    ContactMutationJournal::restoreRejected(const std::vector<ContactMutationRecord>& records,
                                            const std::optional<std::string_view> errorJson)
    {
        if (records.empty())
        {
            return std::nullopt;
        }
        auto transactionResult = sync::MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Reject ContactCard mutations"));
        if (const auto* error = std::get_if<cache::DatabaseError>(&transactionResult))
        {
            return *error;
        }
        auto transaction =
            std::get<sync::MutationProjectionTransaction>(std::move(transactionResult));
        std::vector<ContactSummary> restored;
        std::vector<std::string> removed;
        for (const auto& record : records)
        {
            if (const auto error = transaction.transition(
                    record.mutationId, sync::MutationStatus::Rejected, std::nullopt, errorJson))
            {
                return error;
            }
            if (record.baseDocument.has_value())
            {
                const auto summary =
                    summarizeContact(record.accountId, api::ContactCard{
                                                           .id = record.objectId,
                                                           .uid = {},
                                                           .kind = {},
                                                           .document = *record.baseDocument,
                                                       });
                if (!summary.has_value())
                {
                    return cache::DatabaseError{
                        .code = cache::DatabaseErrorCode::QueryFailed,
                        .message =
                            QStringLiteral("Unable to restore a rejected ContactCard mutation."),
                    };
                }
                restored.push_back(*summary);
            }
            else if (record.kind == ContactMutationKind::Create)
            {
                removed.push_back(record.objectId);
            }
        }
        if (const auto error = m_contacts.projectContacts(
                transaction.cacheTransaction(), records.front().accountId, restored, removed))
        {
            return error;
        }
        if (const auto error = transaction.commit())
        {
            return error;
        }
        if (!records.empty())
        {
            m_contacts.notifyChanged(records.front().accountId);
        }
        return std::nullopt;
    }
} // namespace javelin::jmap::contacts
