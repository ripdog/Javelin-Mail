#include "jmap/sync/MutationJournal.h"

#include <QSqlError>
#include <QSqlQuery>

namespace javelin::jmap::sync
{

    namespace
    {

        [[nodiscard]] javelin::jmap::cache::DatabaseError queryError(const QString& operation,
                                                                     const QSqlQuery& query)
        {
            return {
                .code = javelin::jmap::cache::DatabaseErrorCode::QueryFailed,
                .message = operation + QStringLiteral(": ") + query.lastError().text(),
            };
        }

        [[nodiscard]] std::optional<std::string> optionalString(const QVariant& value)
        {
            return value.isNull() ? std::nullopt
                                  : std::optional<std::string>{value.toString().toStdString()};
        }

        [[nodiscard]] std::optional<MutationRecord> recordFromQuery(const QSqlQuery& query)
        {
            const auto status = mutationStatusFromString(query.value(6).toString().toStdString());
            if (!status.has_value())
            {
                return std::nullopt;
            }

            return MutationRecord{
                .mutationId = query.value(0).toString().toStdString(),
                .operationGroupId = optionalString(query.value(1)),
                .domain =
                    {
                        .accountId = query.value(2).toString().toStdString(),
                        .dataType = query.value(3).toString().toStdString(),
                    },
                .objectId = query.value(4).toString().toStdString(),
                .mutationKind = query.value(5).toString().toStdString(),
                .status = *status,
                .payloadJson = query.value(7).toString().toStdString(),
                .baseState = optionalString(query.value(8)),
                .acceptedState = optionalString(query.value(9)),
                .errorJson = optionalString(query.value(10)),
                .sequence = query.value(11).toLongLong(),
            };
        }

        [[nodiscard]] QString selectColumns()
        {
            return QStringLiteral(
                "mutation_id,operation_group_id,account_id,data_type,object_id,mutation_kind,"
                "status,payload_json,base_state,accepted_state,error_json,sequence");
        }

    } // namespace

    MutationJournalRepository::MutationJournalRepository(
        javelin::jmap::cache::DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MutationJournalRepository::put(const MutationRecord& record)
    {
        const javelin::jmap::cache::DatabaseWriteScope writeScope{m_connection};
        if (const auto error = m_connection.validate())
        {
            return error;
        }
        if (record.mutationId.empty() || record.domain.accountId.empty() ||
            record.domain.dataType.empty() || record.objectId.empty() ||
            record.mutationKind.empty() || record.payloadJson.empty())
        {
            return javelin::jmap::cache::DatabaseError{
                .code = javelin::jmap::cache::DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("A mutation journal record is incomplete."),
            };
        }

        QSqlQuery sequence{m_connection.database()};
        if (!sequence.exec(
                QStringLiteral("UPDATE mutation_journal_sequence SET next_value=next_value+1 "
                               "WHERE singleton=1 RETURNING next_value-1")) ||
            !sequence.next())
        {
            return queryError(QStringLiteral("Allocate mutation journal sequence"), sequence);
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "INSERT INTO mutation_journal (mutation_id,operation_group_id,account_id,data_type,"
            "object_id,mutation_kind,status,payload_json,base_state,accepted_state,error_json,"
            "sequence) VALUES (:mutation_id,:operation_group_id,:account_id,:data_type,:object_id,"
            ":mutation_kind,:status,:payload_json,:base_state,:accepted_state,:error_json,"
            ":sequence)"));
        query.bindValue(QStringLiteral(":mutation_id"), QString::fromStdString(record.mutationId));
        query.bindValue(QStringLiteral(":operation_group_id"),
                        record.operationGroupId.has_value()
                            ? QVariant{QString::fromStdString(*record.operationGroupId)}
                            : QVariant{});
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(record.domain.accountId));
        query.bindValue(QStringLiteral(":data_type"),
                        QString::fromStdString(record.domain.dataType));
        query.bindValue(QStringLiteral(":object_id"), QString::fromStdString(record.objectId));
        query.bindValue(QStringLiteral(":mutation_kind"),
                        QString::fromStdString(record.mutationKind));
        query.bindValue(QStringLiteral(":status"),
                        QString::fromStdString(std::string{toString(record.status)}));
        query.bindValue(QStringLiteral(":payload_json"),
                        QString::fromStdString(record.payloadJson));
        query.bindValue(QStringLiteral(":base_state"),
                        record.baseState.has_value()
                            ? QVariant{QString::fromStdString(*record.baseState)}
                            : QVariant{});
        query.bindValue(QStringLiteral(":accepted_state"),
                        record.acceptedState.has_value()
                            ? QVariant{QString::fromStdString(*record.acceptedState)}
                            : QVariant{});
        query.bindValue(QStringLiteral(":error_json"),
                        record.errorJson.has_value()
                            ? QVariant{QString::fromStdString(*record.errorJson)}
                            : QVariant{});
        query.bindValue(QStringLiteral(":sequence"), sequence.value(0));
        if (!query.exec())
        {
            return queryError(QStringLiteral("Upsert mutation journal record"), query);
        }

