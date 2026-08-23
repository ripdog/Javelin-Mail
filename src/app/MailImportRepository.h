#pragma once

#include "app/MailImportApplicationPorts.h"
#include "app/MailImportSource.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <QString>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::app
{
    enum class MailImportStatus
    {
        Preparing,
        Running,
        WaitingForNetwork,
        WaitingForAuth,
        WaitingForSpace,
        BlockedUnknown,
        Partial,
        Failed,
        Complete,
    };

    enum class MailImportMailboxPhase
    {
        Pending,
        Reused,
        Created,
        Failed,
    };

    enum class MailImportItemPhase
    {
        Pending,
        Uploading,
        Uploaded,
        Creating,
        Unknown,
        Created,
        Reused,
        NoDestination,
        Failed,
    };

    struct MailImportOperationRecord
    {
        std::string operationId;
        std::string accountId;
        std::optional<std::string> mailboxId;
        std::vector<QString> sourcePaths;
        bool recreateHierarchy = false;
        MailImportStatus status = MailImportStatus::Preparing;
        bool scanSealed = false;
        QString title;
        QString createdAt;
        std::optional<QString> lastError;
    };

    struct MailImportMailboxRecord
    {
        std::size_t ordinal = 0;
        QString relativePath;
        std::optional<QString> parentRelativePath = std::nullopt;
        QString displayName;
        MailImportMailboxPhase phase = MailImportMailboxPhase::Pending;
        std::optional<std::string> resolvedMailboxId = std::nullopt;
        std::optional<QString> lastError = std::nullopt;
    };

    struct MailImportItemRecord
    {
        std::string itemId;
        std::size_t ordinal = 0;
        QString sourcePath;
        std::optional<QString> sourceRelativePath = std::nullopt;
        MailImportFileKind sourceKind = MailImportFileKind::Eml;
        std::optional<std::uint64_t> contentOffset = std::nullopt;
        std::optional<std::uint64_t> contentEnd = std::nullopt;
        std::uint64_t decodedSize = 0;
        MailImportSourceFingerprint sourceFingerprint;
        std::optional<std::string> receivedAt = std::nullopt;
        std::optional<QString> destinationRelativePath = std::nullopt;
        std::optional<std::string> resolvedMailboxId = std::nullopt;
        MailImportItemPhase phase = MailImportItemPhase::Pending;
        std::optional<std::string> sourceSha256 = std::nullopt;
        std::optional<std::string> uploadedBlobId = std::nullopt;
        std::optional<std::string> preState = std::nullopt;
        std::optional<std::string> createdEmailId = std::nullopt;
        std::optional<std::string> existingEmailId = std::nullopt;
        std::optional<QString> lastError = std::nullopt;
    };

    struct MailImportProgressSnapshot
    {
        std::uint64_t totalItems = 0;
        std::uint64_t completedItems = 0;
        std::uint64_t createdItems = 0;
        std::uint64_t reusedItems = 0;
        std::uint64_t failedItems = 0;
        std::uint64_t unknownItems = 0;
        std::uint64_t totalBytes = 0;
        std::uint64_t completedBytes = 0;
    };

    struct MailImportItemTransition
    {
        MailImportItemPhase phase = MailImportItemPhase::Pending;
        std::optional<std::string> sourceSha256 = std::nullopt;
        std::optional<std::string> uploadedBlobId = std::nullopt;
        std::optional<std::string> preState = std::nullopt;
        std::optional<std::string> createdEmailId = std::nullopt;
        std::optional<std::string> existingEmailId = std::nullopt;
        std::optional<QString> lastError = std::nullopt;
    };

    class MailImportRepository
    {
      public:
        explicit MailImportRepository(javelin::jmap::cache::DatabaseConnection& connection);

        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        createOperation(const MailImportOperationRecord& operation);
        [[nodiscard]] std::variant<std::optional<MailImportOperationRecord>,
                                   javelin::jmap::cache::DatabaseError>
        findOperation(std::string_view operationId) const;
        [[nodiscard]] std::variant<std::vector<MailImportOperationRecord>,
                                   javelin::jmap::cache::DatabaseError>
        listRecoverable() const;
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        setStatus(std::string_view operationId, MailImportStatus status,
                  std::optional<QString> lastError = std::nullopt);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        replaceScan(std::string_view operationId,
                    const std::vector<MailImportMailboxRecord>& mailboxes,
                    const std::vector<MailImportItemRecord>& items);
        [[nodiscard]] std::variant<std::vector<MailImportMailboxRecord>,
                                   javelin::jmap::cache::DatabaseError>
        listMailboxes(std::string_view operationId) const;
        [[nodiscard]] std::variant<std::optional<MailImportMailboxRecord>,
                                   javelin::jmap::cache::DatabaseError>
        nextPendingMailbox(std::string_view operationId) const;
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        resolveMailbox(std::string_view operationId, QStringView relativePath,
                       MailImportMailboxPhase phase, std::string mailboxId);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        failMailbox(std::string_view operationId, QStringView relativePath, QString error);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        propagateMailboxResolution(std::string_view operationId, QStringView relativePath,
                                   std::optional<std::string_view> mailboxId,
                                   std::optional<QString> failure = std::nullopt);
        [[nodiscard]] std::variant<std::optional<MailImportItemRecord>,
                                   javelin::jmap::cache::DatabaseError>
        nextActionableItem(std::string_view operationId) const;
        [[nodiscard]] std::variant<std::vector<MailImportItemRecord>,
                                   javelin::jmap::cache::DatabaseError>
        listUnknownItems(std::string_view operationId) const;
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        transitionItem(std::string_view itemId, const MailImportItemTransition& transition);
        [[nodiscard]] std::variant<MailImportProgressSnapshot, javelin::jmap::cache::DatabaseError>
        progress(std::string_view operationId) const;
        [[nodiscard]] std::variant<std::vector<std::string>, javelin::jmap::cache::DatabaseError>
        resolvedMailboxIds(std::string_view operationId) const;

      private:
        javelin::jmap::cache::DatabaseConnection& m_connection;
    };
} // namespace javelin::app
