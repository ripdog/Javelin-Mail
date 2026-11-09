#pragma once

#include "jmap/api/Cancellation.h"
#include "jmap/api/Error.h"
#include "jmap/auth/Auth.h"

#include <QCoroTask>

#include <QByteArray>
#include <QDateTime>
#include <QIODevice>
#include <QList>
#include <QNetworkAccessManager>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QUrl>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::api
{

    enum class HttpMethod
    {
        Get,
        Post,
    };

    struct HttpHeader
    {
        QByteArray name;
        QByteArray value;
    };

    struct BearerAuthentication
    {
        std::string accountId;
        std::string accessToken;
    };

    struct HttpRequest
    {
        HttpMethod method = HttpMethod::Get;
        QUrl url;
        QList<HttpHeader> headers;
        QByteArray body;
        std::optional<BearerAuthentication> authentication;
        CancellationToken cancellation{};
        std::function<void()> dispatched;
    };

    struct HttpResponse
    {
        int statusCode = 0;
        QByteArray body;
    };

    struct HttpFileResponse
    {
        int statusCode = 0;
        std::uint64_t size = 0;
    };

    // Parses an RFC 9110 Retry-After value, which is either delay-seconds or an IMF-fixdate
    // HTTP-date. A date in the past yields a zero delay.
    [[nodiscard]] std::optional<std::chrono::seconds> parseRetryAfter(const QByteArray& value,
                                                                      const QDateTime& now);

    using TransportResult = std::variant<HttpResponse, TransportError>;
    using FileTransportResult = std::variant<HttpFileResponse, TransportError>;

    class AbstractTransport
    {
      public:
        virtual ~AbstractTransport() = default;

        // Aborts in-flight requests authenticated as accountId and drops idle pooled connections.
        virtual void invalidateConnections(std::string_view accountId)
        {
            static_cast<void>(accountId);
        }

        [[nodiscard]] virtual QCoro::Task<TransportResult> send(HttpRequest request) = 0;
        [[nodiscard]] virtual QCoro::Task<TransportResult> sendFromFile(HttpRequest request,
                                                                        QString filePath);
        [[nodiscard]] virtual QCoro::Task<TransportResult>
        sendFromDevice(HttpRequest request, QIODevice& device, std::uint64_t contentLength);
        [[nodiscard]] virtual QCoro::Task<FileTransportResult> sendToFile(HttpRequest request,
                                                                          QString filePath);
    };

    class RefreshingTransport final : public AbstractTransport
    {
      public:
        explicit RefreshingTransport(AbstractTransport& transport);

        void setAccessTokenProvider(javelin::jmap::auth::AccessTokenProvider provider);
        void setRefreshHandler(javelin::jmap::auth::AccessTokenRefreshHandler handler);
        void invalidateConnections(std::string_view accountId) override;

        [[nodiscard]] QCoro::Task<TransportResult> send(HttpRequest request) override;
        [[nodiscard]] QCoro::Task<TransportResult> sendFromFile(HttpRequest request,
                                                                QString filePath) override;
        [[nodiscard]] QCoro::Task<TransportResult>
        sendFromDevice(HttpRequest request, QIODevice& device,
                       std::uint64_t contentLength) override;
        [[nodiscard]] QCoro::Task<FileTransportResult> sendToFile(HttpRequest request,
                                                                  QString filePath) override;

      private:
        // Dispatches request with the current access token and, when the server rejects it with
        // 401 and the refresh handler yields a different token, dispatches it once more.
        // prepareRetry may veto the retry with an error, for example when a body cannot rewind.
        template <typename Result, typename Dispatch, typename PrepareRetry>
        [[nodiscard]] QCoro::Task<Result> sendWithAuthenticationRetry(HttpRequest request,
                                                                      Dispatch dispatch,
                                                                      PrepareRetry prepareRetry);

        AbstractTransport& m_transport;
        javelin::jmap::auth::AccessTokenProvider m_accessTokenProvider;
        javelin::jmap::auth::AccessTokenRefreshHandler m_refreshHandler;
    };

    class QtNetworkTransport : public AbstractTransport
    {
      public:
        explicit QtNetworkTransport(QNetworkAccessManager& networkAccessManager);
        ~QtNetworkTransport() override = default;

        void invalidateConnections(std::string_view accountId) override;

        [[nodiscard]] QCoro::Task<TransportResult> send(HttpRequest request) override;
        [[nodiscard]] QCoro::Task<TransportResult> sendFromFile(HttpRequest request,
                                                                QString filePath) override;
        [[nodiscard]] QCoro::Task<TransportResult>
        sendFromDevice(HttpRequest request, QIODevice& device,
                       std::uint64_t contentLength) override;
        [[nodiscard]] QCoro::Task<FileTransportResult> sendToFile(HttpRequest request,
                                                                  QString filePath) override;

      private:
        QNetworkAccessManager& m_networkAccessManager;
        struct ActiveReply
        {
            QPointer<QNetworkReply> reply;
            std::string accountId;
        };

        void registerActiveReply(QNetworkReply* reply, const HttpRequest& request);
        void unregisterActiveReply(const QNetworkReply* reply);

        std::vector<ActiveReply> m_activeReplies;
    };

} // namespace javelin::jmap::api
