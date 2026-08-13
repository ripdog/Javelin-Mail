#include "app/DeferredSendRepository.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QTimeZone>

namespace javelin::app
{
    namespace
    {
        [[nodiscard]] javelin::jmap::cache::DatabaseError queryError(QString operation,
                                                                     const QSqlQuery& query)
        {
            return {
                .code = javelin::jmap::cache::DatabaseErrorCode::QueryFailed,
                .message = std::move(operation) + QStringLiteral(": ") + query.lastError().text(),
            };
        }

        [[nodiscard]] QVariant optionalText(const std::optional<std::string>& value)
        {
            return value.has_value() ? QVariant{QString::fromStdString(*value)} : QVariant{};
        }

        [[nodiscard]] std::optional<std::string> optionalString(const QVariant& value)
        {
            return value.isNull() ? std::nullopt : std::optional{value.toString().toStdString()};
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

        [[nodiscard]] PendingSend readSend(const QSqlQuery& query)
        {
            return {
                .sendId = query.value(0).toString(),
                .historyEntryId = query.value(1).toString(),
                .connectionId = query.value(2).toString().toStdString(),
                .accountId = query.value(3).toString().toStdString(),
                .composeSessionId = query.value(4).toString().toStdString(),
                .draftEmailId = query.value(5).toString().toStdString(),
                .subject = optionalString(query.value(6)),
                .status = *deferredSendStatusFromString(query.value(7).toString()),
                .dueAt = timestamp(query.value(8)),
                .dispatchStartedAt = query.value(9).isNull()
                                         ? std::nullopt
                                         : std::optional{timestamp(query.value(9))},
                .submissionId = optionalString(query.value(10)),
                .lastError = query.value(11).isNull() ? std::nullopt
                                                      : std::optional{query.value(11).toString()},
                .createdAt = timestamp(query.value(12)),
                .updatedAt = timestamp(query.value(13)),
            };
        }

        [[nodiscard]] QString selectColumns()
        {
            return QStringLiteral(
                "send_id,history_entry_id,connection_id,account_id,compose_session_id,"
                "draft_email_id,subject,status,due_at,dispatch_started_at,submission_id,"
                "last_error,created_at,updated_at");
        }

    } // namespace

    QString toString(const DeferredSendStatus status)
    {
        switch (status)
        {
        case DeferredSendStatus::Scheduled:
            return QStringLiteral("scheduled");
        case DeferredSendStatus::WaitingForNetwork:
            return QStringLiteral("waiting_for_network");
        case DeferredSendStatus::WaitingForAuth:
            return QStringLiteral("waiting_for_auth");
        case DeferredSendStatus::Dispatching:
            return QStringLiteral("dispatching");
        case DeferredSendStatus::Submitted:
            return QStringLiteral("submitted");
        case DeferredSendStatus::Cancelled:
            return QStringLiteral("cancelled");
        case DeferredSendStatus::Failed:
            return QStringLiteral("failed");
        case DeferredSendStatus::Unknown:
            return QStringLiteral("unknown");
        }
        return QStringLiteral("unknown");
    }

    std::optional<DeferredSendStatus> deferredSendStatusFromString(const QStringView value)
    {
        if (value == QStringLiteral("scheduled"))
            return DeferredSendStatus::Scheduled;
        if (value == QStringLiteral("waiting_for_network"))
            return DeferredSendStatus::WaitingForNetwork;
        if (value == QStringLiteral("waiting_for_auth"))
            return DeferredSendStatus::WaitingForAuth;
        if (value == QStringLiteral("dispatching"))
            return DeferredSendStatus::Dispatching;
        if (value == QStringLiteral("submitted"))
            return DeferredSendStatus::Submitted;
        if (value == QStringLiteral("cancelled"))
            return DeferredSendStatus::Cancelled;
        if (value == QStringLiteral("failed"))
            return DeferredSendStatus::Failed;
        if (value == QStringLiteral("unknown"))
            return DeferredSendStatus::Unknown;
        return std::nullopt;
    }

