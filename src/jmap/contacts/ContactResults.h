#pragma once

#include "jmap/OperationError.h"
#include "jmap/sync/MutationCommitReceipt.h"

#include <QByteArray>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace javelin::jmap::contacts
{
    struct CreatedContactMapping
    {
        std::string creationId;
        std::string serverId;

        auto operator<=>(const CreatedContactMapping&) const = default;
    };

    struct ContactRefreshSummary
    {
        std::size_t accountCount = 0;
        std::size_t addressBookCount = 0;
        std::size_t contactCount = 0;
        std::vector<javelin::jmap::sync::ReconciledMutation> reconciledMutations;
    };

    struct ContactMutationSummary
    {
        std::string accountId;
        std::string newState;
        std::optional<std::string> createdId;
        std::vector<CreatedContactMapping> createdIds;
        javelin::jmap::sync::MutationCommitReceipt receipt;
    };

    struct UploadedContactMedia
    {
        std::string accountId;
        std::string blobId;
        std::string mediaType;
        std::uint64_t size = 0;
    };

    struct DownloadedContactMedia
    {
        QByteArray data;
        std::string mediaType;
    };

    using ContactRefreshResult = std::variant<ContactRefreshSummary, javelin::jmap::OperationError>;
    using ContactMutationResult =
        std::variant<ContactMutationSummary, javelin::jmap::OperationError>;
    using ContactUploadResult = std::variant<UploadedContactMedia, javelin::jmap::OperationError>;
    using ContactDownloadResult =
        std::variant<DownloadedContactMedia, javelin::jmap::OperationError>;
} // namespace javelin::jmap::contacts
