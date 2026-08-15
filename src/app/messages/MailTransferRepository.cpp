#include "app/MailTransferRepository.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimeZone>

namespace javelin::app
{
    namespace
    {
        using javelin::jmap::cache::DatabaseError;
        using javelin::jmap::cache::DatabaseErrorCode;

        [[nodiscard]] DatabaseError queryError(QString operation, const QSqlQuery& query)
        {
            return {
                .code = DatabaseErrorCode::QueryFailed,
                .message = std::move(operation) + QStringLiteral(": ") + query.lastError().text(),
            };
        }

        [[nodiscard]] DatabaseError corruptRecord(QString detail)
        {
            return {
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Read mail transfer journal: ") + std::move(detail),
            };
        }

        [[nodiscard]] QVariant optionalText(const std::optional<std::string>& value)
        {
            return value.has_value() ? QVariant{QString::fromStdString(*value)} : QVariant{};
        }

        [[nodiscard]] QVariant optionalText(const std::optional<QString>& value)
        {
            return value.has_value() ? QVariant{*value} : QVariant{};
        }

        [[nodiscard]] std::optional<std::string> optionalString(const QVariant& value)
        {
            return value.isNull() ? std::nullopt : std::optional{value.toString().toStdString()};
        }

        [[nodiscard]] std::optional<QString> optionalQString(const QVariant& value)
        {
            return value.isNull() ? std::nullopt : std::optional{value.toString()};
        }

        [[nodiscard]] QDateTime timestamp(const QVariant& value)
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
            return parsed;
        }

        [[nodiscard]] QString serializeStrings(const std::vector<std::string>& values)
        {
            QJsonArray array;
            for (const auto& value : values)
                array.push_back(QString::fromStdString(value));
            return QString::fromUtf8(QJsonDocument{array}.toJson(QJsonDocument::Compact));
        }

        [[nodiscard]] std::optional<std::vector<std::string>> parseStrings(const QVariant& value)
        {
            if (value.isNull())
                return std::nullopt;
            QJsonParseError parseError;
            const auto document = QJsonDocument::fromJson(value.toString().toUtf8(), &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isArray())
                return std::nullopt;
            std::vector<std::string> result;
            const auto array = document.array();
            result.reserve(static_cast<std::size_t>(array.size()));
            for (const auto& entry : array)
            {
                if (!entry.isString())
                    return std::nullopt;
                result.push_back(entry.toString().toStdString());
            }
            return result;
        }

        [[nodiscard]] QString operationColumns()
        {
            return QStringLiteral(
                "operation_id,operation_group_id,source_account_id,source_mailbox_id,"
                "destination_account_id,destination_mailbox_id,operation,topology,status,title,"
                "last_error,created_at,updated_at");
        }

        [[nodiscard]] QString itemColumns()
        {
            return QStringLiteral(
                "item_id,operation_id,ordinal,source_email_id,source_blob_id,source_email_state,"
                "source_mailbox_ids_json,source_keywords_json,source_received_at,source_size,"
                "source_remove_mailbox_ids_json,source_destroy,raw_content_hash,"
                "destination_creation_id,destination_upload_blob_id,destination_pre_state,"
                "destination_email_id,destination_blob_id,destination_thread_id,destination_size,"
                "reused_existing,destination_prior_mailbox_ids_json,phase,last_error,created_at,"
                "updated_at");
        }

