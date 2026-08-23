#include "app/MailImportRepository.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>

namespace javelin::app
{
    namespace
    {
        using javelin::jmap::cache::DatabaseError;
        using javelin::jmap::cache::DatabaseErrorCode;

        [[nodiscard]] DatabaseError queryError(QString operation, const QSqlQuery& query)
        {
            return {.code = DatabaseErrorCode::QueryFailed,
                    .message =
                        std::move(operation) + QStringLiteral(": ") + query.lastError().text()};
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

        [[nodiscard]] QVariant optionalInteger(const std::optional<std::uint64_t>& value)
        {
            return value.has_value() ? QVariant{static_cast<qulonglong>(*value)} : QVariant{};
        }

        [[nodiscard]] QString statusName(const MailImportStatus status)
        {
            switch (status)
            {
            case MailImportStatus::Preparing:
                return QStringLiteral("preparing");
            case MailImportStatus::Running:
                return QStringLiteral("running");
            case MailImportStatus::WaitingForNetwork:
                return QStringLiteral("waiting_network");
            case MailImportStatus::WaitingForAuth:
                return QStringLiteral("waiting_auth");
            case MailImportStatus::WaitingForSpace:
                return QStringLiteral("waiting_space");
            case MailImportStatus::BlockedUnknown:
                return QStringLiteral("blocked_unknown");
            case MailImportStatus::Partial:
                return QStringLiteral("partial");
            case MailImportStatus::Failed:
                return QStringLiteral("failed");
            case MailImportStatus::Complete:
                return QStringLiteral("complete");
            }
            return QStringLiteral("failed");
        }

        [[nodiscard]] MailImportStatus statusFromName(const QStringView value)
        {
            if (value == u"preparing")
                return MailImportStatus::Preparing;
            if (value == u"running")
                return MailImportStatus::Running;
            if (value == u"waiting_network")
                return MailImportStatus::WaitingForNetwork;
            if (value == u"waiting_auth")
                return MailImportStatus::WaitingForAuth;
            if (value == u"waiting_space")
                return MailImportStatus::WaitingForSpace;
            if (value == u"blocked_unknown")
                return MailImportStatus::BlockedUnknown;
            if (value == u"partial")
                return MailImportStatus::Partial;
            if (value == u"complete")
                return MailImportStatus::Complete;
            return MailImportStatus::Failed;
        }

        [[nodiscard]] QString mailboxPhaseName(const MailImportMailboxPhase phase)
        {
            switch (phase)
            {
            case MailImportMailboxPhase::Pending:
                return QStringLiteral("pending");
            case MailImportMailboxPhase::Reused:
                return QStringLiteral("reused");
            case MailImportMailboxPhase::Created:
                return QStringLiteral("created");
            case MailImportMailboxPhase::Failed:
                return QStringLiteral("failed");
            }
            return QStringLiteral("failed");
        }

        [[nodiscard]] MailImportMailboxPhase mailboxPhaseFromName(const QStringView value)
        {
            if (value == u"pending")
                return MailImportMailboxPhase::Pending;
            if (value == u"reused")
                return MailImportMailboxPhase::Reused;
            if (value == u"created")
                return MailImportMailboxPhase::Created;
            return MailImportMailboxPhase::Failed;
        }

        [[nodiscard]] QString itemPhaseName(const MailImportItemPhase phase)
        {
            switch (phase)
            {
            case MailImportItemPhase::Pending:
                return QStringLiteral("pending");
            case MailImportItemPhase::Uploading:
                return QStringLiteral("uploading");
            case MailImportItemPhase::Uploaded:
                return QStringLiteral("uploaded");
            case MailImportItemPhase::Creating:
                return QStringLiteral("creating");
            case MailImportItemPhase::Unknown:
                return QStringLiteral("unknown");
            case MailImportItemPhase::Created:
                return QStringLiteral("created");
            case MailImportItemPhase::Reused:
                return QStringLiteral("reused");
            case MailImportItemPhase::NoDestination:
                return QStringLiteral("no_destination");
            case MailImportItemPhase::Failed:
                return QStringLiteral("failed");
            }
            return QStringLiteral("failed");
        }

        [[nodiscard]] MailImportItemPhase itemPhaseFromName(const QStringView value)
        {
            if (value == u"pending")
                return MailImportItemPhase::Pending;
            if (value == u"uploading")
                return MailImportItemPhase::Uploading;
            if (value == u"uploaded")
                return MailImportItemPhase::Uploaded;
            if (value == u"creating")
                return MailImportItemPhase::Creating;
            if (value == u"unknown")
                return MailImportItemPhase::Unknown;
            if (value == u"created")
                return MailImportItemPhase::Created;
            if (value == u"reused")
                return MailImportItemPhase::Reused;
            if (value == u"no_destination")
                return MailImportItemPhase::NoDestination;
            return MailImportItemPhase::Failed;
        }

        [[nodiscard]] QString sourceKindName(const MailImportFileKind kind)
        {
            return kind == MailImportFileKind::Mbox ? QStringLiteral("mbox")
                                                    : QStringLiteral("eml");
        }

        [[nodiscard]] MailImportFileKind sourceKindFromName(const QStringView value)
        {
            return value == u"mbox" ? MailImportFileKind::Mbox : MailImportFileKind::Eml;
        }

        [[nodiscard]] QString serializePaths(const std::vector<QString>& paths)
        {
            QJsonArray values;
            for (const auto& path : paths)
                values.push_back(path);
            return QString::fromUtf8(QJsonDocument{values}.toJson(QJsonDocument::Compact));
        }

        [[nodiscard]] std::vector<QString> deserializePaths(const QString& value)
        {
            std::vector<QString> paths;
            const auto document = QJsonDocument::fromJson(value.toUtf8());
            if (!document.isArray())
                return paths;
            const auto values = document.array();
            paths.reserve(static_cast<std::size_t>(values.size()));
            for (const auto& item : values)
                if (item.isString())
                    paths.push_back(item.toString());
            return paths;
        }

        [[nodiscard]] MailImportOperationRecord operationFromQuery(const QSqlQuery& query)
        {
            return {
                .operationId = query.value(0).toString().toStdString(),
                .accountId = query.value(1).toString().toStdString(),
                .mailboxId = optionalString(query.value(2)),
                .sourcePaths = deserializePaths(query.value(3).toString()),
                .recreateHierarchy = query.value(4).toInt() != 0,
                .status = statusFromName(query.value(5).toString()),
                .scanSealed = query.value(6).toInt() != 0,
                .title = query.value(7).toString(),
                .createdAt = query.value(8).toString(),
                .lastError = optionalQString(query.value(9)),
            };
        }

        [[nodiscard]] MailImportMailboxRecord mailboxFromQuery(const QSqlQuery& query)
        {
            return {
                .ordinal = query.value(0).toULongLong(),
                .relativePath = query.value(1).toString(),
                .parentRelativePath = optionalQString(query.value(2)),
                .displayName = query.value(3).toString(),
                .phase = mailboxPhaseFromName(query.value(4).toString()),
                .resolvedMailboxId = optionalString(query.value(5)),
                .lastError = optionalQString(query.value(6)),
            };
        }

        [[nodiscard]] MailImportItemRecord itemFromQuery(const QSqlQuery& query)
        {
            return {
                .itemId = query.value(0).toString().toStdString(),
                .ordinal = query.value(1).toULongLong(),
                .sourcePath = query.value(2).toString(),
                .sourceRelativePath = optionalQString(query.value(3)),
                .sourceKind = sourceKindFromName(query.value(4).toString()),
                .contentOffset = query.value(5).isNull()
                                     ? std::nullopt
                                     : std::optional<std::uint64_t>{query.value(5).toULongLong()},
                .contentEnd = query.value(6).isNull()
                                  ? std::nullopt
                                  : std::optional<std::uint64_t>{query.value(6).toULongLong()},
                .decodedSize = query.value(7).toULongLong(),
                .sourceFingerprint =
                    {
                        .canonicalPath = query.value(8).toString(),
                        .size = query.value(9).toULongLong(),
                        .lastModifiedMs = query.value(10).toLongLong(),
                    },
                .receivedAt = optionalString(query.value(11)),
                .destinationRelativePath = optionalQString(query.value(12)),
                .resolvedMailboxId = optionalString(query.value(13)),
                .phase = itemPhaseFromName(query.value(14).toString()),
                .sourceSha256 = optionalString(query.value(15)),
                .uploadedBlobId = optionalString(query.value(16)),
                .preState = optionalString(query.value(17)),
                .createdEmailId = optionalString(query.value(18)),
                .existingEmailId = optionalString(query.value(19)),
                .lastError = optionalQString(query.value(20)),
            };
        }

        constexpr auto itemColumns =
            "item_id,ordinal,source_path,source_relative_path,source_kind,content_offset,content_"
            "end,"
            "decoded_size,source_canonical_path,source_size,source_mtime_ms,received_at,"
            "destination_relative_path,resolved_mailbox_id,phase,source_sha256,uploaded_blob_id,"
            "pre_state,created_email_id,existing_email_id,last_error";
    } // namespace

