#include "jmap/sieve/SieveMutationJournal.h"

#include <glaze/glaze.hpp>

namespace
{
    struct RawSieveScript
    {
        std::string id;
        std::string name;
        std::string blobId;
        bool isActive = false;
    };

    struct RawSieveMutation
    {
        std::string kind;
        std::vector<RawSieveScript> baseScripts;
        std::vector<RawSieveScript> projectedScripts;
    };
} // namespace

template <> struct glz::meta<RawSieveScript>
{
    using T = RawSieveScript;
    static constexpr auto value =
        glz::object("id", &T::id, "name", &T::name, "blobId", &T::blobId, "isActive", &T::isActive);
};

template <> struct glz::meta<RawSieveMutation>
{
    using T = RawSieveMutation;
    static constexpr auto value = glz::object("kind", &T::kind, "baseScripts", &T::baseScripts,
                                              "projectedScripts", &T::projectedScripts);
};

namespace javelin::jmap::sieve
{
    namespace
    {
        [[nodiscard]] std::string_view kindName(const SieveMutationKind kind)
        {
            switch (kind)
            {
            case SieveMutationKind::Create:
                return "create";
            case SieveMutationKind::Update:
                return "update";
            case SieveMutationKind::Destroy:
                return "destroy";
            case SieveMutationKind::Activate:
                return "activate";
            }
            return "update";
        }

        [[nodiscard]] RawSieveScript raw(const SieveScript& script)
        {
            return {.id = script.id,
                    .name = script.name,
                    .blobId = script.blobId,
                    .isActive = script.isActive};
        }

        [[nodiscard]] std::variant<sync::MutationRecord, cache::DatabaseError>
        genericRecord(const SieveMutationRecord& record)
        {
            RawSieveMutation payload{
                .kind = std::string{kindName(record.kind)},
                .baseScripts = {},
                .projectedScripts = {},
            };
            for (const auto& script : record.baseScripts)
                payload.baseScripts.push_back(raw(script));
            for (const auto& script : record.projectedScripts)
                payload.projectedScripts.push_back(raw(script));
            std::string payloadJson;
            if (glz::write_json(payload, payloadJson))
                return cache::DatabaseError{
                    .code = cache::DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("Unable to serialize a Sieve mutation."),
                };
            return sync::MutationRecord{
                .mutationId = record.mutationId,
                .operationGroupId = record.operationGroupId,
                .domain = {.accountId = record.accountId, .dataType = "SieveScript"},
                .objectId = record.objectId,
                .mutationKind = "sieve_script_set",
                .status = record.status,
                .payloadJson = std::move(payloadJson),
                .baseState = record.baseState,
                .acceptedState = record.acceptedState,
                .errorJson = record.errorJson,
            };
        }
    } // namespace

    SieveMutationJournal::SieveMutationJournal(cache::DatabaseConnection& connection,
                                               cache::SieveRepository& repository)
        : m_connection(connection), m_repository(repository)
    {
    }

    std::optional<cache::DatabaseError>
    SieveMutationJournal::queue(const SieveMutationRecord& record)
    {
        auto transactionResult = sync::MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Project Sieve mutation"));
        if (const auto* error = std::get_if<cache::DatabaseError>(&transactionResult))
            return *error;
        auto transaction =
            std::get<sync::MutationProjectionTransaction>(std::move(transactionResult));
        const auto generic = genericRecord(record);
        if (const auto* error = std::get_if<cache::DatabaseError>(&generic))
            return *error;
        if (const auto error = transaction.append(std::get<sync::MutationRecord>(generic)))
            return error;
        if (const auto error = m_repository.project(transaction.cacheTransaction(),
                                                    record.accountId, record.projectedScripts))
            return error;
        return transaction.commit();
    }

    std::optional<cache::DatabaseError>
    SieveMutationJournal::transition(const SieveMutationRecord& record,
                                     const sync::MutationStatus status,
                                     const std::optional<std::string_view> acceptedState,
                                     const std::optional<std::string_view> errorJson)
    {
        sync::MutationJournalRepository journal{m_connection};
        return journal.transition(record.mutationId, status, acceptedState, errorJson);
    }

    std::optional<cache::DatabaseError>
    SieveMutationJournal::restoreRejected(const SieveMutationRecord& record,
                                          const std::optional<std::string_view> acceptedState,
                                          const std::optional<std::string_view> errorJson)
    {
        auto transactionResult = sync::MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Reject Sieve mutation"));
        if (const auto* error = std::get_if<cache::DatabaseError>(&transactionResult))
            return *error;
        auto transaction =
            std::get<sync::MutationProjectionTransaction>(std::move(transactionResult));
        if (const auto error = transaction.transition(
                record.mutationId, sync::MutationStatus::Rejected, acceptedState, errorJson))
            return error;
        if (acceptedState.has_value())
        {
            if (const auto error =
                    m_repository.replaceAll(transaction.cacheTransaction(), record.accountId,
                                            record.baseScripts, *acceptedState))
                return error;
        }
        else if (const auto error = m_repository.project(transaction.cacheTransaction(),
                                                         record.accountId, record.baseScripts))
            return error;
        return transaction.commit();
    }
} // namespace javelin::jmap::sieve
