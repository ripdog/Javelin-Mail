#include "app/undo/HistoryRepository.h"

#include "app/undo/HistorySerialization.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QTimeZone>
#include <QVariant>

#include <utility>

namespace javelin::app::undo
{

    namespace
    {
        using javelin::jmap::cache::DatabaseError;
        using javelin::jmap::cache::DatabaseErrorCode;

        [[nodiscard]] DatabaseError queryError(const QString& operation, const QSqlQuery& query)
        {
            return javelin::jmap::cache::databaseError(operation, query.lastError());
        }

        [[nodiscard]] DatabaseError serializationError(const QString& operation,
                                                       const QString& message)
        {
            return {
                .code = DatabaseErrorCode::QueryFailed,
                .message = operation + QStringLiteral(": ") + message,
            };
        }

        [[nodiscard]] QString encodeDateTime(const QDateTime& value)
        {
            return value.toUTC().toString(Qt::ISODateWithMs);
        }

        [[nodiscard]] std::optional<QDateTime> decodeOptionalDateTime(const QVariant& value)
        {
            if (value.isNull())
                return std::nullopt;
            const auto parsed = QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
            if (!parsed.isValid())
                return std::nullopt;
            return parsed.toUTC();
        }

        [[nodiscard]] QDateTime decodeStoredDateTime(const QVariant& value)
        {
            auto parsed = QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
            if (!parsed.isValid())
                parsed = QDateTime::fromString(value.toString(), Qt::ISODate);
            if (!parsed.isValid())
            {
                parsed =
                    QDateTime::fromString(value.toString(), QStringLiteral("yyyy-MM-dd HH:mm:ss"));
                parsed.setTimeZone(QTimeZone::UTC);
            }
            return parsed.toUTC();
        }

        void bindOptional(QSqlQuery& query, const QString& name,
                          const std::optional<QString>& value)
        {
            query.bindValue(name, value.has_value() ? QVariant{*value} : QVariant{});
        }

        void bindOptionalDateTime(QSqlQuery& query, const QString& name,
                                  const std::optional<QDateTime>& value)
        {
            query.bindValue(name,
                            value.has_value() ? QVariant{encodeDateTime(*value)} : QVariant{});
        }

        [[nodiscard]] std::variant<HistoryEntry, DatabaseError> readEntry(const QSqlQuery& query)
        {
            const auto stack = historyStackFromString(query.value(1).toString());
            const auto domain = historyDomainFromString(query.value(3).toString());
            const auto status = historyEntryStatusFromString(query.value(8).toString());
            if (!stack.has_value() || !domain.has_value() || !status.has_value())
            {
                return serializationError(QStringLiteral("Read operation history"),
                                          QStringLiteral("Unknown persisted enum value"));
            }

            const auto commandKind = query.value(4).toString();
            const int payloadVersion = query.value(6).toInt();
            const auto payload =
                deserializeHistoryPayload(commandKind, payloadVersion, query.value(7).toString());
            if (const auto* error = std::get_if<HistorySerializationError>(&payload))
            {
                return serializationError(QStringLiteral("Read operation history"), error->message);
            }

            HistoryEntry entry{
                .entryId = query.value(0).toString(),
                .stack = *stack,
                .stackOrder = query.value(2).toLongLong(),
                .label = query.value(5).toString(),
                .domain = *domain,
                .commandKind = commandKind,
                .payloadVersion = payloadVersion,
                .payload = std::get<HistoryPayload>(payload),
                .status = *status,
                .operationGroupId = query.value(9).isNull()
                                        ? std::nullopt
                                        : std::optional<QString>{query.value(9).toString()},
                .expiresAt = decodeOptionalDateTime(query.value(10)),
                .explanation = query.value(11).isNull()
                                   ? std::nullopt
                                   : std::optional<QString>{query.value(11).toString()},
                .failureJson = query.value(12).isNull()
                                   ? std::nullopt
                                   : std::optional<QString>{query.value(12).toString()},
                .createdAt = decodeStoredDateTime(query.value(13)),
                .updatedAt = decodeStoredDateTime(query.value(14)),
            };
            return entry;
        }

        constexpr auto selectColumns =
            "entry_id,stack,stack_order,domain,command_kind,label,payload_version,payload_json,"
            "status,operation_group_id,expires_at,explanation,failure_json,created_at,updated_at";

    } // namespace

