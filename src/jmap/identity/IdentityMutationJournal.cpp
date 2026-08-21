#include "jmap/identity/IdentityMutationJournal.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <array>
#include <utility>

namespace javelin::jmap::identity
{
    namespace
    {
        [[nodiscard]] QString kindName(const IdentityMutationKind kind)
        {
            switch (kind)
            {
            case IdentityMutationKind::Create:
                return QStringLiteral("create");
            case IdentityMutationKind::Update:
                return QStringLiteral("update");
            case IdentityMutationKind::Destroy:
                return QStringLiteral("destroy");
            }
            return QStringLiteral("update");
        }

        [[nodiscard]] std::optional<IdentityMutationKind> parseKind(const QString& value)
        {
            if (value == QStringLiteral("create"))
                return IdentityMutationKind::Create;
            if (value == QStringLiteral("update"))
                return IdentityMutationKind::Update;
            if (value == QStringLiteral("destroy"))
                return IdentityMutationKind::Destroy;
            return std::nullopt;
        }

        [[nodiscard]] QJsonArray
        addresses(const std::vector<javelin::jmap::domain::EmailAddress>& values)
        {
            QJsonArray result;
            for (const auto& value : values)
            {
                QJsonObject address;
                if (value.name.has_value())
                    address.insert(QStringLiteral("name"), QString::fromStdString(*value.name));
                address.insert(QStringLiteral("email"), QString::fromStdString(value.email));
                result.push_back(address);
            }
            return result;
        }

        [[nodiscard]] std::vector<javelin::jmap::domain::EmailAddress>
        addresses(const QJsonValue& value)
        {
            std::vector<javelin::jmap::domain::EmailAddress> result;
            if (!value.isArray())
                return result;
            for (const auto& item : value.toArray())
            {
                if (!item.isObject())
                    continue;
                const auto object = item.toObject();
                const auto email = object.value(QStringLiteral("email")).toString();
                if (email.isEmpty())
                    continue;
                const auto name = object.value(QStringLiteral("name")).toString();
                result.push_back({
                    .name = name.isEmpty() ? std::nullopt
                                           : std::optional<std::string>{name.toStdString()},
                    .email = email.toStdString(),
                });
            }
            return result;
        }

        [[nodiscard]] QJsonObject identity(const javelin::jmap::domain::Identity& value)
        {
            QJsonObject object{
                {QStringLiteral("id"), QString::fromStdString(value.id)},
                {QStringLiteral("name"), QString::fromStdString(value.name)},
                {QStringLiteral("email"), QString::fromStdString(value.email)},
                {QStringLiteral("replyTo"), addresses(value.replyTo)},
                {QStringLiteral("bcc"), addresses(value.bcc)},
                {QStringLiteral("mayDelete"), value.mayDelete},
            };
            if (value.textSignature.has_value())
                object.insert(QStringLiteral("textSignature"),
                              QString::fromStdString(*value.textSignature));
            if (value.htmlSignature.has_value())
                object.insert(QStringLiteral("htmlSignature"),
                              QString::fromStdString(*value.htmlSignature));
            return object;
        }

        [[nodiscard]] std::optional<javelin::jmap::domain::Identity>
        identity(const QJsonValue& value)
        {
            if (!value.isObject())
                return std::nullopt;
            const auto object = value.toObject();
            const auto email = object.value(QStringLiteral("email")).toString();
            if (email.isEmpty())
                return std::nullopt;
            const auto text = object.value(QStringLiteral("textSignature"));
            const auto html = object.value(QStringLiteral("htmlSignature"));
            return javelin::jmap::domain::Identity{
                .id = object.value(QStringLiteral("id")).toString().toStdString(),
                .name = object.value(QStringLiteral("name")).toString().toStdString(),
                .email = email.toStdString(),
                .replyTo = addresses(object.value(QStringLiteral("replyTo"))),
                .bcc = addresses(object.value(QStringLiteral("bcc"))),
                .textSignature = text.isString()
                                     ? std::optional<std::string>{text.toString().toStdString()}
                                     : std::nullopt,
                .htmlSignature = html.isString()
                                     ? std::optional<std::string>{html.toString().toStdString()}
                                     : std::nullopt,
                .mayDelete = object.value(QStringLiteral("mayDelete")).toBool(),
            };
        }

