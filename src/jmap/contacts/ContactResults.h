#pragma once

#include "jmap/OperationError.h"

#include <QByteArray>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace javelin::jmap::contacts
{
    struct ContactRefreshSummary
    {
        std::size_t accountCount = 0;
        std::size_t addressBookCount = 0;
        std::size_t contactCount = 0;
    };

    struct ContactMutationSummary
    {
        std::string accountId;
        std::string newState;
        std::optional<std::string> createdId;
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
