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

        [[nodiscard]] std::optional<SieveMutationKind> parseKind(const std::string_view kind)
        {
            if (kind == "create")
                return SieveMutationKind::Create;
            if (kind == "update")
                return SieveMutationKind::Update;
            if (kind == "destroy")
                return SieveMutationKind::Destroy;
            if (kind == "activate")
                return SieveMutationKind::Activate;
            return std::nullopt;
        }

        [[nodiscard]] SieveScript typed(const RawSieveScript& script)
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
        const std::array domains{sync::ConsistencyDomain{
            .accountId = record.accountId,
            .dataType = "SieveScript",
        }};
        if (const auto error = transaction.advance(domains))
            return error;
        if (const auto error = transaction.retireTerminal())
            return error;
        return transaction.commit();
    }

    std::variant<std::vector<SieveMutationRecord>, cache::DatabaseError>
    SieveMutationJournal::listActive(const std::string_view accountId) const
    {
        sync::MutationJournalRepository journal{m_connection};
        auto result =
            journal.listActive({.accountId = std::string{accountId}, .dataType = "SieveScript"});
        if (const auto* error = std::get_if<cache::DatabaseError>(&result))
            return *error;
        std::vector<SieveMutationRecord> records;
        for (auto& generic : std::get<std::vector<sync::MutationRecord>>(result))
        {
            RawSieveMutation payload;
            if (generic.mutationKind != "sieve_script_set" ||
                glz::read<glz::opts{.error_on_unknown_keys = false}>(payload, generic.payloadJson))
                return cache::DatabaseError{
                    .code = cache::DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("Invalid Sieve mutation journal payload."),
                };
            const auto kind = parseKind(payload.kind);
            if (!kind.has_value())
                return cache::DatabaseError{
                    .code = cache::DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("Invalid Sieve mutation kind."),
                };
            SieveMutationRecord record{
                .mutationId = std::move(generic.mutationId),
                .operationGroupId = std::move(generic.operationGroupId),
                .accountId = std::move(generic.domain.accountId),
                .objectId = std::move(generic.objectId),
                .kind = *kind,
                .status = generic.status,
                .baseScripts = {},
                .projectedScripts = {},
                .baseState = std::move(generic.baseState),
                .acceptedState = std::move(generic.acceptedState),
                .errorJson = std::move(generic.errorJson),
            };
            for (const auto& script : payload.baseScripts)
                record.baseScripts.push_back(typed(script));
            for (const auto& script : payload.projectedScripts)
                record.projectedScripts.push_back(typed(script));
            records.push_back(std::move(record));
        }
        return records;
    }
} // namespace javelin::jmap::sieve
