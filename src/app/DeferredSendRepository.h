#pragma once

#include "jmap/cache/Database.h"

#include <QDateTime>
#include <QString>

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace javelin::app
{
    enum class DeferredSendStatus
    {
        Scheduled,
        WaitingForNetwork,
        WaitingForAuth,
        Dispatching,
        Submitted,
        Cancelled,
        Failed,
        Unknown,
    };

    struct PendingSend
    {
        QString sendId;
        QString historyEntryId;
        std::string connectionId;
        std::string accountId;
        std::string composeSessionId;
        std::string draftEmailId;
        std::optional<std::string> subject;
        DeferredSendStatus status = DeferredSendStatus::Scheduled;
        QDateTime dueAt;
        std::optional<QDateTime> dispatchStartedAt;
        std::optional<std::string> submissionId;
        std::optional<QString> lastError;
        QDateTime createdAt;
        QDateTime updatedAt;
    };

    class DeferredSendRepository
    {
      public:
        explicit DeferredSendRepository(javelin::jmap::cache::DatabaseConnection& connection);

        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        insert(const PendingSend& send);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        insertAndActivateHistory(const PendingSend& send);
        [[nodiscard]] std::variant<std::optional<PendingSend>, javelin::jmap::cache::DatabaseError>
        find(const QString& sendId) const;
        [[nodiscard]] std::variant<std::vector<PendingSend>, javelin::jmap::cache::DatabaseError>
        listRecoverable() const;
        [[nodiscard]] std::variant<bool, javelin::jmap::cache::DatabaseError>
        claimForDispatch(const QString& sendId, const QDateTime& startedAt);
        [[nodiscard]] std::variant<bool, javelin::jmap::cache::DatabaseError>
        cancelBeforeDispatch(const QString& sendId);
        [[nodiscard]] std::variant<bool, javelin::jmap::cache::DatabaseError>
        reschedule(const QString& sendId, const QDateTime& dueAt);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        markWaiting(const QString& sendId, DeferredSendStatus status, QString error);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        markSubmitted(const QString& sendId, std::optional<std::string> submissionId);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        markFailed(const QString& sendId, QString error);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        markUnknown(const QString& sendId, QString error);
        [[nodiscard]] std::variant<std::size_t, javelin::jmap::cache::DatabaseError>
        recoverDispatching();

      private:
        javelin::jmap::cache::DatabaseConnection& m_connection;
    };

    [[nodiscard]] QString toString(DeferredSendStatus status);
    [[nodiscard]] std::optional<DeferredSendStatus> deferredSendStatusFromString(QStringView value);
} // namespace javelin::app