        [[nodiscard]] std::variant<sync::MutationRecord, cache::DatabaseError>
        genericRecord(const IdentityMutationRecord& record)
        {
            QJsonObject payload{{QStringLiteral("kind"), kindName(record.kind)}};
            if (record.creationId.has_value())
                payload.insert(QStringLiteral("creationId"),
                               QString::fromStdString(*record.creationId));
            if (record.before.has_value())
                payload.insert(QStringLiteral("before"), identity(*record.before));
            if (record.after.has_value())
                payload.insert(QStringLiteral("after"), identity(*record.after));
            return sync::MutationRecord{
                .mutationId = record.mutationId,
                .operationGroupId = record.operationGroupId,
                .domain = {.accountId = record.accountId, .dataType = "Identity"},
                .objectId = record.objectId,
                .mutationKind = "identity_set",
                .status = record.status,
                .payloadJson = QJsonDocument{payload}.toJson(QJsonDocument::Compact).toStdString(),
                .baseState = record.baseState,
                .acceptedState = record.acceptedState,
                .errorJson = record.errorJson,
            };
        }

        [[nodiscard]] std::variant<IdentityMutationRecord, cache::DatabaseError>
        typedRecord(sync::MutationRecord record)
        {
            const auto document =
                QJsonDocument::fromJson(QByteArray::fromStdString(record.payloadJson));
            if (record.mutationKind != "identity_set" || !document.isObject())
            {
                return cache::DatabaseError{
                    .code = cache::DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("Invalid Identity mutation journal payload."),
                };
            }
            const auto object = document.object();
            const auto kind = parseKind(object.value(QStringLiteral("kind")).toString());
            if (!kind.has_value())
            {
                return cache::DatabaseError{
                    .code = cache::DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("Invalid Identity mutation kind."),
                };
            }
            const auto before = object.contains(QStringLiteral("before"))
                                    ? identity(object.value(QStringLiteral("before")))
                                    : std::nullopt;
            const auto after = object.contains(QStringLiteral("after"))
                                   ? identity(object.value(QStringLiteral("after")))
                                   : std::nullopt;
            if ((*kind == IdentityMutationKind::Create && !after.has_value()) ||
                (*kind == IdentityMutationKind::Update &&
                 (!before.has_value() || !after.has_value())) ||
                (*kind == IdentityMutationKind::Destroy && !before.has_value()))
            {
                return cache::DatabaseError{
                    .code = cache::DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("Incomplete Identity mutation payload."),
                };
            }
            const auto creation = object.value(QStringLiteral("creationId")).toString();
            return IdentityMutationRecord{
                .mutationId = std::move(record.mutationId),
                .operationGroupId = std::move(record.operationGroupId),
                .accountId = std::move(record.domain.accountId),
                .objectId = std::move(record.objectId),
                .creationId = creation.isEmpty()
                                  ? std::nullopt
                                  : std::optional<std::string>{creation.toStdString()},
                .kind = *kind,
                .status = record.status,
                .before = before,
                .after = after,
                .baseState = std::move(record.baseState),
                .acceptedState = std::move(record.acceptedState),
                .errorJson = std::move(record.errorJson),
            };
        }