    HistoryRepository::HistoryRepository(javelin::jmap::cache::DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    std::variant<std::vector<HistoryEntry>, javelin::jmap::cache::DatabaseError>
    HistoryRepository::load() const
    {
        if (const auto error = m_connection.validate())
            return *error;

        QSqlQuery query{m_connection.database()};
        if (!query.exec(QStringLiteral("SELECT %1 FROM operation_history "
                                       "ORDER BY stack,stack_order DESC")
                            .arg(QString::fromLatin1(selectColumns))))
        {
            return queryError(QStringLiteral("Load operation history"), query);
        }

        std::vector<HistoryEntry> entries;
        while (query.next())
        {
            auto entry = readEntry(query);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&entry))
                return *error;
            entries.push_back(std::get<HistoryEntry>(std::move(entry)));
        }
        return entries;
    }

    std::variant<std::optional<HistoryEntry>, javelin::jmap::cache::DatabaseError>
    HistoryRepository::find(const QString& entryId) const
    {
        if (const auto error = m_connection.validate())
            return *error;

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral("SELECT %1 FROM operation_history WHERE entry_id=:entry_id")
                          .arg(QString::fromLatin1(selectColumns)));
        query.bindValue(QStringLiteral(":entry_id"), entryId);
        if (!query.exec())
            return queryError(QStringLiteral("Find operation history entry"), query);
        if (!query.next())
            return std::optional<HistoryEntry>{std::nullopt};

        auto entry = readEntry(query);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&entry))
            return *error;
        return std::optional<HistoryEntry>{std::get<HistoryEntry>(std::move(entry))};
    }

    std::variant<bool, javelin::jmap::cache::DatabaseError>
    HistoryRepository::hasMutationGroup(const QString& operationGroupId) const
    {
        if (const auto error = m_connection.validate())
            return *error;

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT EXISTS(SELECT 1 FROM mutation_journal WHERE operation_group_id=:group_id)"));
        query.bindValue(QStringLiteral(":group_id"), operationGroupId);
        if (!query.exec() || !query.next())
            return queryError(QStringLiteral("Find operation history mutation group"), query);
        return query.value(0).toBool();
    }

    std::variant<std::vector<javelin::jmap::sync::MutationRecord>,
                 javelin::jmap::cache::DatabaseError>
    HistoryRepository::mutationGroup(const std::string_view accountId,
                                     const std::string_view dataType,
                                     const QString& operationGroupId) const
    {
        javelin::jmap::sync::MutationJournalRepository journal{m_connection};
        return journal.listForOperationGroup(
            {.accountId = std::string{accountId}, .dataType = std::string{dataType}},
            operationGroupId.toStdString());
    }

    std::variant<std::int64_t, javelin::jmap::cache::DatabaseError> HistoryRepository::nextOrder()
    {
        QSqlQuery read{m_connection.database()};
        if (!read.exec(QStringLiteral(
                "SELECT next_value FROM operation_history_sequence WHERE singleton=1")) ||
            !read.next())
        {
            return queryError(QStringLiteral("Read operation history sequence"), read);
        }
        const auto value = read.value(0).toLongLong();

        QSqlQuery update{m_connection.database()};
        update.prepare(QStringLiteral("UPDATE operation_history_sequence "
                                      "SET next_value=:next_value WHERE singleton=1"));
        update.bindValue(QStringLiteral(":next_value"), static_cast<qlonglong>(value + 1));
        if (!update.exec())
            return queryError(QStringLiteral("Advance operation history sequence"), update);
        return value;
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    HistoryRepository::insert(const HistoryEntry& entry)
    {
        const auto serialized = serializeHistoryPayload(entry.payload);
        if (const auto* error = std::get_if<HistorySerializationError>(&serialized))
            return serializationError(QStringLiteral("Insert operation history"), error->message);
        const auto& payload = std::get<SerializedHistoryPayload>(serialized);

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "INSERT INTO operation_history("
            "entry_id,stack,stack_order,domain,command_kind,label,payload_version,payload_json,"
            "status,operation_group_id,expires_at,explanation,failure_json,created_at,updated_at"
            ") VALUES("
            ":entry_id,:stack,:stack_order,:domain,:command_kind,:label,:payload_version,"
            ":payload_json,:status,:operation_group_id,:expires_at,:explanation,:failure_json,"
            ":created_at,:updated_at)"));
        query.bindValue(QStringLiteral(":entry_id"), entry.entryId);
        query.bindValue(QStringLiteral(":stack"), toString(entry.stack));
        query.bindValue(QStringLiteral(":stack_order"), static_cast<qlonglong>(entry.stackOrder));
        query.bindValue(QStringLiteral(":domain"), toString(entry.domain));
        query.bindValue(QStringLiteral(":command_kind"), entry.commandKind);
        query.bindValue(QStringLiteral(":label"), entry.label);
        query.bindValue(QStringLiteral(":payload_version"), payload.version);
        query.bindValue(QStringLiteral(":payload_json"), payload.json);
        query.bindValue(QStringLiteral(":status"), toString(entry.status));
        bindOptional(query, QStringLiteral(":operation_group_id"), entry.operationGroupId);
        bindOptionalDateTime(query, QStringLiteral(":expires_at"), entry.expiresAt);
        bindOptional(query, QStringLiteral(":explanation"), entry.explanation);
        bindOptional(query, QStringLiteral(":failure_json"), entry.failureJson);
        query.bindValue(QStringLiteral(":created_at"), encodeDateTime(entry.createdAt));
        query.bindValue(QStringLiteral(":updated_at"), encodeDateTime(entry.updatedAt));
        if (!query.exec())
            return queryError(QStringLiteral("Insert operation history"), query);
        return std::nullopt;
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    HistoryRepository::updateStored(const HistoryEntry& entry)
    {
        const auto serialized = serializeHistoryPayload(entry.payload);
        if (const auto* error = std::get_if<HistorySerializationError>(&serialized))
            return serializationError(QStringLiteral("Update operation history"), error->message);
        const auto& payload = std::get<SerializedHistoryPayload>(serialized);

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "UPDATE operation_history SET "
            "stack=:stack,stack_order=:stack_order,domain=:domain,command_kind=:command_kind,"
            "label=:label,payload_version=:payload_version,payload_json=:payload_json,"
            "status=:status,operation_group_id=:operation_group_id,expires_at=:expires_at,"
            "explanation=:explanation,failure_json=:failure_json,updated_at=:updated_at "
            "WHERE entry_id=:entry_id"));
        query.bindValue(QStringLiteral(":entry_id"), entry.entryId);
        query.bindValue(QStringLiteral(":stack"), toString(entry.stack));
        query.bindValue(QStringLiteral(":stack_order"), static_cast<qlonglong>(entry.stackOrder));
        query.bindValue(QStringLiteral(":domain"), toString(entry.domain));
        query.bindValue(QStringLiteral(":command_kind"), entry.commandKind);
        query.bindValue(QStringLiteral(":label"), entry.label);
        query.bindValue(QStringLiteral(":payload_version"), payload.version);
        query.bindValue(QStringLiteral(":payload_json"), payload.json);
        query.bindValue(QStringLiteral(":status"), toString(entry.status));
        bindOptional(query, QStringLiteral(":operation_group_id"), entry.operationGroupId);
        bindOptionalDateTime(query, QStringLiteral(":expires_at"), entry.expiresAt);
        bindOptional(query, QStringLiteral(":explanation"), entry.explanation);
        bindOptional(query, QStringLiteral(":failure_json"), entry.failureJson);
        query.bindValue(QStringLiteral(":updated_at"), encodeDateTime(entry.updatedAt));
        if (!query.exec())
            return queryError(QStringLiteral("Update operation history"), query);
        if (query.numRowsAffected() != 1)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Update operation history: entry does not exist"),
            };
        }
        return std::nullopt;
    }

    std::variant<HistoryEntry, javelin::jmap::cache::DatabaseError>
    HistoryRepository::insertPreparing(HistoryEntry entry)
    {
        auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
            m_connection, QStringLiteral("Insert preparing history entry"));
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
            return *error;
        auto transaction =
            std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(transactionResult));

        const auto order = nextOrder();
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&order))
            return *error;
        entry.stackOrder = std::get<std::int64_t>(order);
        entry.stack = HistoryStack::Undo;
        entry.status = HistoryEntryStatus::Preparing;
        entry.commandKind = payloadCommandKind(entry.payload);
        const auto now = QDateTime::currentDateTimeUtc();
        entry.createdAt = now;
        entry.updatedAt = now;
        if (const auto error = insert(entry))
            return *error;
        if (const auto error = transaction.commit())
            return *error;
        return entry;
    }

    std::variant<HistoryEntry, javelin::jmap::cache::DatabaseError>
    HistoryRepository::pushUndoClearingRedo(HistoryEntry entry)
    {
        auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
            m_connection, QStringLiteral("Push operation history"));
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
            return *error;
        auto transaction =
            std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(transactionResult));

        QSqlQuery clear{m_connection.database()};
        if (!clear.exec(QStringLiteral("DELETE FROM operation_history WHERE stack='redo'")))
            return queryError(QStringLiteral("Clear redo history"), clear);
        const auto order = nextOrder();
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&order))
            return *error;
        entry.stack = HistoryStack::Undo;
        entry.stackOrder = std::get<std::int64_t>(order);
        entry.commandKind = payloadCommandKind(entry.payload);
        const auto now = QDateTime::currentDateTimeUtc();
        entry.createdAt = now;
        entry.updatedAt = now;
        if (const auto error = insert(entry))
            return *error;
        if (const auto error = transaction.commit())
            return *error;
        return entry;
    }

    std::variant<HistoryEntry, javelin::jmap::cache::DatabaseError>
    HistoryRepository::markPreparedReady(HistoryEntry entry)
    {
        return markPrepared(std::move(entry), HistoryEntryStatus::Ready);
    }

    std::variant<HistoryEntry, javelin::jmap::cache::DatabaseError>
    HistoryRepository::markPreparedImpossible(HistoryEntry entry)
    {
        return markPrepared(std::move(entry), HistoryEntryStatus::Impossible);
    }

    std::variant<HistoryEntry, javelin::jmap::cache::DatabaseError>
    HistoryRepository::markPrepared(HistoryEntry entry, const HistoryEntryStatus status)
    {
        auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
            m_connection, QStringLiteral("Commit preparing history entry"));
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
            return *error;
        auto transaction =
            std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(transactionResult));

        QSqlQuery clear{m_connection.database()};
        if (!clear.exec(QStringLiteral("DELETE FROM operation_history WHERE stack='redo'")))
            return queryError(QStringLiteral("Clear redo history"), clear);
        const auto order = nextOrder();
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&order))
            return *error;
        entry.stack = HistoryStack::Undo;
        entry.stackOrder = std::get<std::int64_t>(order);
        entry.status = status;
        entry.updatedAt = QDateTime::currentDateTimeUtc();
        if (const auto error = updateStored(entry))
            return *error;
        if (const auto error = transaction.commit())
            return *error;
        return entry;
    }

    std::variant<HistoryEntry, javelin::jmap::cache::DatabaseError>
    HistoryRepository::move(const HistoryEntry& source, const HistoryStack destination,
                            const HistoryEntryStatus status)
    {
        auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
            m_connection, QStringLiteral("Move operation history entry"));
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
            return *error;
        auto transaction =
            std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(transactionResult));
        const auto order = nextOrder();
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&order))
            return *error;

        auto entry = source;
        entry.stack = destination;
        entry.stackOrder = std::get<std::int64_t>(order);
        entry.status = status;
        entry.updatedAt = QDateTime::currentDateTimeUtc();
        if (const auto error = updateStored(entry))
            return *error;
        if (const auto error = transaction.commit())
            return *error;
        return entry;
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    HistoryRepository::update(const HistoryEntry& source)
    {
        auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
            m_connection, QStringLiteral("Update operation history entry"));
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
            return *error;
        auto transaction =
            std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(transactionResult));
        auto entry = source;
        entry.updatedAt = QDateTime::currentDateTimeUtc();
        if (const auto error = updateStored(entry))
            return error;
        return transaction.commit();
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    HistoryRepository::remove(const QString& entryId)
    {
        const javelin::jmap::cache::DatabaseWriteScope writeScope{m_connection};
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral("DELETE FROM operation_history WHERE entry_id=:entry_id"));
        query.bindValue(QStringLiteral(":entry_id"), entryId);
        if (!query.exec())
            return queryError(QStringLiteral("Remove operation history entry"), query);
        return std::nullopt;
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    HistoryRepository::removeAndClearRedo(const QString& entryId)
    {
        auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
            m_connection, QStringLiteral("Remove branched history entry"));
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
            return *error;
        auto transaction =
            std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(transactionResult));
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "DELETE FROM operation_history WHERE entry_id=:entry_id OR stack='redo'"));
        query.bindValue(QStringLiteral(":entry_id"), entryId);
        if (!query.exec())
            return queryError(QStringLiteral("Remove history entry and Redo branch"), query);
        return transaction.commit();
    }

    std::optional<javelin::jmap::cache::DatabaseError> HistoryRepository::clearRedo()
    {
        const javelin::jmap::cache::DatabaseWriteScope writeScope{m_connection};
        QSqlQuery query{m_connection.database()};
        if (!query.exec(QStringLiteral("DELETE FROM operation_history WHERE stack='redo'")))
            return queryError(QStringLiteral("Clear redo history"), query);
        return std::nullopt;
    }

} // namespace javelin::app::undo
