#include "app/MailExportRepository.h"

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

        [[nodiscard]] QString scopeName(const MailExportScopeKind scope)
        {
            return scope == MailExportScopeKind::Mailbox ? QStringLiteral("mailbox")
                                                         : QStringLiteral("account");
        }

        [[nodiscard]] MailExportScopeKind scopeFromName(const QStringView value)
        {
            return value == u"account" ? MailExportScopeKind::Account
                                       : MailExportScopeKind::Mailbox;
        }

        [[nodiscard]] QString formatName(const MailExportFormat format)
        {
            return format == MailExportFormat::MboxRd ? QStringLiteral("mboxrd")
                                                      : QStringLiteral("eml");
        }

        [[nodiscard]] MailExportFormat formatFromName(const QStringView value)
        {
            return value == u"mboxrd" ? MailExportFormat::MboxRd : MailExportFormat::Eml;
        }

        [[nodiscard]] QString statusName(const MailExportStatus status)
        {
            switch (status)
            {
            case MailExportStatus::Preparing:
                return QStringLiteral("preparing");
            case MailExportStatus::Running:
                return QStringLiteral("running");
            case MailExportStatus::WaitingForNetwork:
                return QStringLiteral("waiting_network");
            case MailExportStatus::WaitingForAuth:
                return QStringLiteral("waiting_auth");
            case MailExportStatus::WaitingForSpace:
                return QStringLiteral("waiting_space");
            case MailExportStatus::Partial:
                return QStringLiteral("partial");
            case MailExportStatus::Failed:
                return QStringLiteral("failed");
            case MailExportStatus::Complete:
                return QStringLiteral("complete");
            }
            return QStringLiteral("failed");
        }

        [[nodiscard]] MailExportStatus statusFromName(const QStringView value)
        {
            if (value == u"preparing")
                return MailExportStatus::Preparing;
            if (value == u"running")
                return MailExportStatus::Running;
            if (value == u"waiting_network")
                return MailExportStatus::WaitingForNetwork;
            if (value == u"waiting_auth")
                return MailExportStatus::WaitingForAuth;
            if (value == u"waiting_space")
                return MailExportStatus::WaitingForSpace;
            if (value == u"partial")
                return MailExportStatus::Partial;
            if (value == u"complete")
                return MailExportStatus::Complete;
            return MailExportStatus::Failed;
        }

        [[nodiscard]] QString phaseName(const MailExportItemPhase phase)
        {
            switch (phase)
            {
            case MailExportItemPhase::Pending:
                return QStringLiteral("pending");
            case MailExportItemPhase::SourceReady:
                return QStringLiteral("source_ready");
            case MailExportItemPhase::Writing:
                return QStringLiteral("writing");
            case MailExportItemPhase::Complete:
                return QStringLiteral("complete");
            case MailExportItemPhase::Failed:
                return QStringLiteral("failed");
            }
            return QStringLiteral("failed");
        }

        [[nodiscard]] MailExportItemPhase phaseFromName(const QStringView value)
        {
            if (value == u"pending")
                return MailExportItemPhase::Pending;
            if (value == u"source_ready")
                return MailExportItemPhase::SourceReady;
            if (value == u"writing")
                return MailExportItemPhase::Writing;
            if (value == u"complete")
                return MailExportItemPhase::Complete;
            return MailExportItemPhase::Failed;
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

        [[nodiscard]] MailExportOperationRecord operationFromQuery(const QSqlQuery& query)
        {
            return {
                .operationId = query.value(0).toString().toStdString(),
                .accountId = query.value(1).toString().toStdString(),
                .scopeKind = scopeFromName(query.value(2).toString()),
                .mailboxId = optionalString(query.value(3)),
                .format = formatFromName(query.value(4).toString()),
                .destinationDirectory = query.value(5).toString(),
                .status = statusFromName(query.value(6).toString()),
                .manifestSealed = query.value(7).toInt() != 0,
                .manifestEmailState = optionalString(query.value(8)),
                .title = query.value(9).toString(),
                .createdAt = query.value(10).toString(),
                .lastError = optionalQString(query.value(11)),
            };
        }

        [[nodiscard]] MailExportItemRecord itemFromQuery(const QSqlQuery& query)
        {
            return {
                .itemId = query.value(0).toString().toStdString(),
                .ordinal = query.value(1).toULongLong(),
                .mailboxId = query.value(2).toString().toStdString(),
                .emailId = query.value(3).toString().toStdString(),
                .blobId = query.value(4).toString().toStdString(),
                .size = query.value(5).toULongLong(),
                .subject = optionalString(query.value(6)),
                .receivedAt = query.value(7).toString().toStdString(),
                .senderName = optionalString(query.value(8)),
                .senderEmail = optionalString(query.value(9)),
                .phase = phaseFromName(query.value(10).toString()),
                .outputRelativePath = optionalQString(query.value(11)),
                .rawContentHash = optionalString(query.value(12)),
                .mboxStartOffset =
                    query.value(13).isNull()
                        ? std::nullopt
                        : std::optional<std::uint64_t>{query.value(13).toULongLong()},
                .mboxEndOffset = query.value(14).isNull()
                                     ? std::nullopt
                                     : std::optional<std::uint64_t>{query.value(14).toULongLong()},
                .lastError = optionalQString(query.value(15)),
            };
        }
    } // namespace

    MailExportRepository::MailExportRepository(javelin::jmap::cache::DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MailExportRepository::createOperation(const MailExportOperationRecord& operation)
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "INSERT INTO mail_export_operations(operation_id,account_id,scope_kind,mailbox_id,"
            "format,destination_directory,status,manifest_sealed,manifest_email_state,title,"
            "created_at,last_error) VALUES(:id,:account,:scope,:mailbox,:format,:destination,"
            ":status,:sealed,:state,:title,:created,:error)"));
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(operation.operationId));
        query.bindValue(QStringLiteral(":account"), QString::fromStdString(operation.accountId));
        query.bindValue(QStringLiteral(":scope"), scopeName(operation.scopeKind));
        query.bindValue(QStringLiteral(":mailbox"), optionalText(operation.mailboxId));
        query.bindValue(QStringLiteral(":format"), formatName(operation.format));
        query.bindValue(QStringLiteral(":destination"), operation.destinationDirectory);
        query.bindValue(QStringLiteral(":status"), statusName(operation.status));
        query.bindValue(QStringLiteral(":sealed"), operation.manifestSealed ? 1 : 0);
        query.bindValue(QStringLiteral(":state"), optionalText(operation.manifestEmailState));
        query.bindValue(QStringLiteral(":title"), operation.title);
        query.bindValue(QStringLiteral(":created"), operation.createdAt);
        query.bindValue(QStringLiteral(":error"), optionalText(operation.lastError));
        if (!query.exec())
            return queryError(QStringLiteral("Create mail export"), query);
        return std::nullopt;
    }

    std::variant<std::optional<MailExportOperationRecord>, javelin::jmap::cache::DatabaseError>
    MailExportRepository::findOperation(const std::string_view operationId) const
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT operation_id,account_id,scope_kind,mailbox_id,format,destination_directory,"
            "status,manifest_sealed,manifest_email_state,title,created_at,last_error FROM "
            "mail_export_operations WHERE operation_id=:id"));
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{operationId}));
        if (!query.exec())
            return queryError(QStringLiteral("Read mail export"), query);
        if (!query.next())
            return std::optional<MailExportOperationRecord>{};
        return std::optional<MailExportOperationRecord>{operationFromQuery(query)};
    }

    std::variant<std::vector<MailExportOperationRecord>, javelin::jmap::cache::DatabaseError>
    MailExportRepository::listRecoverable() const
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT operation_id,account_id,scope_kind,mailbox_id,format,destination_directory,"
            "status,manifest_sealed,manifest_email_state,title,created_at,last_error FROM "
            "mail_export_operations WHERE status IN ('preparing','running','waiting_network',"
            "'waiting_auth','waiting_space') ORDER BY created_at,operation_id"));
        if (!query.exec())
            return queryError(QStringLiteral("List recoverable mail exports"), query);
        std::vector<MailExportOperationRecord> values;
        while (query.next())
            values.push_back(operationFromQuery(query));
        return values;
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MailExportRepository::setStatus(const std::string_view operationId,
                                    const MailExportStatus status, std::optional<QString> lastError)
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "UPDATE mail_export_operations SET status=:status,last_error=:error WHERE "
            "operation_id=:id"));
        query.bindValue(QStringLiteral(":status"), statusName(status));
        query.bindValue(QStringLiteral(":error"), optionalText(lastError));
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{operationId}));
        if (!query.exec())
            return queryError(QStringLiteral("Update mail export status"), query);
        return std::nullopt;
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MailExportRepository::replaceMailboxes(const std::string_view operationId,
                                           const std::vector<MailExportMailboxRecord>& mailboxes)
    {
        auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
            m_connection, QStringLiteral("Replace mail export mailbox snapshot"));
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
            return *error;
        auto transaction =
            std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(transactionResult));
        QSqlQuery remove{m_connection.database()};
        remove.prepare(QStringLiteral("DELETE FROM mail_export_mailboxes WHERE operation_id=:id"));
        remove.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{operationId}));
        if (!remove.exec())
            return queryError(QStringLiteral("Clear mail export mailbox snapshot"), remove);

        QSqlQuery insert{m_connection.database()};
        insert.prepare(QStringLiteral(
            "INSERT INTO mail_export_mailboxes(operation_id,ordinal,mailbox_id,display_name,"
            "relative_path) VALUES(:operation,:ordinal,:mailbox,:name,:path)"));
        for (const auto& mailbox : mailboxes)
        {
            insert.bindValue(QStringLiteral(":operation"),
                             QString::fromStdString(std::string{operationId}));
            insert.bindValue(QStringLiteral(":ordinal"), static_cast<qulonglong>(mailbox.ordinal));
            insert.bindValue(QStringLiteral(":mailbox"), QString::fromStdString(mailbox.mailboxId));
            insert.bindValue(QStringLiteral(":name"), mailbox.displayName);
            insert.bindValue(QStringLiteral(":path"), mailbox.relativePath);
            if (!insert.exec())
                return queryError(QStringLiteral("Store mail export mailbox snapshot"), insert);
        }
        return transaction.commit();
    }

    std::variant<std::vector<MailExportMailboxRecord>, javelin::jmap::cache::DatabaseError>
    MailExportRepository::listMailboxes(const std::string_view operationId) const
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT ordinal,mailbox_id,display_name,relative_path FROM mail_export_mailboxes WHERE "
            "operation_id=:id ORDER BY ordinal"));
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{operationId}));
        if (!query.exec())
            return queryError(QStringLiteral("Read mail export mailbox snapshot"), query);
        std::vector<MailExportMailboxRecord> values;
        while (query.next())
        {
            values.push_back({
                .ordinal = query.value(0).toULongLong(),
                .mailboxId = query.value(1).toString().toStdString(),
                .displayName = query.value(2).toString(),
                .relativePath = query.value(3).toString(),
            });
        }
        return values;
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MailExportRepository::resetManifest(const std::string_view operationId)
    {
        auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
            m_connection, QStringLiteral("Reset mail export manifest"));
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
            return *error;
        auto transaction =
            std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(transactionResult));
        QSqlQuery items{m_connection.database()};
        items.prepare(QStringLiteral("DELETE FROM mail_export_items WHERE operation_id=:id"));
        items.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{operationId}));
        if (!items.exec())
            return queryError(QStringLiteral("Clear mail export manifest"), items);
        QSqlQuery operation{m_connection.database()};
        operation.prepare(QStringLiteral(
            "UPDATE mail_export_operations SET manifest_sealed=0,manifest_email_state=NULL,status="
            "'preparing',last_error=NULL WHERE operation_id=:id"));
        operation.bindValue(QStringLiteral(":id"),
                            QString::fromStdString(std::string{operationId}));
        if (!operation.exec())
            return queryError(QStringLiteral("Reset mail export operation"), operation);
        return transaction.commit();
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MailExportRepository::appendItems(const std::string_view operationId,
                                      const std::vector<MailExportItemRecord>& items)
    {
        if (items.empty())
            return std::nullopt;
        auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
            m_connection, QStringLiteral("Append mail export manifest"));
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
            return *error;
        auto transaction =
            std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(transactionResult));
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "INSERT OR IGNORE INTO mail_export_items(item_id,operation_id,ordinal,mailbox_id,"
            "email_id,blob_id,size,subject,received_at,sender_name,sender_email,phase,"
            "output_relative_path,raw_content_hash,mbox_start_offset,mbox_end_offset,last_error) "
            "VALUES(:item,:operation,:ordinal,:mailbox,:email,:blob,:size,:subject,:received,"
            ":sender_name,:sender_email,:phase,:output,:hash,:offset,:end_offset,:error)"));
        for (const auto& item : items)
        {
            query.bindValue(QStringLiteral(":item"), QString::fromStdString(item.itemId));
            query.bindValue(QStringLiteral(":operation"),
                            QString::fromStdString(std::string{operationId}));
            query.bindValue(QStringLiteral(":ordinal"), static_cast<qulonglong>(item.ordinal));
            query.bindValue(QStringLiteral(":mailbox"), QString::fromStdString(item.mailboxId));
            query.bindValue(QStringLiteral(":email"), QString::fromStdString(item.emailId));
            query.bindValue(QStringLiteral(":blob"), QString::fromStdString(item.blobId));
            query.bindValue(QStringLiteral(":size"), static_cast<qulonglong>(item.size));
            query.bindValue(QStringLiteral(":subject"), optionalText(item.subject));
            query.bindValue(QStringLiteral(":received"), QString::fromStdString(item.receivedAt));
            query.bindValue(QStringLiteral(":sender_name"), optionalText(item.senderName));
            query.bindValue(QStringLiteral(":sender_email"), optionalText(item.senderEmail));
            query.bindValue(QStringLiteral(":phase"), phaseName(item.phase));
            query.bindValue(QStringLiteral(":output"), optionalText(item.outputRelativePath));
            query.bindValue(QStringLiteral(":hash"), optionalText(item.rawContentHash));
            query.bindValue(QStringLiteral(":offset"),
                            item.mboxStartOffset.has_value()
                                ? QVariant{static_cast<qulonglong>(*item.mboxStartOffset)}
                                : QVariant{});
            query.bindValue(QStringLiteral(":end_offset"),
                            item.mboxEndOffset.has_value()
                                ? QVariant{static_cast<qulonglong>(*item.mboxEndOffset)}
                                : QVariant{});
            query.bindValue(QStringLiteral(":error"), optionalText(item.lastError));
            if (!query.exec())
                return queryError(QStringLiteral("Append mail export item"), query);
        }
        return transaction.commit();
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MailExportRepository::sealManifest(const std::string_view operationId, std::string emailState)
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "UPDATE mail_export_operations SET manifest_sealed=1,manifest_email_state=:state,"
            "status='running',last_error=NULL WHERE operation_id=:id"));
        query.bindValue(QStringLiteral(":state"), QString::fromStdString(emailState));
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{operationId}));
        if (!query.exec())
            return queryError(QStringLiteral("Seal mail export manifest"), query);
        return std::nullopt;
    }

    std::variant<std::optional<MailExportItemRecord>, javelin::jmap::cache::DatabaseError>
    MailExportRepository::nextIncompleteItem(const std::string_view operationId) const
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT item_id,ordinal,mailbox_id,email_id,blob_id,size,subject,received_at,"
            "sender_name,sender_email,phase,output_relative_path,raw_content_hash,mbox_start_"
            "offset,"
            "mbox_end_offset,last_error FROM mail_export_items WHERE operation_id=:id AND phase IN "
            "('pending','source_ready','writing') ORDER BY ordinal LIMIT 1"));
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{operationId}));
        if (!query.exec())
            return queryError(QStringLiteral("Read next mail export item"), query);
        if (!query.next())
            return std::optional<MailExportItemRecord>{};
        return std::optional<MailExportItemRecord>{itemFromQuery(query)};
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MailExportRepository::markSourceReady(const std::string_view itemId, std::string contentHash,
                                          QString outputRelativePath)
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "UPDATE mail_export_items SET phase='source_ready',raw_content_hash=:hash,"
            "output_relative_path=:output,last_error=NULL WHERE item_id=:id"));
        query.bindValue(QStringLiteral(":hash"), QString::fromStdString(contentHash));
        query.bindValue(QStringLiteral(":output"), outputRelativePath);
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{itemId}));
        if (!query.exec())
            return queryError(QStringLiteral("Pin mail export source"), query);
        return std::nullopt;
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MailExportRepository::markWriting(const std::string_view itemId,
                                      const std::optional<std::uint64_t> mboxStartOffset)
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "UPDATE mail_export_items SET phase='writing',mbox_start_offset=:offset WHERE "
            "item_id=:id"));
        query.bindValue(QStringLiteral(":offset"),
                        mboxStartOffset.has_value()
                            ? QVariant{static_cast<qulonglong>(*mboxStartOffset)}
                            : QVariant{});
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{itemId}));
        if (!query.exec())
            return queryError(QStringLiteral("Start mail export item write"), query);
        return std::nullopt;
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MailExportRepository::markComplete(const std::string_view itemId,
                                       const std::optional<std::uint64_t> mboxEndOffset)
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(
            QStringLiteral("UPDATE mail_export_items SET phase='complete',mbox_start_offset=NULL,"
                           "mbox_end_offset=:end_offset,last_error=NULL WHERE item_id=:id"));
        query.bindValue(QStringLiteral(":end_offset"),
                        mboxEndOffset.has_value()
                            ? QVariant{static_cast<qulonglong>(*mboxEndOffset)}
                            : QVariant{});
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{itemId}));
        if (!query.exec())
            return queryError(QStringLiteral("Complete mail export item"), query);
        return std::nullopt;
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MailExportRepository::markFailed(const std::string_view itemId, QString error)
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "UPDATE mail_export_items SET phase='failed',last_error=:error WHERE item_id=:id"));
        query.bindValue(QStringLiteral(":error"), std::move(error));
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{itemId}));
        if (!query.exec())
            return queryError(QStringLiteral("Fail mail export item"), query);
        return std::nullopt;
    }

    std::variant<MailExportProgressSnapshot, javelin::jmap::cache::DatabaseError>
    MailExportRepository::progress(const std::string_view operationId) const
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT COUNT(*),COALESCE(SUM(CASE WHEN phase='complete' THEN 1 ELSE 0 END),0),"
            "COALESCE(SUM(CASE WHEN phase='failed' THEN 1 ELSE 0 END),0),COALESCE(SUM(size),0),"
            "COALESCE(SUM(CASE WHEN phase='complete' THEN size ELSE 0 END),0) FROM "
            "mail_export_items WHERE operation_id=:id"));
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{operationId}));
        if (!query.exec() || !query.next())
            return queryError(QStringLiteral("Read mail export progress"), query);
        return MailExportProgressSnapshot{
            .totalItems = query.value(0).toULongLong(),
            .completedItems = query.value(1).toULongLong(),
            .failedItems = query.value(2).toULongLong(),
            .totalBytes = query.value(3).toULongLong(),
            .completedBytes = query.value(4).toULongLong(),
        };
    }

    std::variant<std::vector<MailExportItemRecord>, javelin::jmap::cache::DatabaseError>
    MailExportRepository::listWritingItems(const std::string_view operationId) const
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT item_id,ordinal,mailbox_id,email_id,blob_id,size,subject,received_at,"
            "sender_name,sender_email,phase,output_relative_path,raw_content_hash,mbox_start_"
            "offset,"
            "mbox_end_offset,last_error FROM mail_export_items WHERE operation_id=:id AND "
            "phase='writing' ORDER BY ordinal"));
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{operationId}));
        if (!query.exec())
            return queryError(QStringLiteral("Read interrupted mail export items"), query);
        std::vector<MailExportItemRecord> values;
        while (query.next())
            values.push_back(itemFromQuery(query));
        return values;
    }

    std::variant<std::uint64_t, javelin::jmap::cache::DatabaseError>
    MailExportRepository::committedMboxSize(const std::string_view operationId,
                                            const std::string_view mailboxId) const
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(
            QStringLiteral("SELECT COALESCE(MAX(mbox_end_offset),0) FROM mail_export_items WHERE "
                           "operation_id=:operation AND mailbox_id=:mailbox AND phase='complete'"));
        query.bindValue(QStringLiteral(":operation"),
                        QString::fromStdString(std::string{operationId}));
        query.bindValue(QStringLiteral(":mailbox"), QString::fromStdString(std::string{mailboxId}));
        if (!query.exec() || !query.next())
            return queryError(QStringLiteral("Read committed mbox size"), query);
        return query.value(0).toULongLong();
    }
} // namespace javelin::app
