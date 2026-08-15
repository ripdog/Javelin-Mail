#pragma once

#include "storage/sqlite/DatabaseConnection.h"

#include <QDateTime>
#include <QString>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::app
{
    enum class MailTransferOperation
    {
        Copy,
        Move,
    };

    enum class MailTransferTopology
    {
        SameSessionCopy,
        CrossServerImport,
    };

    enum class MailTransferStatus
    {
        Preparing,
        Running,
        WaitingForNetwork,
        WaitingForAuth,
        WaitingForSpace,
        BlockedUnknown,
        Partial,
        Failed,
        Cancelled,
        Complete,
    };

    enum class MailTransferItemPhase
    {
        Prepared,
        AcquiringSource,
        SourceReady,
        Uploading,
        Uploaded,
        CreatingDestination,
        DestinationUnknown,
        DestinationConfirmed,
        RemovingSource,
        SourceCleanupUnknown,
        PartialSourceRetained,
        Failed,
        Cancelled,
        Complete,
    };

    struct MailTransferOperationRecord
    {
        std::string operationId;
        std::optional<std::string> operationGroupId;
        std::string sourceAccountId; // Stable local/cache account key.
        std::optional<std::string> sourceMailboxId;
        std::string destinationAccountId; // Stable local/cache account key.
        std::string destinationMailboxId;
        MailTransferOperation operation = MailTransferOperation::Copy;
        MailTransferTopology topology = MailTransferTopology::CrossServerImport;
        MailTransferStatus status = MailTransferStatus::Preparing;
        QString title;
        std::optional<QString> lastError;
        std::optional<QString> historyEntryId;
        QDateTime createdAt;
        QDateTime updatedAt;
    };

    struct MailTransferItemRecord
    {
        std::string itemId;
        std::string operationId;
        std::int64_t ordinal = 0;
        std::string sourceEmailId;
        std::string sourceBlobId;
        std::optional<std::string> sourceEmailState;
        std::vector<std::string> sourceMailboxIds;
        std::vector<std::string> sourceKeywords;
        std::vector<std::string> sourceMessageIds;
        std::optional<std::string> sourceReceivedAt;
        std::uint64_t sourceSize = 0;
        std::vector<std::string> sourceRemoveMailboxIds;
        bool sourceDestroy = false;
        std::optional<std::string> rawContentHash;
        std::string destinationCreationId;
        std::optional<std::string> destinationUploadBlobId;
        std::optional<std::string> destinationPreState;
        std::optional<std::string> destinationEmailId;
        std::optional<std::string> destinationBlobId;
        std::optional<std::string> destinationThreadId;
        std::optional<std::uint64_t> destinationSize;
        bool reusedExisting = false;
        std::optional<std::vector<std::string>> destinationPriorMailboxIds;
        MailTransferItemPhase phase = MailTransferItemPhase::Prepared;
        std::optional<QString> lastError;
        QDateTime createdAt;
        QDateTime updatedAt;
    };

    struct MailTransferDestinationResult
    {
        std::string emailId;
        std::optional<std::string> blobId;
        std::optional<std::string> threadId;
        std::optional<std::uint64_t> size;
        bool reusedExisting = false;
        std::optional<std::vector<std::string>> priorMailboxIds;
    };

    class MailTransferRepository
    {
      public:
        explicit MailTransferRepository(javelin::jmap::cache::DatabaseConnection& connection);

        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        create(const MailTransferOperationRecord& operation,
               const std::vector<MailTransferItemRecord>& items);
        [[nodiscard]]
        std::variant<std::optional<MailTransferOperationRecord>,
                     javelin::jmap::cache::DatabaseError>
        findOperation(std::string_view operationId) const;
        [[nodiscard]] std::variant<std::vector<MailTransferItemRecord>,
                                   javelin::jmap::cache::DatabaseError>
        listItems(std::string_view operationId) const;
        [[nodiscard]] std::variant<std::vector<MailTransferOperationRecord>,
                                   javelin::jmap::cache::DatabaseError>
        listRecoverable() const;

        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        updateStatus(std::string_view operationId, MailTransferStatus status,
                     std::optional<QString> error = std::nullopt);
        [[nodiscard]] std::variant<bool, javelin::jmap::cache::DatabaseError>
        markHistoryPublished(std::string_view operationId, QString historyEntryId);
        [[nodiscard]] std::variant<bool, javelin::jmap::cache::DatabaseError>
        transitionItem(std::string_view itemId, MailTransferItemPhase expected,
                       MailTransferItemPhase next, std::optional<QString> error = std::nullopt);
        [[nodiscard]] std::variant<bool, javelin::jmap::cache::DatabaseError>
        markSourceReady(std::string_view itemId, MailTransferItemPhase expected,
                        std::string_view rawContentHash);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        releaseSourcePin(std::string_view itemId);
        [[nodiscard]] std::variant<bool, javelin::jmap::cache::DatabaseError>
        reassignSourcePin(std::string_view itemId, std::string_view ownerKind,
                          std::string_view ownerId);
        [[nodiscard]] std::variant<bool, javelin::jmap::cache::DatabaseError>
        markUploaded(std::string_view itemId, MailTransferItemPhase expected,
                     std::string_view destinationUploadBlobId);
        [[nodiscard]] std::variant<bool, javelin::jmap::cache::DatabaseError>
        markDestinationDispatching(std::string_view itemId, MailTransferItemPhase expected,
                                   std::string_view destinationPreState);
        [[nodiscard]] std::variant<bool, javelin::jmap::cache::DatabaseError>
        markExistingDestinationCandidate(std::string_view itemId, MailTransferItemPhase expected,
                                         std::string_view destinationEmailId,
                                         std::string_view destinationPreState,
                                         const std::vector<std::string>& priorMailboxIds);
        [[nodiscard]] std::variant<bool, javelin::jmap::cache::DatabaseError>
        markDestinationConfirmed(std::string_view itemId, MailTransferItemPhase expected,
                                 const MailTransferDestinationResult& destination);
        [[nodiscard]] std::variant<bool, javelin::jmap::cache::DatabaseError>
        markSourceCleanupPrepared(std::string_view itemId, MailTransferItemPhase expected,
                                  std::string_view sourceEmailState,
                                  const std::vector<std::string>& removeMailboxIds,
                                  bool destroy);
        [[nodiscard]] std::variant<bool, javelin::jmap::cache::DatabaseError>
        pinSourceForCleanup(std::string_view itemId, MailTransferItemPhase expected,
                            std::string_view rawContentHash);

      private:
        javelin::jmap::cache::DatabaseConnection& m_connection;
    };

    [[nodiscard]] QString toString(MailTransferOperation operation);
    [[nodiscard]] std::optional<MailTransferOperation>
    mailTransferOperationFromString(QStringView value);
    [[nodiscard]] QString toString(MailTransferTopology topology);
    [[nodiscard]] std::optional<MailTransferTopology>
    mailTransferTopologyFromString(QStringView value);
    [[nodiscard]] QString toString(MailTransferStatus status);
    [[nodiscard]] std::optional<MailTransferStatus> mailTransferStatusFromString(QStringView value);
    [[nodiscard]] QString toString(MailTransferItemPhase phase);
    [[nodiscard]] std::optional<MailTransferItemPhase>
    mailTransferItemPhaseFromString(QStringView value);

} // namespace javelin::app