        [[nodiscard]] std::variant<MailTransferOperationRecord, DatabaseError>
        readOperation(const QSqlQuery& query)
        {
            const auto operation = mailTransferOperationFromString(query.value(6).toString());
            const auto topology = mailTransferTopologyFromString(query.value(7).toString());
            const auto status = mailTransferStatusFromString(query.value(8).toString());
            if (!operation.has_value() || !topology.has_value() || !status.has_value())
                return corruptRecord(QStringLiteral("invalid operation enum value"));

            return MailTransferOperationRecord{
                .operationId = query.value(0).toString().toStdString(),
                .operationGroupId = optionalString(query.value(1)),
                .sourceAccountId = query.value(2).toString().toStdString(),
                .sourceMailboxId = optionalString(query.value(3)),
                .destinationAccountId = query.value(4).toString().toStdString(),
                .destinationMailboxId = query.value(5).toString().toStdString(),
                .operation = *operation,
                .topology = *topology,
                .status = *status,
                .title = query.value(9).toString(),
                .lastError = optionalQString(query.value(10)),
                .createdAt = timestamp(query.value(11)),
                .updatedAt = timestamp(query.value(12)),
            };
        }

        [[nodiscard]] std::variant<MailTransferItemRecord, DatabaseError>
        readItem(const QSqlQuery& query)
        {
            const auto sourceMailboxIds = parseStrings(query.value(6));
            const auto sourceKeywords = parseStrings(query.value(7));
            const auto sourceRemoveMailboxIds = parseStrings(query.value(10));
            const auto phase = mailTransferItemPhaseFromString(query.value(22).toString());
            if (!sourceMailboxIds.has_value() || !sourceKeywords.has_value() ||
                !sourceRemoveMailboxIds.has_value() || !phase.has_value())
            {
                return corruptRecord(QStringLiteral("invalid item snapshot"));
            }

            std::optional<std::vector<std::string>> destinationPriorMailboxIds;
            if (!query.value(21).isNull())
            {
                destinationPriorMailboxIds = parseStrings(query.value(21));
                if (!destinationPriorMailboxIds.has_value())
                    return corruptRecord(QStringLiteral("invalid destination mailbox snapshot"));
            }

            return MailTransferItemRecord{
                .itemId = query.value(0).toString().toStdString(),
                .operationId = query.value(1).toString().toStdString(),
                .ordinal = query.value(2).toLongLong(),
                .sourceEmailId = query.value(3).toString().toStdString(),
                .sourceBlobId = query.value(4).toString().toStdString(),
                .sourceEmailState = optionalString(query.value(5)),
                .sourceMailboxIds = *sourceMailboxIds,
                .sourceKeywords = *sourceKeywords,
                .sourceReceivedAt = optionalString(query.value(8)),
                .sourceSize = query.value(9).toULongLong(),
                .sourceRemoveMailboxIds = *sourceRemoveMailboxIds,
                .sourceDestroy = query.value(11).toInt() != 0,
                .rawContentHash = optionalString(query.value(12)),
                .destinationCreationId = query.value(13).toString().toStdString(),
                .destinationUploadBlobId = optionalString(query.value(14)),
                .destinationPreState = optionalString(query.value(15)),
                .destinationEmailId = optionalString(query.value(16)),
                .destinationBlobId = optionalString(query.value(17)),
                .destinationThreadId = optionalString(query.value(18)),
                .destinationSize =
                    query.value(19).isNull()
                        ? std::nullopt
                        : std::optional<std::uint64_t>{query.value(19).toULongLong()},
                .reusedExisting = query.value(20).toInt() != 0,
                .destinationPriorMailboxIds = std::move(destinationPriorMailboxIds),
                .phase = *phase,
                .lastError = optionalQString(query.value(23)),
                .createdAt = timestamp(query.value(24)),
                .updatedAt = timestamp(query.value(25)),
            };
        }

    } // namespace

    QString toString(const MailTransferOperation operation)
    {
        return operation == MailTransferOperation::Copy ? QStringLiteral("copy")
                                                        : QStringLiteral("move");
    }

    std::optional<MailTransferOperation> mailTransferOperationFromString(const QStringView value)
    {
        if (value == QStringLiteral("copy"))
            return MailTransferOperation::Copy;
        if (value == QStringLiteral("move"))
            return MailTransferOperation::Move;
        return std::nullopt;
    }

    QString toString(const MailTransferTopology topology)
    {
        return topology == MailTransferTopology::SameSessionCopy
                   ? QStringLiteral("same_session_copy")
                   : QStringLiteral("cross_server_import");
    }

    std::optional<MailTransferTopology> mailTransferTopologyFromString(const QStringView value)
    {
        if (value == QStringLiteral("same_session_copy"))
            return MailTransferTopology::SameSessionCopy;
        if (value == QStringLiteral("cross_server_import"))
            return MailTransferTopology::CrossServerImport;
        return std::nullopt;
    }

    QString toString(const MailTransferStatus status)
    {
        switch (status)
        {
        case MailTransferStatus::Preparing:
            return QStringLiteral("preparing");
        case MailTransferStatus::Running:
            return QStringLiteral("running");
        case MailTransferStatus::WaitingForNetwork:
            return QStringLiteral("waiting_for_network");
        case MailTransferStatus::WaitingForAuth:
            return QStringLiteral("waiting_for_auth");
        case MailTransferStatus::WaitingForSpace:
            return QStringLiteral("waiting_for_space");
        case MailTransferStatus::BlockedUnknown:
            return QStringLiteral("blocked_unknown");
        case MailTransferStatus::Partial:
            return QStringLiteral("partial");
        case MailTransferStatus::Failed:
            return QStringLiteral("failed");
        case MailTransferStatus::Cancelled:
            return QStringLiteral("cancelled");
        case MailTransferStatus::Complete:
            return QStringLiteral("complete");
        }
        return QStringLiteral("failed");
    }

    std::optional<MailTransferStatus> mailTransferStatusFromString(const QStringView value)
    {
        if (value == QStringLiteral("preparing"))
            return MailTransferStatus::Preparing;
        if (value == QStringLiteral("running"))
            return MailTransferStatus::Running;
        if (value == QStringLiteral("waiting_for_network"))
            return MailTransferStatus::WaitingForNetwork;
        if (value == QStringLiteral("waiting_for_auth"))
            return MailTransferStatus::WaitingForAuth;
        if (value == QStringLiteral("waiting_for_space"))
            return MailTransferStatus::WaitingForSpace;
        if (value == QStringLiteral("blocked_unknown"))
            return MailTransferStatus::BlockedUnknown;
        if (value == QStringLiteral("partial"))
            return MailTransferStatus::Partial;
        if (value == QStringLiteral("failed"))
            return MailTransferStatus::Failed;
        if (value == QStringLiteral("cancelled"))
            return MailTransferStatus::Cancelled;
        if (value == QStringLiteral("complete"))
            return MailTransferStatus::Complete;
        return std::nullopt;
    }

    QString toString(const MailTransferItemPhase phase)
    {
        switch (phase)
        {
        case MailTransferItemPhase::Prepared:
            return QStringLiteral("prepared");
        case MailTransferItemPhase::AcquiringSource:
            return QStringLiteral("acquiring_source");
        case MailTransferItemPhase::SourceReady:
            return QStringLiteral("source_ready");
        case MailTransferItemPhase::Uploading:
            return QStringLiteral("uploading");
        case MailTransferItemPhase::Uploaded:
            return QStringLiteral("uploaded");
        case MailTransferItemPhase::CreatingDestination:
            return QStringLiteral("creating_destination");
        case MailTransferItemPhase::DestinationUnknown:
            return QStringLiteral("destination_unknown");
        case MailTransferItemPhase::DestinationConfirmed:
            return QStringLiteral("destination_confirmed");
        case MailTransferItemPhase::RemovingSource:
            return QStringLiteral("removing_source");
        case MailTransferItemPhase::SourceCleanupUnknown:
            return QStringLiteral("source_cleanup_unknown");
        case MailTransferItemPhase::PartialSourceRetained:
            return QStringLiteral("partial_source_retained");
        case MailTransferItemPhase::Failed:
            return QStringLiteral("failed");
        case MailTransferItemPhase::Cancelled:
            return QStringLiteral("cancelled");
        case MailTransferItemPhase::Complete:
            return QStringLiteral("complete");
        }
        return QStringLiteral("failed");
    }

    std::optional<MailTransferItemPhase> mailTransferItemPhaseFromString(const QStringView value)
    {
        if (value == QStringLiteral("prepared"))
            return MailTransferItemPhase::Prepared;
        if (value == QStringLiteral("acquiring_source"))
            return MailTransferItemPhase::AcquiringSource;
        if (value == QStringLiteral("source_ready"))
            return MailTransferItemPhase::SourceReady;
        if (value == QStringLiteral("uploading"))
            return MailTransferItemPhase::Uploading;
        if (value == QStringLiteral("uploaded"))
            return MailTransferItemPhase::Uploaded;
        if (value == QStringLiteral("creating_destination"))
            return MailTransferItemPhase::CreatingDestination;
        if (value == QStringLiteral("destination_unknown"))
            return MailTransferItemPhase::DestinationUnknown;
        if (value == QStringLiteral("destination_confirmed"))
            return MailTransferItemPhase::DestinationConfirmed;
        if (value == QStringLiteral("removing_source"))
            return MailTransferItemPhase::RemovingSource;
        if (value == QStringLiteral("source_cleanup_unknown"))
            return MailTransferItemPhase::SourceCleanupUnknown;
        if (value == QStringLiteral("partial_source_retained"))
            return MailTransferItemPhase::PartialSourceRetained;
        if (value == QStringLiteral("failed"))
            return MailTransferItemPhase::Failed;
        if (value == QStringLiteral("cancelled"))
            return MailTransferItemPhase::Cancelled;
        if (value == QStringLiteral("complete"))
            return MailTransferItemPhase::Complete;
        return std::nullopt;
    }

    MailTransferRepository::MailTransferRepository(
        javelin::jmap::cache::DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    std::optional<DatabaseError>
    MailTransferRepository::create(const MailTransferOperationRecord& operation,
                                   const std::vector<MailTransferItemRecord>& items)
    {
        auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
            m_connection, QStringLiteral("Create mail transfer journal"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
            return *error;
        auto transaction =
            std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(transactionResult));

        QSqlQuery insertOperation{m_connection.database()};
        insertOperation.prepare(QStringLiteral(
            "INSERT INTO mail_transfer_operations("
            "operation_id,operation_group_id,source_account_id,source_mailbox_id,"
            "destination_account_id,destination_mailbox_id,operation,topology,status,title,last_"
            "error"
            ") VALUES(:operation_id,:operation_group_id,:source_account_id,:source_mailbox_id,"
            ":destination_account_id,:destination_mailbox_id,:operation,:topology,:status,:title,"
            ":last_error)"));
        insertOperation.bindValue(QStringLiteral(":operation_id"),
                                  QString::fromStdString(operation.operationId));
        insertOperation.bindValue(QStringLiteral(":operation_group_id"),
                                  optionalText(operation.operationGroupId));
        insertOperation.bindValue(QStringLiteral(":source_account_id"),
                                  QString::fromStdString(operation.sourceAccountId));
        insertOperation.bindValue(QStringLiteral(":source_mailbox_id"),
                                  optionalText(operation.sourceMailboxId));
        insertOperation.bindValue(QStringLiteral(":destination_account_id"),
                                  QString::fromStdString(operation.destinationAccountId));
        insertOperation.bindValue(QStringLiteral(":destination_mailbox_id"),
                                  QString::fromStdString(operation.destinationMailboxId));
        insertOperation.bindValue(QStringLiteral(":operation"), toString(operation.operation));
        insertOperation.bindValue(QStringLiteral(":topology"), toString(operation.topology));
        insertOperation.bindValue(QStringLiteral(":status"), toString(operation.status));
        insertOperation.bindValue(QStringLiteral(":title"), operation.title);
        insertOperation.bindValue(QStringLiteral(":last_error"), optionalText(operation.lastError));
        if (!insertOperation.exec())
            return queryError(QStringLiteral("Insert mail transfer operation"), insertOperation);

        QSqlQuery insertItem{m_connection.database()};
        insertItem.prepare(QStringLiteral(
            "INSERT INTO mail_transfer_items("
            "item_id,operation_id,ordinal,source_email_id,source_blob_id,source_email_state,"
            "source_mailbox_ids_json,source_keywords_json,source_received_at,source_size,"
            "source_remove_mailbox_ids_json,source_destroy,raw_content_hash,destination_creation_"
            "id,"
            "destination_upload_blob_id,destination_pre_state,destination_email_id,"
            "destination_blob_id,destination_thread_id,destination_size,reused_existing,"
            "destination_prior_mailbox_ids_json,phase,last_error"
            ") VALUES(:item_id,:operation_id,:ordinal,:source_email_id,:source_blob_id,"
            ":source_email_state,:source_mailbox_ids_json,:source_keywords_json,:source_received_"
            "at,"
            ":source_size,:source_remove_mailbox_ids_json,:source_destroy,:raw_content_hash,"
            ":destination_creation_id,:destination_upload_blob_id,:destination_pre_state,"
            ":destination_email_id,:destination_blob_id,:destination_thread_id,:destination_size,"
            ":reused_existing,"
            ":destination_prior_mailbox_ids_json,:phase,:last_error)"));
        for (const auto& item : items)
        {
            if (item.operationId != operation.operationId)
            {
                return DatabaseError{
                    .code = DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("Mail transfer item belongs to another operation."),
                };
            }
            insertItem.bindValue(QStringLiteral(":item_id"), QString::fromStdString(item.itemId));
            insertItem.bindValue(QStringLiteral(":operation_id"),
                                 QString::fromStdString(item.operationId));
            insertItem.bindValue(QStringLiteral(":ordinal"), static_cast<qlonglong>(item.ordinal));
            insertItem.bindValue(QStringLiteral(":source_email_id"),
                                 QString::fromStdString(item.sourceEmailId));
            insertItem.bindValue(QStringLiteral(":source_blob_id"),
                                 QString::fromStdString(item.sourceBlobId));
            insertItem.bindValue(QStringLiteral(":source_email_state"),
                                 optionalText(item.sourceEmailState));
            insertItem.bindValue(QStringLiteral(":source_mailbox_ids_json"),
                                 serializeStrings(item.sourceMailboxIds));
            insertItem.bindValue(QStringLiteral(":source_keywords_json"),
                                 serializeStrings(item.sourceKeywords));
            insertItem.bindValue(QStringLiteral(":source_received_at"),
                                 optionalText(item.sourceReceivedAt));
            insertItem.bindValue(QStringLiteral(":source_size"),
                                 static_cast<qulonglong>(item.sourceSize));
            insertItem.bindValue(QStringLiteral(":source_remove_mailbox_ids_json"),
                                 serializeStrings(item.sourceRemoveMailboxIds));
            insertItem.bindValue(QStringLiteral(":source_destroy"), item.sourceDestroy ? 1 : 0);
            insertItem.bindValue(QStringLiteral(":raw_content_hash"),
                                 optionalText(item.rawContentHash));
            insertItem.bindValue(QStringLiteral(":destination_creation_id"),
                                 QString::fromStdString(item.destinationCreationId));
            insertItem.bindValue(QStringLiteral(":destination_upload_blob_id"),
                                 optionalText(item.destinationUploadBlobId));
            insertItem.bindValue(QStringLiteral(":destination_pre_state"),
                                 optionalText(item.destinationPreState));
            insertItem.bindValue(QStringLiteral(":destination_email_id"),
                                 optionalText(item.destinationEmailId));
            insertItem.bindValue(QStringLiteral(":destination_blob_id"),
                                 optionalText(item.destinationBlobId));
            insertItem.bindValue(QStringLiteral(":destination_thread_id"),
                                 optionalText(item.destinationThreadId));
            insertItem.bindValue(QStringLiteral(":destination_size"),
                                 item.destinationSize.has_value()
                                     ? QVariant{static_cast<qulonglong>(*item.destinationSize)}
                                     : QVariant{});
            insertItem.bindValue(QStringLiteral(":reused_existing"), item.reusedExisting ? 1 : 0);
            insertItem.bindValue(QStringLiteral(":destination_prior_mailbox_ids_json"),
                                 item.destinationPriorMailboxIds.has_value()
                                     ? QVariant{serializeStrings(*item.destinationPriorMailboxIds)}
                                     : QVariant{});
            insertItem.bindValue(QStringLiteral(":phase"), toString(item.phase));
            insertItem.bindValue(QStringLiteral(":last_error"), optionalText(item.lastError));
            if (!insertItem.exec())
                return queryError(QStringLiteral("Insert mail transfer item"), insertItem);
        }

        return transaction.commit();
    }

    std::variant<std::optional<MailTransferOperationRecord>, DatabaseError>
    MailTransferRepository::findOperation(const std::string_view operationId) const
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral("SELECT ") + operationColumns() +
                      QStringLiteral(" FROM mail_transfer_operations WHERE operation_id=:id"));
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{operationId}));
        if (!query.exec())
            return queryError(QStringLiteral("Find mail transfer operation"), query);
        if (!query.next())
            return std::optional<MailTransferOperationRecord>{};
        auto record = readOperation(query);
        if (const auto* error = std::get_if<DatabaseError>(&record))
            return *error;
        return std::optional{std::get<MailTransferOperationRecord>(std::move(record))};
    }

    std::variant<std::vector<MailTransferItemRecord>, DatabaseError>
    MailTransferRepository::listItems(const std::string_view operationId) const
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral("SELECT ") + itemColumns() +
                      QStringLiteral(" FROM mail_transfer_items WHERE operation_id=:id ORDER BY "
                                     "ordinal"));
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{operationId}));
        if (!query.exec())
            return queryError(QStringLiteral("List mail transfer items"), query);

        std::vector<MailTransferItemRecord> items;
        while (query.next())
        {
            auto record = readItem(query);
            if (const auto* error = std::get_if<DatabaseError>(&record))
                return *error;
            items.push_back(std::get<MailTransferItemRecord>(std::move(record)));
        }
        return items;
    }

    std::variant<std::vector<MailTransferOperationRecord>, DatabaseError>
    MailTransferRepository::listRecoverable() const
    {
        QSqlQuery query{m_connection.database()};
        if (!query.exec(QStringLiteral("SELECT ") + operationColumns() +
                        QStringLiteral(" FROM mail_transfer_operations WHERE status IN "
                                       "('preparing','running','waiting_for_network',"
                                       "'waiting_for_auth','waiting_for_space','blocked_unknown',"
                                       "'partial') ORDER BY created_at,operation_id")))
        {
            return queryError(QStringLiteral("List recoverable mail transfers"), query);
        }

        std::vector<MailTransferOperationRecord> operations;
        while (query.next())
        {
            auto record = readOperation(query);
            if (const auto* error = std::get_if<DatabaseError>(&record))
                return *error;
            operations.push_back(std::get<MailTransferOperationRecord>(std::move(record)));
        }
        return operations;
    }

    std::optional<DatabaseError>
    MailTransferRepository::updateStatus(const std::string_view operationId,
                                         const MailTransferStatus status,
                                         std::optional<QString> error)
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "UPDATE mail_transfer_operations SET status=:status,last_error=:last_error,"
            "updated_at=CURRENT_TIMESTAMP WHERE operation_id=:id"));
        query.bindValue(QStringLiteral(":status"), toString(status));
        query.bindValue(QStringLiteral(":last_error"), optionalText(error));
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{operationId}));
        if (!query.exec())
            return queryError(QStringLiteral("Update mail transfer status"), query);
        if (query.numRowsAffected() != 1)
            return corruptRecord(QStringLiteral("mail transfer operation is unavailable"));
        return std::nullopt;
    }

    std::variant<bool, DatabaseError> MailTransferRepository::transitionItem(
        const std::string_view itemId, const MailTransferItemPhase expected,
        const MailTransferItemPhase next, std::optional<QString> error)
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(
            QStringLiteral("UPDATE mail_transfer_items SET phase=:next,last_error=:last_error,"
                           "updated_at=CURRENT_TIMESTAMP WHERE item_id=:id AND phase=:expected"));
        query.bindValue(QStringLiteral(":next"), toString(next));
        query.bindValue(QStringLiteral(":last_error"), optionalText(error));
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{itemId}));
        query.bindValue(QStringLiteral(":expected"), toString(expected));
        if (!query.exec())
            return queryError(QStringLiteral("Transition mail transfer item"), query);
        return query.numRowsAffected() == 1;
    }

    std::variant<bool, DatabaseError>
    MailTransferRepository::markSourceReady(const std::string_view itemId,
                                            const MailTransferItemPhase expected,
                                            const std::string_view rawContentHash)
    {
        auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
            m_connection, QStringLiteral("Pin mail transfer source"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
            return *error;
        auto transaction =
            std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(transactionResult));

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "UPDATE mail_transfer_items SET raw_content_hash=:hash,phase='source_ready',"
            "last_error=NULL,updated_at=CURRENT_TIMESTAMP WHERE item_id=:id AND phase=:expected"));
        query.bindValue(QStringLiteral(":hash"),
                        QString::fromStdString(std::string{rawContentHash}));
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{itemId}));
        query.bindValue(QStringLiteral(":expected"), toString(expected));
        if (!query.exec())
            return queryError(QStringLiteral("Mark mail transfer source ready"), query);
        if (query.numRowsAffected() != 1)
        {
            transaction.rollback();
            return false;
        }

        QSqlQuery pin{m_connection.database()};
        pin.prepare(QStringLiteral(
            "INSERT INTO mail_vault_pins(owner_kind,owner_id,content_hash) "
            "VALUES('mail_transfer_item',:owner_id,:content_hash)"));
        pin.bindValue(QStringLiteral(":owner_id"), QString::fromStdString(std::string{itemId}));
        pin.bindValue(QStringLiteral(":content_hash"),
                      QString::fromStdString(std::string{rawContentHash}));
        if (!pin.exec())
            return queryError(QStringLiteral("Pin mail transfer source object"), pin);
        if (const auto error = transaction.commit())
            return *error;
        return true;
    }

    std::optional<DatabaseError>
    MailTransferRepository::releaseSourcePin(const std::string_view itemId)
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "DELETE FROM mail_vault_pins WHERE owner_kind='mail_transfer_item' AND owner_id=:id"));
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{itemId}));
        if (!query.exec())
            return queryError(QStringLiteral("Release mail transfer source pin"), query);
        return std::nullopt;
    }

    std::variant<bool, DatabaseError>
    MailTransferRepository::reassignSourcePin(const std::string_view itemId,
                                              const std::string_view ownerKind,
                                              const std::string_view ownerId)
    {
        auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
            m_connection, QStringLiteral("Reassign mail transfer source pin"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
            return *error;
        auto transaction =
            std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(transactionResult));

        QSqlQuery insert{m_connection.database()};
        insert.prepare(QStringLiteral(
            "INSERT INTO mail_vault_pins(owner_kind,owner_id,content_hash) "
            "SELECT :owner_kind,:owner_id,content_hash FROM mail_vault_pins WHERE "
            "owner_kind='mail_transfer_item' AND owner_id=:item_id"));
        insert.bindValue(QStringLiteral(":owner_kind"),
                         QString::fromStdString(std::string{ownerKind}));
        insert.bindValue(QStringLiteral(":owner_id"), QString::fromStdString(std::string{ownerId}));
        insert.bindValue(QStringLiteral(":item_id"), QString::fromStdString(std::string{itemId}));
        if (!insert.exec())
            return queryError(QStringLiteral("Create reassigned mail vault pin"), insert);
        if (insert.numRowsAffected() != 1)
        {
            transaction.rollback();
            return false;
        }

        QSqlQuery remove{m_connection.database()};
        remove.prepare(QStringLiteral(
            "DELETE FROM mail_vault_pins WHERE owner_kind='mail_transfer_item' AND owner_id=:id"));
        remove.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{itemId}));
        if (!remove.exec())
            return queryError(QStringLiteral("Remove original mail transfer source pin"), remove);
        if (const auto error = transaction.commit())
            return *error;
        return true;
    }

    std::variant<bool, DatabaseError>
    MailTransferRepository::markUploaded(const std::string_view itemId,
                                         const MailTransferItemPhase expected,
                                         const std::string_view destinationUploadBlobId)
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "UPDATE mail_transfer_items SET destination_upload_blob_id=:blob_id,phase='uploaded',"
            "last_error=NULL,updated_at=CURRENT_TIMESTAMP WHERE item_id=:id AND phase=:expected"));
        query.bindValue(QStringLiteral(":blob_id"),
                        QString::fromStdString(std::string{destinationUploadBlobId}));
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{itemId}));
        query.bindValue(QStringLiteral(":expected"), toString(expected));
        if (!query.exec())
            return queryError(QStringLiteral("Mark mail transfer upload complete"), query);
        return query.numRowsAffected() == 1;
    }

    std::variant<bool, DatabaseError>
    MailTransferRepository::markDestinationDispatching(const std::string_view itemId,
                                                       const MailTransferItemPhase expected,
                                                       const std::string_view destinationPreState)
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "UPDATE mail_transfer_items SET destination_pre_state=:state,"
            "phase='creating_destination',last_error=NULL,updated_at=CURRENT_TIMESTAMP WHERE "
            "item_id=:id AND phase=:expected"));
        query.bindValue(QStringLiteral(":state"),
                        QString::fromStdString(std::string{destinationPreState}));
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{itemId}));
        query.bindValue(QStringLiteral(":expected"), toString(expected));
        if (!query.exec())
            return queryError(QStringLiteral("Mark mail transfer destination dispatching"), query);
        return query.numRowsAffected() == 1;
    }

    std::variant<bool, DatabaseError> MailTransferRepository::markDestinationConfirmed(
        const std::string_view itemId, const MailTransferItemPhase expected,
        const MailTransferDestinationResult& destination)
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "UPDATE mail_transfer_items SET destination_email_id=:email_id,"
            "destination_blob_id=:blob_id,destination_thread_id=:thread_id,"
            "destination_size=:size,reused_existing=:reused,"
            "destination_prior_mailbox_ids_json=:prior_mailboxes,phase='destination_confirmed',"
            "last_error=NULL,updated_at=CURRENT_TIMESTAMP WHERE item_id=:id AND phase=:expected"));
        query.bindValue(QStringLiteral(":email_id"), QString::fromStdString(destination.emailId));
        query.bindValue(QStringLiteral(":blob_id"), optionalText(destination.blobId));
        query.bindValue(QStringLiteral(":thread_id"), optionalText(destination.threadId));
        query.bindValue(QStringLiteral(":size"),
                        destination.size.has_value()
                            ? QVariant{static_cast<qulonglong>(*destination.size)}
                            : QVariant{});
        query.bindValue(QStringLiteral(":reused"), destination.reusedExisting ? 1 : 0);
        query.bindValue(QStringLiteral(":prior_mailboxes"),
                        destination.priorMailboxIds.has_value()
                            ? QVariant{serializeStrings(*destination.priorMailboxIds)}
                            : QVariant{});
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{itemId}));
        query.bindValue(QStringLiteral(":expected"), toString(expected));
        if (!query.exec())
            return queryError(QStringLiteral("Confirm mail transfer destination"), query);
        return query.numRowsAffected() == 1;
    }

} // namespace javelin::app
