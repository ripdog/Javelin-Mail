#include "jmap/contacts/AddressBookMutationJournal.h"

#include <glaze/glaze.hpp>

#include <algorithm>

namespace
{
    struct RawAddressBookMutation
    {
        std::string kind;
        std::optional<std::string> creationId;
        std::string requestedDocument;
        std::optional<std::string> baseDocument;
        std::optional<std::string> projectedDocument;
    };
} // namespace

template <> struct glz::meta<RawAddressBookMutation>
{
    using T = RawAddressBookMutation;
    static constexpr auto value = glz::object(
        "kind", &T::kind, "creationId", &T::creationId, "requestedDocument", &T::requestedDocument,
        "baseDocument", &T::baseDocument, "projectedDocument", &T::projectedDocument);
};

namespace javelin::jmap::contacts
{
    namespace
    {
        [[nodiscard]] std::string_view kindName(const AddressBookMutationKind kind)
        {
            switch (kind)
            {
            case AddressBookMutationKind::Create:
                return "create";
            case AddressBookMutationKind::Update:
                return "update";
            case AddressBookMutationKind::Destroy:
                return "destroy";
            }
            return "update";
        }

        [[nodiscard]] std::optional<AddressBookMutationKind> parseKind(const std::string_view kind)
        {
            if (kind == "create")
                return AddressBookMutationKind::Create;
            if (kind == "update")
                return AddressBookMutationKind::Update;
            if (kind == "destroy")
                return AddressBookMutationKind::Destroy;
            return std::nullopt;
        }

        [[nodiscard]] std::variant<sync::MutationRecord, cache::DatabaseError>
        genericRecord(const AddressBookMutationRecord& record)
        {
            std::string payload;
            if (glz::write_json(
                    RawAddressBookMutation{
                        .kind = std::string{kindName(record.kind)},
                        .creationId = record.creationId,
                        .requestedDocument = record.requestedDocument,
                        .baseDocument = record.baseDocument,
                        .projectedDocument = record.projectedDocument,
                    },
                    payload))
            {
                return cache::DatabaseError{
                    .code = cache::DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("Unable to serialize an AddressBook mutation."),
                };
            }
            return sync::MutationRecord{
                .mutationId = record.mutationId,
                .operationGroupId = record.operationGroupId,
                .domain = {.accountId = record.accountId, .dataType = "AddressBook"},
                .objectId = record.objectId,
                .mutationKind = "address_book_set",
                .status = record.status,
                .payloadJson = std::move(payload),
                .baseState = record.baseState,
                .acceptedState = record.acceptedState,
                .errorJson = record.errorJson,
            };
        }