        [[nodiscard]] std::optional<cache::DatabaseError>
        project(cache::IdentityRepository& repository, cache::DatabaseTransaction& transaction,
                const IdentityMutationRecord& record)
        {
            switch (record.kind)
            {
            case IdentityMutationKind::Create:
                return repository.projectPendingCreate(transaction, record.accountId,
                                                       *record.creationId, record.mutationId,
                                                       *record.after);
            case IdentityMutationKind::Update:
                return repository.projectUpsert(transaction, record.accountId, *record.after);
            case IdentityMutationKind::Destroy:
                return repository.projectDestroy(transaction, record.accountId, record.objectId);
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<cache::DatabaseError>
        restore(cache::IdentityRepository& repository, cache::DatabaseTransaction& transaction,
                const IdentityMutationRecord& record)
        {
            switch (record.kind)
            {
            case IdentityMutationKind::Create:
                return repository.removePendingCreate(transaction, record.accountId,
                                                      *record.creationId);
            case IdentityMutationKind::Update:
            case IdentityMutationKind::Destroy:
                return repository.projectUpsert(transaction, record.accountId, *record.before);
            }
            return std::nullopt;
        }
    } // namespace

    IdentityMutationJournal::IdentityMutationJournal(cache::DatabaseConnection& connection,
                                                     cache::IdentityRepository& repository)
        : m_connection(connection), m_repository(repository)
    {
    }

    std::optional<cache::DatabaseError>
    IdentityMutationJournal::queue(const IdentityMutationRecord& record)
    {
        auto transactionResult = sync::MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Queue Identity mutation"));
        if (const auto* error = std::get_if<cache::DatabaseError>(&transactionResult))
            return *error;
        auto transaction =
            std::get<sync::MutationProjectionTransaction>(std::move(transactionResult));
        const auto generic = genericRecord(record);
        if (const auto* error = std::get_if<cache::DatabaseError>(&generic))
            return *error;
        if (const auto error = transaction.append(std::get<sync::MutationRecord>(generic)))
            return error;
        if (const auto error = project(m_repository, transaction.cacheTransaction(), record))
            return error;
        const std::array domains{sync::ConsistencyDomain{
            .accountId = record.accountId,
            .dataType = "Identity",
        }};
        if (const auto error = transaction.advance(domains))
            return error;
        return transaction.commit();
    }

    std::optional<cache::DatabaseError>
    IdentityMutationJournal::transition(const IdentityMutationRecord& record,
                                        const sync::MutationStatus status,
                                        const std::optional<std::string_view> acceptedState,
                                        const std::optional<std::string_view> errorJson)
    {
        sync::MutationJournalRepository journal{m_connection};
        return journal.transition(record.mutationId, status, acceptedState, errorJson);
    }

    std::optional<cache::DatabaseError>
    IdentityMutationJournal::reject(const IdentityMutationRecord& record,
                                    const std::optional<std::string_view> acceptedState,
                                    const std::optional<std::string_view> errorJson)
    {
        auto transactionResult = sync::MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Reject Identity mutation"));
        if (const auto* error = std::get_if<cache::DatabaseError>(&transactionResult))
            return *error;
        auto transaction =
            std::get<sync::MutationProjectionTransaction>(std::move(transactionResult));
        if (const auto error = transaction.transition(
                record.mutationId, sync::MutationStatus::Rejected, acceptedState, errorJson))
            return error;
        if (const auto error = restore(m_repository, transaction.cacheTransaction(), record))
            return error;
        const std::array domains{sync::ConsistencyDomain{
            .accountId = record.accountId,
            .dataType = "Identity",
        }};
        if (const auto error = transaction.advance(domains))
            return error;
        if (const auto error = transaction.retireTerminal())
            return error;
        return transaction.commit();
    }

    std::optional<cache::DatabaseError>
    IdentityMutationJournal::accept(const IdentityMutationRecord& record,
                                    const std::vector<javelin::jmap::domain::Identity>& confirmed,
                                    const std::string_view state)
    {
        auto transactionResult = sync::MutationProjectionTransaction::begin(
            m_connection, QStringLiteral("Accept Identity mutation"));
        if (const auto* error = std::get_if<cache::DatabaseError>(&transactionResult))
            return *error;
        auto transaction =
            std::get<sync::MutationProjectionTransaction>(std::move(transactionResult));
        if (const auto error =
                transaction.transition(record.mutationId, sync::MutationStatus::Accepted, state))
            return error;
        if (record.creationId.has_value())
        {
            if (const auto error = m_repository.removePendingCreate(
                    transaction.cacheTransaction(), record.accountId, *record.creationId))
                return error;
        }
        if (const auto error = m_repository.replaceAll(transaction.cacheTransaction(),
                                                       record.accountId, confirmed, state))
            return error;
        const std::array domains{sync::ConsistencyDomain{
            .accountId = record.accountId,
            .dataType = "Identity",
        }};
        if (const auto error = transaction.advance(domains))
            return error;
        if (const auto error = transaction.retireTerminal())
            return error;
        return transaction.commit();
    }

    std::variant<std::vector<IdentityMutationRecord>, cache::DatabaseError>
    IdentityMutationJournal::listActive(const std::string_view accountId) const
    {
        sync::MutationJournalRepository journal{m_connection};
        auto result =
            journal.listActive({.accountId = std::string{accountId}, .dataType = "Identity"});
        if (const auto* error = std::get_if<cache::DatabaseError>(&result))
            return *error;
        std::vector<IdentityMutationRecord> records;
        for (auto& generic : std::get<std::vector<sync::MutationRecord>>(result))
        {
            auto typed = typedRecord(std::move(generic));
            if (const auto* error = std::get_if<cache::DatabaseError>(&typed))
                return *error;
            records.push_back(std::get<IdentityMutationRecord>(std::move(typed)));
        }
        return records;
    }
} // namespace javelin::jmap::identity
