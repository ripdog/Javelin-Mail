#include "jmap/api/BlobUpload.h"

#include "jmap/api/Transport.h"

#include <QByteArray>
#include <QFileInfo>
#include <QUrl>

#include <glaze/glaze.hpp>

namespace
{
    struct RawBlobUploadResponse
    {
        std::string accountId;
        std::string blobId;
        std::string type;
        std::uint64_t size = 0;
    };

    [[nodiscard]] QUrl uploadUrl(const std::string_view templateUrl,
                                 const std::string_view accountId)
    {
        QString expanded = QString::fromStdString(std::string{templateUrl});
        const QString encodedAccountId = QString::fromUtf8(
            QUrl::toPercentEncoding(QString::fromStdString(std::string{accountId})));
        expanded.replace(QStringLiteral("{accountId}"), encodedAccountId);
        return QUrl{expanded};
    }

    [[nodiscard]] javelin::jmap::api::ProtocolError invalidRequest(std::string message)
    {
        return {
            .code = javelin::jmap::api::ProtocolErrorCode::InvalidRequest,
            .message = std::move(message),
        };
    }

    [[nodiscard]] javelin::jmap::api::ProtocolError invalidResponse(std::string message)
    {
        return {
            .code = javelin::jmap::api::ProtocolErrorCode::InvalidResponse,
            .message = std::move(message),
        };
    }

    [[nodiscard]] javelin::jmap::api::HttpRequest
    uploadRequest(const javelin::jmap::api::BlobUploadContext& context,
                  const std::string& authenticationAccountId, const std::string& accountId,
                  const std::string& accessToken, const std::string& mediaType,
                  javelin::jmap::api::CancellationToken cancellation,
                  std::function<void()> dispatched)
    {
        return {
            .method = javelin::jmap::api::HttpMethod::Post,
            .url = uploadUrl(context.uploadUrl, accountId),
            .headers =
                {
                    javelin::jmap::api::HttpHeader{
                        .name = QByteArrayLiteral("Authorization"),
                        .value =
                            QByteArrayLiteral("Bearer ") + QByteArray::fromStdString(accessToken),
                    },
                    javelin::jmap::api::HttpHeader{
                        .name = QByteArrayLiteral("Accept"),
                        .value = QByteArrayLiteral("application/json"),
                    },
                    javelin::jmap::api::HttpHeader{
                        .name = QByteArrayLiteral("Content-Type"),
                        .value = QByteArray::fromStdString(mediaType),
                    },
                },
            .body = {},
            .authentication =
                javelin::jmap::api::BearerAuthentication{
                    .accountId = authenticationAccountId,
                    .accessToken = accessToken,
                },
            .cancellation = std::move(cancellation),
            .dispatched = std::move(dispatched),
        };
    }

    [[nodiscard]] javelin::jmap::api::BlobUploadResult
    decodeUploadResult(javelin::jmap::api::TransportResult transportResult,
                       const std::string_view expectedAccountId)
    {
        using namespace javelin::jmap::api;
        if (const auto* error = std::get_if<TransportError>(&transportResult))
            return *error;

        const auto& response = std::get<HttpResponse>(transportResult);
        if (response.statusCode < 200 || response.statusCode >= 300)
        {
            return TransportError{
                .code = TransportErrorCode::HttpFailure,
                .message = "JMAP blob upload failed with HTTP status " +
                           std::to_string(response.statusCode),
                .httpStatus = response.statusCode,
            };
        }
        RawBlobUploadResponse raw;
        std::string json = response.body.toStdString();
        const auto parseError = glz::read<glz::opts{.error_on_unknown_keys = false}>(raw, json);
        if (parseError)
            return invalidResponse("Failed to decode the JMAP blob upload response");
        if (raw.accountId.empty() || raw.blobId.empty() || raw.type.empty())
            return invalidResponse("The JMAP blob upload response is missing required fields");
        if (raw.accountId != expectedAccountId)
            return invalidResponse("The JMAP blob upload response returned the wrong account id");

        return BlobUploadResponse{
            .accountId = std::move(raw.accountId),
            .blobId = std::move(raw.blobId),
            .type = std::move(raw.type),
            .size = raw.size,
        };
    }

} // namespace