    MailImportRepository::MailImportRepository(javelin::jmap::cache::DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    std::optional<DatabaseError>
    MailImportRepository::createOperation(const MailImportOperationRecord& operation)
    {
        const javelin::jmap::cache::DatabaseWriteScope writeScope{m_connection};
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "INSERT INTO mail_import_operations(operation_id,account_id,mailbox_id,"
            "source_paths_json,recreate_hierarchy,status,scan_sealed,title,created_at,last_error) "
            "VALUES(:id,:account,:mailbox,:paths,:hierarchy,:status,:sealed,:title,:created,:"
            "error)"));
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(operation.operationId));
        query.bindValue(QStringLiteral(":account"), QString::fromStdString(operation.accountId));
        query.bindValue(QStringLiteral(":mailbox"), optionalText(operation.mailboxId));
        query.bindValue(QStringLiteral(":paths"), serializePaths(operation.sourcePaths));
        query.bindValue(QStringLiteral(":hierarchy"), operation.recreateHierarchy ? 1 : 0);
        query.bindValue(QStringLiteral(":status"), statusName(operation.status));
        query.bindValue(QStringLiteral(":sealed"), operation.scanSealed ? 1 : 0);
        query.bindValue(QStringLiteral(":title"), operation.title);
        query.bindValue(QStringLiteral(":created"), operation.createdAt);
        query.bindValue(QStringLiteral(":error"), optionalText(operation.lastError));
        if (!query.exec())
            return queryError(QStringLiteral("Create mail import"), query);
        return std::nullopt;
    }

    std::variant<std::optional<MailImportOperationRecord>, DatabaseError>
    MailImportRepository::findOperation(const std::string_view operationId) const
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT operation_id,account_id,mailbox_id,source_paths_json,recreate_hierarchy,status,"
            "scan_sealed,title,created_at,last_error FROM mail_import_operations WHERE "
            "operation_id=:id"));
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{operationId}));
        if (!query.exec())
            return queryError(QStringLiteral("Read mail import"), query);
        if (!query.next())
            return std::optional<MailImportOperationRecord>{};
        return std::optional<MailImportOperationRecord>{operationFromQuery(query)};
    }

    std::variant<std::vector<MailImportOperationRecord>, DatabaseError>
    MailImportRepository::listRecoverable() const
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT operation_id,account_id,mailbox_id,source_paths_json,recreate_hierarchy,status,"
            "scan_sealed,title,created_at,last_error FROM mail_import_operations WHERE status IN "
            "('preparing','running','waiting_network','waiting_auth','waiting_space',"
            "'blocked_unknown') ORDER BY created_at,operation_id"));
        if (!query.exec())
            return queryError(QStringLiteral("List recoverable mail imports"), query);
        std::vector<MailImportOperationRecord> result;
        while (query.next())
            result.push_back(operationFromQuery(query));
        return result;
    }

    std::optional<DatabaseError> MailImportRepository::setStatus(const std::string_view operationId,
                                                                 const MailImportStatus status,
                                                                 std::optional<QString> lastError)
    {
        const javelin::jmap::cache::DatabaseWriteScope writeScope{m_connection};
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "UPDATE mail_import_operations SET status=:status,last_error=:error WHERE "
            "operation_id=:id"));
        query.bindValue(QStringLiteral(":status"), statusName(status));
        query.bindValue(QStringLiteral(":error"), optionalText(lastError));
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{operationId}));
        if (!query.exec())
            return queryError(QStringLiteral("Update mail import status"), query);
        return std::nullopt;
    }

    std::optional<DatabaseError>
    MailImportRepository::replaceScan(const std::string_view operationId,
                                      const std::vector<MailImportMailboxRecord>& mailboxes,
                                      const std::vector<MailImportItemRecord>& items)
    {
        auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
            m_connection, QStringLiteral("Replace mail import scan"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
            return *error;
        auto transaction =
            std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(transactionResult));

        QSqlQuery clearMailboxes{m_connection.database()};
        clearMailboxes.prepare(
            QStringLiteral("DELETE FROM mail_import_mailboxes WHERE operation_id=:id"));
        clearMailboxes.bindValue(QStringLiteral(":id"),
                                 QString::fromStdString(std::string{operationId}));
        if (!clearMailboxes.exec())
            return queryError(QStringLiteral("Clear mail import mailbox scan"), clearMailboxes);
        QSqlQuery clearItems{m_connection.database()};
        clearItems.prepare(QStringLiteral("DELETE FROM mail_import_items WHERE operation_id=:id"));
        clearItems.bindValue(QStringLiteral(":id"),
                             QString::fromStdString(std::string{operationId}));
        if (!clearItems.exec())
            return queryError(QStringLiteral("Clear mail import item scan"), clearItems);

        QSqlQuery mailboxInsert{m_connection.database()};
        mailboxInsert.prepare(QStringLiteral(
            "INSERT INTO mail_import_mailboxes(operation_id,ordinal,relative_path,"
            "parent_relative_path,display_name,phase,resolved_mailbox_id,last_error) VALUES("
            ":operation,:ordinal,:path,:parent,:name,:phase,:mailbox,:error)"));
        for (const auto& mailbox : mailboxes)
        {
            mailboxInsert.bindValue(QStringLiteral(":operation"),
                                    QString::fromStdString(std::string{operationId}));
            mailboxInsert.bindValue(QStringLiteral(":ordinal"),
                                    static_cast<qulonglong>(mailbox.ordinal));
            mailboxInsert.bindValue(QStringLiteral(":path"), mailbox.relativePath);
            mailboxInsert.bindValue(QStringLiteral(":parent"),
                                    optionalText(mailbox.parentRelativePath));
            mailboxInsert.bindValue(QStringLiteral(":name"), mailbox.displayName);
            mailboxInsert.bindValue(QStringLiteral(":phase"), mailboxPhaseName(mailbox.phase));
            mailboxInsert.bindValue(QStringLiteral(":mailbox"),
                                    optionalText(mailbox.resolvedMailboxId));
            mailboxInsert.bindValue(QStringLiteral(":error"), optionalText(mailbox.lastError));
            if (!mailboxInsert.exec())
                return queryError(QStringLiteral("Store mail import mailbox scan"), mailboxInsert);
        }

        QSqlQuery itemInsert{m_connection.database()};
        itemInsert.prepare(QStringLiteral(
            "INSERT INTO mail_import_items(item_id,operation_id,ordinal,source_path,"
            "source_relative_path,source_kind,content_offset,content_end,decoded_size,"
            "source_canonical_path,source_size,source_mtime_ms,received_at,"
            "destination_relative_path,resolved_mailbox_id,phase,source_sha256,uploaded_blob_id,"
            "pre_state,created_email_id,existing_email_id,last_error) VALUES(:item,:operation,"
            ":ordinal,:source,:relative,:kind,:offset,:end,:decoded,:canonical,:source_size,:mtime,"
            ":received,:destination,:mailbox,:phase,:sha,:blob,:state,:created,:existing,:error)"));
        for (const auto& item : items)
        {
            itemInsert.bindValue(QStringLiteral(":item"), QString::fromStdString(item.itemId));
            itemInsert.bindValue(QStringLiteral(":operation"),
                                 QString::fromStdString(std::string{operationId}));
            itemInsert.bindValue(QStringLiteral(":ordinal"), static_cast<qulonglong>(item.ordinal));
            itemInsert.bindValue(QStringLiteral(":source"), item.sourcePath);
            itemInsert.bindValue(QStringLiteral(":relative"),
                                 optionalText(item.sourceRelativePath));
            itemInsert.bindValue(QStringLiteral(":kind"), sourceKindName(item.sourceKind));
            itemInsert.bindValue(QStringLiteral(":offset"), optionalInteger(item.contentOffset));
            itemInsert.bindValue(QStringLiteral(":end"), optionalInteger(item.contentEnd));
            itemInsert.bindValue(QStringLiteral(":decoded"),
                                 static_cast<qulonglong>(item.decodedSize));
            itemInsert.bindValue(QStringLiteral(":canonical"),
                                 item.sourceFingerprint.canonicalPath);
            itemInsert.bindValue(QStringLiteral(":source_size"),
                                 static_cast<qulonglong>(item.sourceFingerprint.size));
            itemInsert.bindValue(QStringLiteral(":mtime"), item.sourceFingerprint.lastModifiedMs);
            itemInsert.bindValue(QStringLiteral(":received"), optionalText(item.receivedAt));
            itemInsert.bindValue(QStringLiteral(":destination"),
                                 optionalText(item.destinationRelativePath));
            itemInsert.bindValue(QStringLiteral(":mailbox"), optionalText(item.resolvedMailboxId));
            itemInsert.bindValue(QStringLiteral(":phase"), itemPhaseName(item.phase));
            itemInsert.bindValue(QStringLiteral(":sha"), optionalText(item.sourceSha256));
            itemInsert.bindValue(QStringLiteral(":blob"), optionalText(item.uploadedBlobId));
            itemInsert.bindValue(QStringLiteral(":state"), optionalText(item.preState));
            itemInsert.bindValue(QStringLiteral(":created"), optionalText(item.createdEmailId));
            itemInsert.bindValue(QStringLiteral(":existing"), optionalText(item.existingEmailId));
            itemInsert.bindValue(QStringLiteral(":error"), optionalText(item.lastError));
            if (!itemInsert.exec())
                return queryError(QStringLiteral("Store mail import item scan"), itemInsert);
        }

        QSqlQuery seal{m_connection.database()};
        seal.prepare(QStringLiteral("UPDATE mail_import_operations SET "
                                    "scan_sealed=1,status='running',last_error=NULL WHERE "
                                    "operation_id=:id"));
        seal.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{operationId}));
        if (!seal.exec())
            return queryError(QStringLiteral("Seal mail import scan"), seal);
        return transaction.commit();
    }

    std::variant<std::vector<MailImportMailboxRecord>, DatabaseError>
    MailImportRepository::listMailboxes(const std::string_view operationId) const
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT ordinal,relative_path,parent_relative_path,display_name,phase,"
            "resolved_mailbox_id,last_error FROM mail_import_mailboxes WHERE operation_id=:id "
            "ORDER BY ordinal"));
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{operationId}));
        if (!query.exec())
            return queryError(QStringLiteral("Read mail import mailboxes"), query);
        std::vector<MailImportMailboxRecord> result;
        while (query.next())
            result.push_back(mailboxFromQuery(query));
        return result;
    }

    std::variant<std::optional<MailImportMailboxRecord>, DatabaseError>
    MailImportRepository::nextPendingMailbox(const std::string_view operationId) const
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT ordinal,relative_path,parent_relative_path,display_name,phase,"
            "resolved_mailbox_id,last_error FROM mail_import_mailboxes WHERE operation_id=:id AND "
            "phase='pending' ORDER BY ordinal LIMIT 1"));
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{operationId}));
        if (!query.exec())
            return queryError(QStringLiteral("Read next mail import mailbox"), query);
        if (!query.next())
            return std::optional<MailImportMailboxRecord>{};
        return std::optional<MailImportMailboxRecord>{mailboxFromQuery(query)};
    }

    std::optional<DatabaseError>
    MailImportRepository::resolveMailbox(const std::string_view operationId,
                                         const QStringView relativePath,
                                         const MailImportMailboxPhase phase, std::string mailboxId)
    {
        const javelin::jmap::cache::DatabaseWriteScope writeScope{m_connection};
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "UPDATE mail_import_mailboxes SET phase=:phase,resolved_mailbox_id=:mailbox,last_error="
            "NULL WHERE operation_id=:operation AND relative_path=:path"));
        query.bindValue(QStringLiteral(":phase"), mailboxPhaseName(phase));
        query.bindValue(QStringLiteral(":mailbox"), QString::fromStdString(mailboxId));
        query.bindValue(QStringLiteral(":operation"),
                        QString::fromStdString(std::string{operationId}));
        query.bindValue(QStringLiteral(":path"), relativePath.toString());
        if (!query.exec())
            return queryError(QStringLiteral("Resolve mail import mailbox"), query);
        return std::nullopt;
    }

    std::optional<DatabaseError>
    MailImportRepository::failMailbox(const std::string_view operationId,
                                      const QStringView relativePath, QString error)
    {
        const javelin::jmap::cache::DatabaseWriteScope writeScope{m_connection};
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "UPDATE mail_import_mailboxes SET phase='failed',last_error=:error WHERE operation_id="
            ":operation AND relative_path=:path"));
        query.bindValue(QStringLiteral(":error"), std::move(error));
        query.bindValue(QStringLiteral(":operation"),
                        QString::fromStdString(std::string{operationId}));
        query.bindValue(QStringLiteral(":path"), relativePath.toString());
        if (!query.exec())
            return queryError(QStringLiteral("Fail mail import mailbox"), query);
        return std::nullopt;
    }

    std::optional<DatabaseError> MailImportRepository::propagateMailboxResolution(
        const std::string_view operationId, const QStringView relativePath,
        const std::optional<std::string_view> mailboxId, std::optional<QString> failure)
    {
        const javelin::jmap::cache::DatabaseWriteScope writeScope{m_connection};
        QSqlQuery query{m_connection.database()};
        if (mailboxId.has_value())
        {
            query.prepare(QStringLiteral(
                "UPDATE mail_import_items SET resolved_mailbox_id=:mailbox WHERE operation_id="
                ":operation AND destination_relative_path=:path AND phase='pending'"));
            query.bindValue(QStringLiteral(":mailbox"),
                            QString::fromStdString(std::string{*mailboxId}));
        }
        else
        {
            query.prepare(QStringLiteral(
                "UPDATE mail_import_items SET phase='no_destination',last_error=:error WHERE "
                "operation_id=:operation AND (destination_relative_path=:path OR "
                "substr(destination_relative_path,1,length(:prefix))=:prefix) AND "
                "phase='pending'"));
            query.bindValue(QStringLiteral(":error"), optionalText(failure));
            query.bindValue(QStringLiteral(":prefix"),
                            QString{relativePath.toString() + QStringLiteral("/")});
        }
        query.bindValue(QStringLiteral(":operation"),
                        QString::fromStdString(std::string{operationId}));
        query.bindValue(QStringLiteral(":path"), relativePath.toString());
        if (!query.exec())
            return queryError(QStringLiteral("Propagate mail import mailbox resolution"), query);
        return std::nullopt;
    }

    std::variant<std::optional<MailImportItemRecord>, DatabaseError>
    MailImportRepository::nextActionableItem(const std::string_view operationId) const
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral("SELECT ") + QString::fromLatin1(itemColumns) +
                      QStringLiteral(" FROM mail_import_items WHERE operation_id=:id AND phase IN "
                                     "('pending','uploading','uploaded','creating','unknown') "
                                     "ORDER BY ordinal LIMIT 1"));
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{operationId}));
        if (!query.exec())
            return queryError(QStringLiteral("Read next mail import item"), query);
        if (!query.next())
            return std::optional<MailImportItemRecord>{};
        return std::optional<MailImportItemRecord>{itemFromQuery(query)};
    }

    std::variant<std::vector<MailImportItemRecord>, DatabaseError>
    MailImportRepository::listUnknownItems(const std::string_view operationId) const
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral("SELECT ") + QString::fromLatin1(itemColumns) +
                      QStringLiteral(" FROM mail_import_items WHERE operation_id=:id AND phase IN "
                                     "('creating','unknown') ORDER BY ordinal"));
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{operationId}));
        if (!query.exec())
            return queryError(QStringLiteral("Read unknown mail import items"), query);
        std::vector<MailImportItemRecord> result;
        while (query.next())
            result.push_back(itemFromQuery(query));
        return result;
    }

    std::optional<DatabaseError>
    MailImportRepository::transitionItem(const std::string_view itemId,
                                         const MailImportItemTransition& transition)
    {
        const javelin::jmap::cache::DatabaseWriteScope writeScope{m_connection};
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "UPDATE mail_import_items SET phase=:phase,source_sha256=COALESCE(:sha,source_sha256),"
            "uploaded_blob_id=COALESCE(:blob,uploaded_blob_id),pre_state=COALESCE(:state,pre_state)"
            ","
            "created_email_id=COALESCE(:created,created_email_id),existing_email_id=COALESCE("
            ":existing,existing_email_id),last_error=:error WHERE item_id=:id"));
        query.bindValue(QStringLiteral(":phase"), itemPhaseName(transition.phase));
        query.bindValue(QStringLiteral(":sha"), optionalText(transition.sourceSha256));
        query.bindValue(QStringLiteral(":blob"), optionalText(transition.uploadedBlobId));
        query.bindValue(QStringLiteral(":state"), optionalText(transition.preState));
        query.bindValue(QStringLiteral(":created"), optionalText(transition.createdEmailId));
        query.bindValue(QStringLiteral(":existing"), optionalText(transition.existingEmailId));
        query.bindValue(QStringLiteral(":error"), optionalText(transition.lastError));
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{itemId}));
        if (!query.exec())
            return queryError(QStringLiteral("Transition mail import item"), query);
        return std::nullopt;
    }

    std::variant<MailImportProgressSnapshot, DatabaseError>
    MailImportRepository::progress(const std::string_view operationId) const
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT COUNT(*),COALESCE(SUM(CASE WHEN phase IN ('created','reused','failed',"
            "'no_destination') THEN 1 ELSE 0 END),0),COALESCE(SUM(CASE WHEN phase='created' THEN 1 "
            "ELSE 0 END),0),COALESCE(SUM(CASE WHEN phase='reused' THEN 1 ELSE 0 END),0),COALESCE("
            "SUM(CASE WHEN phase IN ('failed','no_destination') THEN 1 ELSE 0 END),0),COALESCE(SUM("
            "CASE WHEN phase IN ('creating','unknown') THEN 1 ELSE 0 END),0),COALESCE(SUM("
            "decoded_size),0),COALESCE(SUM(CASE WHEN phase IN ('created','reused','failed',"
            "'no_destination') THEN decoded_size ELSE 0 END),0) FROM mail_import_items WHERE "
            "operation_id=:id"));
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{operationId}));
        if (!query.exec() || !query.next())
            return queryError(QStringLiteral("Read mail import progress"), query);
        return MailImportProgressSnapshot{
            .totalItems = query.value(0).toULongLong(),
            .completedItems = query.value(1).toULongLong(),
            .createdItems = query.value(2).toULongLong(),
            .reusedItems = query.value(3).toULongLong(),
            .failedItems = query.value(4).toULongLong(),
            .unknownItems = query.value(5).toULongLong(),
            .totalBytes = query.value(6).toULongLong(),
            .completedBytes = query.value(7).toULongLong(),
        };
    }

    std::variant<std::vector<std::string>, DatabaseError>
    MailImportRepository::resolvedMailboxIds(const std::string_view operationId) const
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT DISTINCT resolved_mailbox_id FROM mail_import_items WHERE operation_id=:id "
            "AND resolved_mailbox_id IS NOT NULL ORDER BY resolved_mailbox_id"));
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{operationId}));
        if (!query.exec())
            return queryError(QStringLiteral("Read mail import destination mailboxes"), query);
        std::vector<std::string> result;
        while (query.next())
            result.push_back(query.value(0).toString().toStdString());
        return result;
    }
} // namespace javelin::app