        return std::nullopt;
    }

    std::variant<std::optional<MutationRecord>, javelin::jmap::cache::DatabaseError>
    MutationJournalRepository::find(const std::string_view mutationId) const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral("SELECT %1 FROM mutation_journal WHERE mutation_id=:id")
                          .arg(selectColumns()));
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{mutationId}));
        if (!query.exec())
        {
            return queryError(QStringLiteral("Read mutation journal record"), query);
        }
        if (!query.next())
        {
            return std::optional<MutationRecord>{};
        }
        const auto record = recordFromQuery(query);
        if (!record.has_value())
        {
            return javelin::jmap::cache::DatabaseError{
                .code = javelin::jmap::cache::DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Mutation journal record has an invalid status."),
            };
        }
        return record;
    }

    std::variant<std::vector<MutationRecord>, javelin::jmap::cache::DatabaseError>
    MutationJournalRepository::listForObject(const ConsistencyDomain& domain,
                                             const std::string_view objectId) const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral("SELECT %1 FROM mutation_journal WHERE account_id=:account_id "
                                     "AND data_type=:data_type AND object_id=:object_id "
                                     "ORDER BY sequence")
                          .arg(selectColumns()));
        query.bindValue(QStringLiteral(":account_id"), QString::fromStdString(domain.accountId));
        query.bindValue(QStringLiteral(":data_type"), QString::fromStdString(domain.dataType));
        query.bindValue(QStringLiteral(":object_id"),
                        QString::fromStdString(std::string{objectId}));
        if (!query.exec())
        {
            return queryError(QStringLiteral("Read object mutation journal"), query);
        }

        std::vector<MutationRecord> records;
        while (query.next())
        {
            const auto record = recordFromQuery(query);
            if (!record.has_value())
            {
                return javelin::jmap::cache::DatabaseError{
                    .code = javelin::jmap::cache::DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("Mutation journal record has an invalid status."),
                };
            }
            records.push_back(*record);
        }
        return records;
    }

    std::variant<std::vector<MutationRecord>, javelin::jmap::cache::DatabaseError>
    MutationJournalRepository::listForOperationGroup(const ConsistencyDomain& domain,
                                                     const std::string_view operationGroupId) const
    {
        if (const auto error = m_connection.validate())
            return *error;

        QSqlQuery query{m_connection.database()};
        query.prepare(
            QStringLiteral("SELECT %1 FROM mutation_journal WHERE account_id=:account_id "
                           "AND data_type=:data_type AND operation_group_id=:operation_group_id "
                           "ORDER BY sequence")
                .arg(selectColumns()));
        query.bindValue(QStringLiteral(":account_id"), QString::fromStdString(domain.accountId));
        query.bindValue(QStringLiteral(":data_type"), QString::fromStdString(domain.dataType));
        query.bindValue(QStringLiteral(":operation_group_id"),
                        QString::fromStdString(std::string{operationGroupId}));
        if (!query.exec())
            return queryError(QStringLiteral("Read mutation journal operation group"), query);

        std::vector<MutationRecord> records;
        while (query.next())
        {
            const auto record = recordFromQuery(query);
            if (!record.has_value())
            {
                return javelin::jmap::cache::DatabaseError{
                    .code = javelin::jmap::cache::DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("Mutation journal record has an invalid status."),
                };
            }
            records.push_back(*record);
        }
        return records;
    }

    std::variant<std::vector<MutationRecord>, javelin::jmap::cache::DatabaseError>
    MutationJournalRepository::listPendingForOperationGroup(const ConsistencyDomain& domain,
                                                            const std::string_view operationGroupId,
                                                            const std::size_t objectLimit) const
    {
        if (const auto error = m_connection.validate())
            return *error;

        QSqlQuery query{m_connection.database()};
        query.prepare(
            QStringLiteral(
                "SELECT %1 FROM mutation_journal WHERE account_id=:account_id "
                "AND data_type=:data_type AND operation_group_id=:operation_group_id "
                "AND status='pending' AND object_id IN ("
                "SELECT object_id FROM mutation_journal WHERE account_id=:account_id "
                "AND data_type=:data_type AND operation_group_id=:operation_group_id "
                "AND status='pending' GROUP BY object_id ORDER BY MIN(sequence) LIMIT :limit) "
                "ORDER BY sequence")
                .arg(selectColumns()));
        query.bindValue(QStringLiteral(":account_id"), QString::fromStdString(domain.accountId));
        query.bindValue(QStringLiteral(":data_type"), QString::fromStdString(domain.dataType));
        query.bindValue(QStringLiteral(":operation_group_id"),
                        QString::fromStdString(std::string{operationGroupId}));
        query.bindValue(QStringLiteral(":limit"), static_cast<qulonglong>(objectLimit));
        if (!query.exec())
            return queryError(QStringLiteral("Read pending mutation journal operation batch"),
                              query);

        std::vector<MutationRecord> records;
        while (query.next())
        {
            const auto record = recordFromQuery(query);
            if (!record.has_value())
                return javelin::jmap::cache::DatabaseError{
                    .code = javelin::jmap::cache::DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("Mutation journal record has an invalid status."),
                };
            records.push_back(*record);
        }
        return records;
    }

    std::variant<std::vector<MutationRecord>, javelin::jmap::cache::DatabaseError>
    MutationJournalRepository::listByStatus(const ConsistencyDomain& domain,
                                            const MutationStatus status,
                                            const std::size_t limit) const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral("SELECT %1 FROM mutation_journal WHERE account_id=:account_id "
                                     "AND data_type=:data_type AND status=:status "
                                     "ORDER BY sequence LIMIT :limit")
                          .arg(selectColumns()));
        query.bindValue(QStringLiteral(":account_id"), QString::fromStdString(domain.accountId));
        query.bindValue(QStringLiteral(":data_type"), QString::fromStdString(domain.dataType));
        query.bindValue(QStringLiteral(":status"),
                        QString::fromStdString(std::string{toString(status)}));
        query.bindValue(QStringLiteral(":limit"), static_cast<qulonglong>(limit));
        if (!query.exec())
        {
            return queryError(QStringLiteral("Read mutation journal by status"), query);
        }

        std::vector<MutationRecord> records;
        while (query.next())
        {
            const auto record = recordFromQuery(query);
            if (!record.has_value())
            {
                return javelin::jmap::cache::DatabaseError{
                    .code = javelin::jmap::cache::DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("Mutation journal record has an invalid status."),
                };
            }
            records.push_back(*record);
        }
        return records;
    }

    std::variant<std::vector<MutationRecord>, javelin::jmap::cache::DatabaseError>
    MutationJournalRepository::listActive(const ConsistencyDomain& domain) const
    {
        if (const auto error = m_connection.validate())
            return *error;
        QSqlQuery query{m_connection.database()};
        query.prepare(
            QStringLiteral(
                "SELECT %1 FROM mutation_journal WHERE account_id=:account_id "
                "AND data_type=:data_type AND status IN ('pending','in_flight','unknown') "
                "ORDER BY sequence")
                .arg(selectColumns()));
        query.bindValue(QStringLiteral(":account_id"), QString::fromStdString(domain.accountId));
        query.bindValue(QStringLiteral(":data_type"), QString::fromStdString(domain.dataType));
        if (!query.exec())
            return queryError(QStringLiteral("Read active mutation journal"), query);
        std::vector<MutationRecord> records;
        while (query.next())
        {
            const auto record = recordFromQuery(query);
            if (!record.has_value())
                return javelin::jmap::cache::DatabaseError{
                    .code = javelin::jmap::cache::DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("Mutation journal record has an invalid status."),
                };
            records.push_back(*record);
        }
        return records;
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MutationJournalRepository::transition(const std::string_view mutationId,
                                          const MutationStatus status,
                                          const std::optional<std::string_view> acceptedState,
                                          const std::optional<std::string_view> errorJson)
    {
        const javelin::jmap::cache::DatabaseWriteScope writeScope{m_connection};
        if (const auto error = m_connection.validate())
        {
            return error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "UPDATE mutation_journal SET status=:status,accepted_state=:accepted_state,"
            "error_json=:error_json,updated_at=CURRENT_TIMESTAMP WHERE mutation_id=:mutation_id"));
        query.bindValue(QStringLiteral(":status"),
                        QString::fromStdString(std::string{toString(status)}));
        query.bindValue(QStringLiteral(":accepted_state"),
                        acceptedState.has_value()
                            ? QVariant{QString::fromStdString(std::string{*acceptedState})}
                            : QVariant{});
        query.bindValue(QStringLiteral(":error_json"),
                        errorJson.has_value()
                            ? QVariant{QString::fromStdString(std::string{*errorJson})}
                            : QVariant{});
        query.bindValue(QStringLiteral(":mutation_id"),
                        QString::fromStdString(std::string{mutationId}));
        if (!query.exec())
        {
            return queryError(QStringLiteral("Transition mutation journal record"), query);
        }
        if (query.numRowsAffected() != 1)
        {
            return javelin::jmap::cache::DatabaseError{
                .code = javelin::jmap::cache::DatabaseErrorCode::QueryFailed,
                .message =
                    QStringLiteral("Transition mutation journal record: record does not exist"),
            };
        }
        return std::nullopt;
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MutationJournalRepository::resetPending(const std::string_view mutationId,
                                            const std::string_view baseState)
    {
        const javelin::jmap::cache::DatabaseWriteScope writeScope{m_connection};
        if (const auto error = m_connection.validate())
            return error;

        QSqlQuery query{m_connection.database()};
        query.prepare(
            QStringLiteral("UPDATE mutation_journal SET status='pending',base_state=:base_state,"
                           "accepted_state=NULL,error_json=NULL,updated_at=CURRENT_TIMESTAMP WHERE "
                           "mutation_id=:mutation_id"));
        query.bindValue(QStringLiteral(":base_state"),
                        QString::fromStdString(std::string{baseState}));
        query.bindValue(QStringLiteral(":mutation_id"),
                        QString::fromStdString(std::string{mutationId}));
        if (!query.exec())
            return queryError(QStringLiteral("Reset mutation journal record"), query);
        if (query.numRowsAffected() != 1)
        {
            return javelin::jmap::cache::DatabaseError{
                .code = javelin::jmap::cache::DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Reset mutation journal record: record does not exist"),
            };
        }
        return std::nullopt;
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MutationJournalRepository::rebasePendingOperationGroup(const ConsistencyDomain& domain,
                                                           const std::string_view operationGroupId,
                                                           const std::string_view baseState)
    {
        const javelin::jmap::cache::DatabaseWriteScope writeScope{m_connection};
        if (const auto error = m_connection.validate())
            return error;

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "UPDATE mutation_journal SET base_state=:base_state,updated_at=CURRENT_TIMESTAMP "
            "WHERE account_id=:account_id AND data_type=:data_type "
            "AND operation_group_id=:operation_group_id AND status='pending'"));
        query.bindValue(QStringLiteral(":base_state"),
                        QString::fromStdString(std::string{baseState}));
        query.bindValue(QStringLiteral(":account_id"), QString::fromStdString(domain.accountId));
        query.bindValue(QStringLiteral(":data_type"), QString::fromStdString(domain.dataType));
        query.bindValue(QStringLiteral(":operation_group_id"),
                        QString::fromStdString(std::string{operationGroupId}));
        if (!query.exec())
            return queryError(QStringLiteral("Rebase pending mutation operation group"), query);
        return std::nullopt;
    }

    std::variant<std::size_t, javelin::jmap::cache::DatabaseError>
    MutationJournalRepository::recoverInFlight()
    {
        const javelin::jmap::cache::DatabaseWriteScope writeScope{m_connection};
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "UPDATE mutation_journal SET status='unknown',updated_at=CURRENT_TIMESTAMP "
            "WHERE status='in_flight'"));
        if (!query.exec())
        {
            return queryError(QStringLiteral("Recover in-flight mutations"), query);
        }
        return static_cast<std::size_t>(query.numRowsAffected());
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MutationJournalRepository::remove(const std::string_view mutationId)
    {
        const javelin::jmap::cache::DatabaseWriteScope writeScope{m_connection};
        if (const auto error = m_connection.validate())
        {
            return error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(
            QStringLiteral("DELETE FROM mutation_journal WHERE mutation_id=:mutation_id"));
        query.bindValue(QStringLiteral(":mutation_id"),
                        QString::fromStdString(std::string{mutationId}));
        if (!query.exec())
        {
            return queryError(QStringLiteral("Delete mutation journal record"), query);
        }
        return std::nullopt;
    }

    MutationProjectionTransaction::MutationProjectionTransaction(
        javelin::jmap::cache::DatabaseConnection& connection,
        javelin::jmap::cache::DatabaseTransaction transaction)
        : m_connection(&connection), m_transaction(std::move(transaction))
    {
    }

    std::variant<MutationProjectionTransaction, javelin::jmap::cache::DatabaseError>
    MutationProjectionTransaction::begin(javelin::jmap::cache::DatabaseConnection& connection,
                                         QString operation)
    {
        auto transactionResult =
            javelin::jmap::cache::DatabaseTransaction::begin(connection, std::move(operation));
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
        {
            return *error;
        }
        return MutationProjectionTransaction{
            connection,
            std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(transactionResult)),
        };
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MutationProjectionTransaction::append(const MutationRecord& record)
    {
        MutationJournalRepository journal{*m_connection};
        return journal.put(record);
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MutationProjectionTransaction::transition(const std::string_view mutationId,
                                              const MutationStatus status,
                                              const std::optional<std::string_view> acceptedState,
                                              const std::optional<std::string_view> errorJson)
    {
        MutationJournalRepository journal{*m_connection};
        return journal.transition(mutationId, status, acceptedState, errorJson);
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MutationProjectionTransaction::resetPending(const std::string_view mutationId,
                                                const std::string_view baseState)
    {
        MutationJournalRepository journal{*m_connection};
        return journal.resetPending(mutationId, baseState);
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MutationProjectionTransaction::rebasePendingOperationGroup(
        const ConsistencyDomain& domain, const std::string_view operationGroupId,
        const std::string_view baseState)
    {
        MutationJournalRepository journal{*m_connection};
        return journal.rebasePendingOperationGroup(domain, operationGroupId, baseState);
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MutationProjectionTransaction::remove(const std::string_view mutationId)
    {
        MutationJournalRepository journal{*m_connection};
        return journal.remove(mutationId);
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MutationProjectionTransaction::advance(const std::span<const ConsistencyDomain> domains)
    {
        ConsistencyDomainRepository consistency{*m_connection};
        for (const auto& domain : domains)
        {
            const auto generation = consistency.advanceMutation(domain);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&generation))
            {
                return *error;
            }
        }
        return std::nullopt;
    }

    javelin::jmap::cache::DatabaseTransaction& MutationProjectionTransaction::cacheTransaction()
    {
        return m_transaction;
    }

    std::optional<javelin::jmap::cache::DatabaseError> MutationProjectionTransaction::commit()
    {
        return m_transaction.commit();
    }

    std::string_view toString(const MutationStatus status)
    {
        switch (status)
        {
        case MutationStatus::Pending:
            return "pending";
        case MutationStatus::InFlight:
            return "in_flight";
        case MutationStatus::Accepted:
            return "accepted";
        case MutationStatus::Rejected:
            return "rejected";
        case MutationStatus::Unknown:
            return "unknown";
        }
        return "unknown";
    }

    std::optional<MutationStatus> mutationStatusFromString(const std::string_view value)
    {
        if (value == "pending")
            return MutationStatus::Pending;
        if (value == "in_flight")
            return MutationStatus::InFlight;
        if (value == "accepted")
            return MutationStatus::Accepted;
        if (value == "rejected")
            return MutationStatus::Rejected;
        if (value == "unknown")
            return MutationStatus::Unknown;
        return std::nullopt;
    }

    bool projectsOptimistically(const MutationStatus status)
    {
        return status == MutationStatus::Pending || status == MutationStatus::InFlight ||
               status == MutationStatus::Accepted || status == MutationStatus::Unknown;
    }

} // namespace javelin::jmap::sync