        [[nodiscard]] std::variant<AddressBookMutationRecord, cache::DatabaseError>
        typedRecord(sync::MutationRecord record)
        {
            RawAddressBookMutation payload;
            if (record.mutationKind != "address_book_set" ||
                glz::read<glz::opts{.error_on_unknown_keys = false}>(payload, record.payloadJson))
            {
                return cache::DatabaseError{
                    .code = cache::DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("Invalid AddressBook mutation journal payload."),
                };
            }
            const auto kind = parseKind(payload.kind);
            if (!kind.has_value())
            {
                return cache::DatabaseError{
                    .code = cache::DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("Invalid AddressBook mutation kind."),
                };
            }
            return AddressBookMutationRecord{
                .mutationId = std::move(record.mutationId),
                .operationGroupId = std::move(record.operationGroupId),
                .accountId = std::move(record.domain.accountId),
                .objectId = std::move(record.objectId),
                .creationId = std::move(payload.creationId),
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

    AddressBookMutationJournal::AddressBookMutationJournal(cache::DatabaseConnection& connection,
                                                           cache::ContactRepository& contacts)
        : m_connection(connection), m_contacts(contacts), m_journal(connection)
    {
    }

    std::optional<cache::DatabaseError>
    AddressBookMutationJournal::queue(const std::vector<AddressBookMutationRecord>& records,
                                      const std::vector<api::AddressBook>& projectedBooks,
                                      const std::string_view state)
    {
        if (records.empty())
        {
            return cache::DatabaseError{
                .code = cache::DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("An AddressBook projection requires mutation records."),
            };
        }
        const auto& accountId = records.front().accountId;
        if (accountId.empty() ||
            std::ranges::any_of(records, [&accountId](const AddressBookMutationRecord& record)
                                { return record.accountId != accountId; }))
        {
            return cache::DatabaseError{
                .code = cache::DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("AddressBook projections must belong to one account."),
            };
        }
        auto transactionResult = sync::MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Project AddressBook mutations"));
        if (const auto* error = std::get_if<cache::DatabaseError>(&transactionResult))
            return *error;
        auto transaction =
            std::get<sync::MutationProjectionTransaction>(std::move(transactionResult));
        for (const auto& record : records)
        {
            const auto generic = genericRecord(record);
            if (const auto* error = std::get_if<cache::DatabaseError>(&generic))
                return *error;
            if (const auto error = transaction.append(std::get<sync::MutationRecord>(generic)))
                return error;
        }
        if (const auto error = m_contacts.replaceAddressBooks(transaction.cacheTransaction(),
                                                              accountId, projectedBooks, state))
            return error;
        if (const auto error = transaction.commit())
            return error;
        m_contacts.notifyChanged(accountId);
        return std::nullopt;
    }

    std::variant<std::vector<AddressBookMutationRecord>, cache::DatabaseError>
    AddressBookMutationJournal::listForAddressBook(const std::string_view accountId,
                                                   const std::string_view addressBookId) const
    {
        auto result = m_journal.listForObject(
            {.accountId = std::string{accountId}, .dataType = "AddressBook"}, addressBookId);
        if (const auto* error = std::get_if<cache::DatabaseError>(&result))
            return *error;
        std::vector<AddressBookMutationRecord> records;
        for (auto& generic : std::get<std::vector<sync::MutationRecord>>(result))
        {
            auto typed = typedRecord(std::move(generic));
            if (const auto* error = std::get_if<cache::DatabaseError>(&typed))
                return *error;
            records.push_back(std::get<AddressBookMutationRecord>(std::move(typed)));
        }
        return records;
    }

    std::variant<std::vector<AddressBookMutationRecord>, cache::DatabaseError>
    AddressBookMutationJournal::listActive(const std::string_view accountId) const
    {
        auto result =
            m_journal.listActive({.accountId = std::string{accountId}, .dataType = "AddressBook"});
        if (const auto* error = std::get_if<cache::DatabaseError>(&result))
            return *error;
        std::vector<AddressBookMutationRecord> records;
        for (auto& generic : std::get<std::vector<sync::MutationRecord>>(result))
        {
            auto typed = typedRecord(std::move(generic));
            if (const auto* error = std::get_if<cache::DatabaseError>(&typed))
                return *error;
            records.push_back(std::get<AddressBookMutationRecord>(std::move(typed)));
        }
        return records;
    }

    std::optional<cache::DatabaseError>
    AddressBookMutationJournal::transition(const std::vector<AddressBookMutationRecord>& records,
                                           const sync::MutationStatus status,
                                           const std::optional<std::string_view> acceptedState,
                                           const std::optional<std::string_view> errorJson)
    {
        if (records.empty())
            return std::nullopt;
        auto transactionResult = sync::MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Transition AddressBook mutations"));
        if (const auto* error = std::get_if<cache::DatabaseError>(&transactionResult))
            return *error;
        auto transaction =
            std::get<sync::MutationProjectionTransaction>(std::move(transactionResult));
        for (const auto& record : records)
        {
            if (const auto error =
                    transaction.transition(record.mutationId, status, acceptedState, errorJson))
                return error;
        }
        return transaction.commit();
    }
} // namespace javelin::jmap::contacts