    DeferredSendRepository::DeferredSendRepository(
        javelin::jmap::cache::DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    DeferredSendRepository::insert(const PendingSend& send)
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "INSERT INTO pending_sends("
            "send_id,history_entry_id,connection_id,account_id,compose_session_id,draft_email_id,"
            "subject,status,due_at"
            ") VALUES(:send_id,:history_entry_id,:connection_id,:account_id,:compose_session_id,"
            ":draft_email_id,:subject,:status,:due_at)"));
        query.bindValue(QStringLiteral(":send_id"), send.sendId);
        query.bindValue(QStringLiteral(":history_entry_id"), send.historyEntryId);
        query.bindValue(QStringLiteral(":connection_id"),
                        QString::fromStdString(send.connectionId));
        query.bindValue(QStringLiteral(":account_id"), QString::fromStdString(send.accountId));
        query.bindValue(QStringLiteral(":compose_session_id"),
                        QString::fromStdString(send.composeSessionId));
        query.bindValue(QStringLiteral(":draft_email_id"),
                        QString::fromStdString(send.draftEmailId));
        query.bindValue(QStringLiteral(":subject"), optionalText(send.subject));
        query.bindValue(QStringLiteral(":status"), toString(send.status));
        query.bindValue(QStringLiteral(":due_at"), send.dueAt.toUTC().toString(Qt::ISODateWithMs));
        if (!query.exec())
            return queryError(QStringLiteral("Insert pending send"), query);
        return std::nullopt;
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    DeferredSendRepository::insertAndActivateHistory(const PendingSend& send)
    {
        auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
            m_connection, QStringLiteral("Schedule deferred send"));
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
            return *error;
        auto transaction =
            std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(transactionResult));
        if (const auto error = insert(send))
            return error;

        QSqlQuery clearRedo{m_connection.database()};
        if (!clearRedo.exec(QStringLiteral("DELETE FROM operation_history WHERE stack='redo'")))
            return queryError(QStringLiteral("Clear redo history for deferred send"), clearRedo);

        QSqlQuery readOrder{m_connection.database()};
        if (!readOrder.exec(QStringLiteral(
                "SELECT next_value FROM operation_history_sequence WHERE singleton=1")) ||
            !readOrder.next())
            return queryError(QStringLiteral("Read deferred send history order"), readOrder);
        const auto order = readOrder.value(0).toLongLong();

        QSqlQuery advanceOrder{m_connection.database()};
        advanceOrder.prepare(QStringLiteral(
            "UPDATE operation_history_sequence SET next_value=:next_value WHERE singleton=1"));
        advanceOrder.bindValue(QStringLiteral(":next_value"), order + 1);
        if (!advanceOrder.exec())
            return queryError(QStringLiteral("Advance deferred send history order"), advanceOrder);

        QSqlQuery activate{m_connection.database()};
        activate.prepare(QStringLiteral(
            "UPDATE operation_history SET stack='undo',stack_order=:stack_order,status='ready',"
            "updated_at=:updated_at WHERE entry_id=:entry_id AND status='preparing'"));
        activate.bindValue(QStringLiteral(":stack_order"), order);
        activate.bindValue(QStringLiteral(":updated_at"),
                           QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        activate.bindValue(QStringLiteral(":entry_id"), send.historyEntryId);
        if (!activate.exec())
            return queryError(QStringLiteral("Activate deferred send history"), activate);
        if (activate.numRowsAffected() != 1)
            return javelin::jmap::cache::DatabaseError{
                .code = javelin::jmap::cache::DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Deferred send history reservation is unavailable."),
            };
        return transaction.commit();
    }

    std::variant<std::optional<PendingSend>, javelin::jmap::cache::DatabaseError>
    DeferredSendRepository::find(const QString& sendId) const
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral("SELECT ") + selectColumns() +
                      QStringLiteral(" FROM pending_sends WHERE send_id=:send_id"));
        query.bindValue(QStringLiteral(":send_id"), sendId);
        if (!query.exec())
            return queryError(QStringLiteral("Find pending send"), query);
        if (!query.next())
            return std::optional<PendingSend>{std::nullopt};
        return std::optional{readSend(query)};
    }

    std::variant<std::vector<PendingSend>, javelin::jmap::cache::DatabaseError>
    DeferredSendRepository::listRecoverable() const
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(
            QStringLiteral("SELECT ") + selectColumns() +
            QStringLiteral(" FROM pending_sends WHERE status IN "
                           "('scheduled','waiting_for_network','waiting_for_auth','dispatching',"
                           "'unknown') ORDER BY due_at,created_at"));
        if (!query.exec())
            return queryError(QStringLiteral("List pending sends"), query);
        std::vector<PendingSend> sends;
        while (query.next())
            sends.push_back(readSend(query));
        return sends;
    }

    std::variant<bool, javelin::jmap::cache::DatabaseError>
    DeferredSendRepository::claimForDispatch(const QString& sendId, const QDateTime& startedAt)
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "UPDATE pending_sends SET status='dispatching',dispatch_started_at=:started_at,"
            "updated_at=CURRENT_TIMESTAMP WHERE send_id=:send_id AND status IN "
            "('scheduled','waiting_for_network','waiting_for_auth')"));
        query.bindValue(QStringLiteral(":started_at"),
                        startedAt.toUTC().toString(Qt::ISODateWithMs));
        query.bindValue(QStringLiteral(":send_id"), sendId);
        if (!query.exec())
            return queryError(QStringLiteral("Claim pending send"), query);
        return query.numRowsAffected() == 1;
    }

    std::variant<bool, javelin::jmap::cache::DatabaseError>
    DeferredSendRepository::cancelBeforeDispatch(const QString& sendId)
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "UPDATE pending_sends SET status='cancelled',updated_at=CURRENT_TIMESTAMP "
            "WHERE send_id=:send_id AND status IN "
            "('scheduled','waiting_for_network','waiting_for_auth')"));
        query.bindValue(QStringLiteral(":send_id"), sendId);
        if (!query.exec())
            return queryError(QStringLiteral("Cancel pending send"), query);
        return query.numRowsAffected() == 1;
    }

    std::variant<bool, javelin::jmap::cache::DatabaseError>
    DeferredSendRepository::reschedule(const QString& sendId, const QDateTime& dueAt)
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "UPDATE pending_sends SET status='scheduled',due_at=:due_at,"
            "dispatch_started_at=NULL,submission_id=NULL,last_error=NULL,"
            "updated_at=CURRENT_TIMESTAMP WHERE send_id=:send_id AND status='cancelled'"));
        query.bindValue(QStringLiteral(":due_at"), dueAt.toUTC().toString(Qt::ISODateWithMs));
        query.bindValue(QStringLiteral(":send_id"), sendId);
        if (!query.exec())
            return queryError(QStringLiteral("Reschedule pending send"), query);
        return query.numRowsAffected() == 1;
    }

    std::variant<bool, javelin::jmap::cache::DatabaseError>
    DeferredSendRepository::releaseForDispatch(const QString& sendId, const QDateTime& releasedAt)
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(
            QStringLiteral("UPDATE pending_sends SET due_at=CASE WHEN due_at>:released_at "
                           "THEN :released_at ELSE due_at END,updated_at=CURRENT_TIMESTAMP "
                           "WHERE send_id=:send_id AND status='scheduled'"));
        query.bindValue(QStringLiteral(":released_at"),
                        releasedAt.toUTC().toString(Qt::ISODateWithMs));
        query.bindValue(QStringLiteral(":send_id"), sendId);
        if (!query.exec())
            return queryError(QStringLiteral("Release pending send for dispatch"), query);
        return query.numRowsAffected() == 1;
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    DeferredSendRepository::markWaiting(const QString& sendId, const DeferredSendStatus status,
                                        QString error)
    {
        if (status != DeferredSendStatus::WaitingForNetwork &&
            status != DeferredSendStatus::WaitingForAuth)
            return javelin::jmap::cache::DatabaseError{
                .code = javelin::jmap::cache::DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Invalid waiting send status."),
            };
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "UPDATE pending_sends SET status=:status,last_error=:error,"
            "dispatch_started_at=NULL,updated_at=CURRENT_TIMESTAMP WHERE send_id=:send_id"));
        query.bindValue(QStringLiteral(":status"), toString(status));
        query.bindValue(QStringLiteral(":error"), error);
        query.bindValue(QStringLiteral(":send_id"), sendId);
        if (!query.exec())
            return queryError(QStringLiteral("Mark pending send waiting"), query);
        return std::nullopt;
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    DeferredSendRepository::markSubmitted(const QString& sendId,
                                          std::optional<std::string> submissionId)
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "UPDATE pending_sends SET status='submitted',submission_id=:submission_id,"
            "last_error=NULL,updated_at=CURRENT_TIMESTAMP WHERE send_id=:send_id"));
        query.bindValue(QStringLiteral(":submission_id"), optionalText(submissionId));
        query.bindValue(QStringLiteral(":send_id"), sendId);
        if (!query.exec())
            return queryError(QStringLiteral("Mark pending send submitted"), query);
        return std::nullopt;
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    DeferredSendRepository::markFailed(const QString& sendId, QString error)
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral("UPDATE pending_sends SET status='failed',last_error=:error,"
                                     "updated_at=CURRENT_TIMESTAMP WHERE send_id=:send_id"));
        query.bindValue(QStringLiteral(":error"), error);
        query.bindValue(QStringLiteral(":send_id"), sendId);
        if (!query.exec())
            return queryError(QStringLiteral("Mark pending send failed"), query);
        return std::nullopt;
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    DeferredSendRepository::markUnknown(const QString& sendId, QString error)
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral("UPDATE pending_sends SET status='unknown',last_error=:error,"
                                     "updated_at=CURRENT_TIMESTAMP WHERE send_id=:send_id"));
        query.bindValue(QStringLiteral(":error"), error);
        query.bindValue(QStringLiteral(":send_id"), sendId);
        if (!query.exec())
            return queryError(QStringLiteral("Mark pending send unknown"), query);
        return std::nullopt;
    }

    std::variant<std::size_t, javelin::jmap::cache::DatabaseError>
    DeferredSendRepository::recoverDispatching()
    {
        QSqlQuery query{m_connection.database()};
        if (!query.exec(
                QStringLiteral("UPDATE pending_sends SET status='unknown',"
                               "last_error='Dispatch was interrupted and requires reconciliation',"
                               "updated_at=CURRENT_TIMESTAMP WHERE status='dispatching'")))
            return queryError(QStringLiteral("Recover dispatching sends"), query);
        return static_cast<std::size_t>(query.numRowsAffected());
    }
} // namespace javelin::app