template <> struct glz::meta<RawBlobUploadResponse>
{
    using T = RawBlobUploadResponse;
    static constexpr auto value = glz::object("accountId", &T::accountId, "blobId", &T::blobId,
                                              "type", &T::type, "size", &T::size);
};

namespace javelin::jmap::api
{
    BlobUploadContextResult blobUploadContext(const Session& session,
                                              const std::string_view accountId)
    {
        if (accountId.empty())
            return invalidRequest("Blob upload requires a destination account id");
        if (session.uploadUrl.empty())
            return invalidRequest("The JMAP session does not advertise an upload URL");
        if (!session.accounts.contains(std::string{accountId}))
            return invalidRequest("The destination account is not present in the JMAP session");

        return BlobUploadContext{
            .uploadUrl = session.uploadUrl,
            .maxSizeUpload = session.capabilities.coreDetails.has_value()
                                 ? session.capabilities.coreDetails->maxSizeUpload
                                 : std::nullopt,
        };
    }

    QCoro::Task<BlobUploadResult>
    uploadBlobFromFile(AbstractTransport& transport, BlobUploadContext context,
                       std::string authenticationAccountId, std::string accountId,
                       std::string accessToken, QString filePath, std::string mediaType,
                       CancellationToken cancellation, std::function<void()> dispatched)
    {
        if (accountId.empty() || authenticationAccountId.empty() || accessToken.empty())
            co_return invalidRequest("Blob upload requires account and authentication identifiers");
        if (mediaType.empty())
            co_return invalidRequest("Blob upload requires a media type");
        if (context.uploadUrl.empty())
            co_return invalidRequest("Blob upload requires an upload URL");

        const QFileInfo source{filePath};
        if (!source.exists() || !source.isFile() || !source.isReadable())
        {
            co_return TransportError{
                .code = TransportErrorCode::LocalIoFailure,
                .message = "The blob upload source file is not readable",
                .httpStatus = std::nullopt,
                .networkError = std::nullopt,
                .retryAfter = std::nullopt,
            };
        }

        const auto sourceSize = static_cast<std::uint64_t>(source.size());
        if (context.maxSizeUpload.has_value() && sourceSize > *context.maxSizeUpload)
            co_return invalidRequest("Blob upload exceeds the server maxSizeUpload limit");

        auto request = uploadRequest(context, authenticationAccountId, accountId, accessToken,
                                     mediaType, std::move(cancellation), std::move(dispatched));
        auto transportResult =
            co_await transport.sendFromFile(std::move(request), std::move(filePath));
        co_return decodeUploadResult(std::move(transportResult), accountId);
    }

    QCoro::Task<BlobUploadResult>
    uploadBlobFromDevice(AbstractTransport& transport, BlobUploadContext context,
                         std::string authenticationAccountId, std::string accountId,
                         std::string accessToken, QIODevice& device,
                         const std::uint64_t contentLength, std::string mediaType,
                         CancellationToken cancellation, std::function<void()> dispatched)
    {
        if (accountId.empty() || authenticationAccountId.empty() || accessToken.empty())
            co_return invalidRequest("Blob upload requires account and authentication identifiers");
        if (mediaType.empty())
            co_return invalidRequest("Blob upload requires a media type");
        if (context.uploadUrl.empty())
            co_return invalidRequest("Blob upload requires an upload URL");
        if (!device.isOpen() || !device.isReadable())
        {
            co_return TransportError{
                .code = TransportErrorCode::LocalIoFailure,
                .message = "The blob upload source device is not readable",
                .httpStatus = std::nullopt,
                .networkError = std::nullopt,
                .retryAfter = std::nullopt,
            };
        }
        if (context.maxSizeUpload.has_value() && contentLength > *context.maxSizeUpload)
            co_return invalidRequest("Blob upload exceeds the server maxSizeUpload limit");

        auto request = uploadRequest(context, authenticationAccountId, accountId, accessToken,
                                     mediaType, std::move(cancellation), std::move(dispatched));
        auto transportResult =
            co_await transport.sendFromDevice(std::move(request), device, contentLength);
        co_return decodeUploadResult(std::move(transportResult), accountId);
    }

} // namespace javelin::jmap::api
