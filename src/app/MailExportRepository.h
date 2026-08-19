#pragma once

#include "app/MailExportApplicationPorts.h"
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
    enum class MailExportStatus
    {
        Preparing,
        Running,
        WaitingForNetwork,
        WaitingForAuth,
        WaitingForSpace,
        Partial,
        Failed,
        Complete,
    };

    enum class MailExportItemPhase
    {
        Pending,
        SourceReady,
        Writing,
        Complete,
        Failed,
    };

    struct MailExportOperationRecord
    {
        std::string operationId;
        std::string accountId;
        MailExportScopeKind scopeKind = MailExportScopeKind::Mailbox;
        std::optional<std::string> mailboxId;
        MailExportFormat format = MailExportFormat::Eml;
        QString destinationDirectory;
        MailExportStatus status = MailExportStatus::Preparing;
        bool manifestSealed = false;
        std::optional<std::string> manifestEmailState;
        QString title;
        QString createdAt;
        std::optional<QString> lastError;
    };

    struct MailExportMailboxRecord
    {
        std::size_t ordinal = 0;
        std::string mailboxId;
        QString displayName;
        QString relativePath;
    };

    struct MailExportItemRecord
    {
        std::string itemId;
        std::size_t ordinal = 0;
        std::string mailboxId;
        std::string emailId;
        std::string blobId;
        std::uint64_t size = 0;
        std::optional<std::string> subject;
        std::string receivedAt;
        std::optional<std::string> senderName;
        std::optional<std::string> senderEmail;
        MailExportItemPhase phase = MailExportItemPhase::Pending;
        std::optional<QString> outputRelativePath;
        std::optional<std::string> rawContentHash;
        std::optional<std::uint64_t> mboxStartOffset;
        std::optional<std::uint64_t> mboxEndOffset;
        std::optional<QString> lastError;
    };

    struct MailExportProgressSnapshot
    {
        std::uint64_t totalItems = 0;
        std::uint64_t completedItems = 0;
        std::uint64_t failedItems = 0;
        std::uint64_t totalBytes = 0;
        std::uint64_t completedBytes = 0;
    };

    class MailExportRepository
    {
      public:
        explicit MailExportRepository(javelin::jmap::cache::DatabaseConnection& connection);

        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        createOperation(const MailExportOperationRecord& operation);
        [[nodiscard]] std::variant<std::optional<MailExportOperationRecord>,
                                   javelin::jmap::cache::DatabaseError>
        findOperation(std::string_view operationId) const;
        [[nodiscard]] std::variant<std::vector<MailExportOperationRecord>,
                                   javelin::jmap::cache::DatabaseError>
        listRecoverable() const;
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        setStatus(std::string_view operationId, MailExportStatus status,
                  std::optional<QString> lastError = std::nullopt);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        replaceMailboxes(std::string_view operationId,
                         const std::vector<MailExportMailboxRecord>& mailboxes);
        [[nodiscard]] std::variant<std::vector<MailExportMailboxRecord>,
                                   javelin::jmap::cache::DatabaseError>
        listMailboxes(std::string_view operationId) const;
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        resetManifest(std::string_view operationId);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        appendItems(std::string_view operationId, const std::vector<MailExportItemRecord>& items);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        sealManifest(std::string_view operationId, std::string emailState);
        [[nodiscard]] std::variant<std::optional<MailExportItemRecord>,
                                   javelin::jmap::cache::DatabaseError>
        nextIncompleteItem(std::string_view operationId) const;
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        markSourceReady(std::string_view itemId, std::string contentHash,
                        QString outputRelativePath);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        markWriting(std::string_view itemId, std::optional<std::uint64_t> mboxStartOffset);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        markComplete(std::string_view itemId,
                     std::optional<std::uint64_t> mboxEndOffset = std::nullopt);
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        markFailed(std::string_view itemId, QString error);
        [[nodiscard]] std::variant<MailExportProgressSnapshot, javelin::jmap::cache::DatabaseError>
        progress(std::string_view operationId) const;
        [[nodiscard]] std::variant<std::vector<MailExportItemRecord>,
                                   javelin::jmap::cache::DatabaseError>
        listWritingItems(std::string_view operationId) const;
        [[nodiscard]] std::variant<std::uint64_t, javelin::jmap::cache::DatabaseError>
        committedMboxSize(std::string_view operationId, std::string_view mailboxId) const;

      private:
        javelin::jmap::cache::DatabaseConnection& m_connection;
    };
} // namespace javelin::app
