#pragma once

#include "jmap/api/Cancellation.h"
#include "jmap/api/Error.h"
#include "jmap/api/Session.h"

#include <QCoroTask>

#include <QString>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace javelin::jmap::api
{
    class AbstractTransport;

    struct BlobUploadResponse
    {
        std::string accountId;
        std::string blobId;
        std::string type;
        std::uint64_t size = 0;
    };

    struct BlobUploadContext
    {
        std::string uploadUrl;
        std::optional<std::uint64_t> maxSizeUpload;
    };

    using BlobUploadContextResult = std::variant<BlobUploadContext, ProtocolError>;
    using BlobUploadResult = std::variant<BlobUploadResponse, TransportError, ProtocolError>;

    [[nodiscard]] BlobUploadContextResult blobUploadContext(const Session& session,
                                                            std::string_view accountId);

    [[nodiscard]] QCoro::Task<BlobUploadResult>
    uploadBlobFromFile(AbstractTransport& transport, BlobUploadContext context,
                       std::string authenticationAccountId, std::string accountId,
                       std::string accessToken, QString filePath, std::string mediaType,
                       CancellationToken cancellation = {}, std::function<void()> dispatched = {});

} // namespace javelin::jmap::api
