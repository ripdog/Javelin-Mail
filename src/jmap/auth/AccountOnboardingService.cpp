#include "jmap/auth/AccountOnboardingService.h"

#include "jmap/api/SessionDiscovery.h"
#include "jmap/api/SessionParser.h"

#include <glaze/glaze.hpp>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif
#include <QCoroNetworkReply>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QUrlQuery>
#include <QUuid>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace javelin::jmap::auth::detail
{
    struct ProtectedResourceMetadata
    {
        std::string resource;
        std::vector<std::string> authorizationServers;
        std::vector<std::string> scopesSupported;
    };

    struct AuthorizationServerMetadata
    {
        std::string issuer;
        std::string authorizationEndpoint;
        std::string tokenEndpoint;
        std::optional<std::string> registrationEndpoint;
        std::optional<std::string> revocationEndpoint;
        std::vector<std::string> scopesSupported;
        std::vector<std::string> responseTypesSupported;
        std::vector<std::string> grantTypesSupported;
        std::vector<std::string> tokenEndpointAuthMethodsSupported;
        std::vector<std::string> revocationEndpointAuthMethodsSupported;
        std::vector<std::string> codeChallengeMethodsSupported;
        bool authorizationResponseIssParameterSupported = false;
    };

    struct RegistrationRequest
    {
        std::vector<std::string> redirectUris;
        std::string clientName;
        std::string tokenEndpointAuthMethod;
        std::vector<std::string> grantTypes;
        std::vector<std::string> responseTypes;
        std::string scope;
        std::string clientUri;
        std::string logoUri;
        std::string tosUri;
        std::string policyUri;
        std::string softwareId;
        std::string softwareVersion;
    };

    struct RegistrationResponse
    {
        std::string clientId;
        std::optional<std::string> tokenEndpointAuthMethod;
        std::optional<std::vector<std::string>> redirectUris;
        std::optional<std::string> registrationClientUri;
        std::optional<std::string> registrationAccessToken;
    };

    struct OAuthErrorResponse
    {
        std::optional<std::string> error;
        std::optional<std::string> errorDescription;
        std::optional<std::string> title;
        std::optional<std::string> detail;
    };

    struct TokenResponse
    {
        std::string accessToken;
        std::optional<std::string> refreshToken;
        std::optional<std::string> tokenType;
        std::optional<std::string> scope;
        std::optional<std::int64_t> expiresIn;
        std::optional<std::string> error;
        std::optional<std::string> errorDescription;
    };
} // namespace javelin::jmap::auth::detail

template <> struct glz::meta<javelin::jmap::auth::detail::ProtectedResourceMetadata>
{
    using T = javelin::jmap::auth::detail::ProtectedResourceMetadata;
    static constexpr auto value =
        glz::object("resource", &T::resource, "authorization_servers", &T::authorizationServers,
                    "scopes_supported", &T::scopesSupported);
};

template <> struct glz::meta<javelin::jmap::auth::detail::AuthorizationServerMetadata>
{
    using T = javelin::jmap::auth::detail::AuthorizationServerMetadata;
    static constexpr auto value = glz::object(
        "issuer", &T::issuer, "authorization_endpoint", &T::authorizationEndpoint, "token_endpoint",
        &T::tokenEndpoint, "registration_endpoint", &T::registrationEndpoint, "revocation_endpoint",
        &T::revocationEndpoint, "scopes_supported", &T::scopesSupported, "response_types_supported",
        &T::responseTypesSupported, "grant_types_supported", &T::grantTypesSupported,
        "token_endpoint_auth_methods_supported", &T::tokenEndpointAuthMethodsSupported,
        "revocation_endpoint_auth_methods_supported", &T::revocationEndpointAuthMethodsSupported,
        "code_challenge_methods_supported", &T::codeChallengeMethodsSupported,
        "authorization_response_iss_parameter_supported",
        &T::authorizationResponseIssParameterSupported);
};

template <> struct glz::meta<javelin::jmap::auth::detail::RegistrationRequest>
{
    using T = javelin::jmap::auth::detail::RegistrationRequest;
    static constexpr auto value = glz::object(
        "redirect_uris", &T::redirectUris, "client_name", &T::clientName,
        "token_endpoint_auth_method", &T::tokenEndpointAuthMethod, "grant_types", &T::grantTypes,
        "response_types", &T::responseTypes, "scope", &T::scope, "client_uri", &T::clientUri,
        "logo_uri", &T::logoUri, "tos_uri", &T::tosUri, "policy_uri", &T::policyUri, "software_id",
        &T::softwareId, "software_version", &T::softwareVersion);
};

template <> struct glz::meta<javelin::jmap::auth::detail::RegistrationResponse>
{
    using T = javelin::jmap::auth::detail::RegistrationResponse;
    static constexpr auto value = glz::object(
        "client_id", &T::clientId, "token_endpoint_auth_method", &T::tokenEndpointAuthMethod,
        "redirect_uris", &T::redirectUris, "registration_client_uri", &T::registrationClientUri,
        "registration_access_token", &T::registrationAccessToken);
};

template <> struct glz::meta<javelin::jmap::auth::detail::OAuthErrorResponse>
{
    using T = javelin::jmap::auth::detail::OAuthErrorResponse;
    static constexpr auto value =
        glz::object("error", &T::error, "error_description", &T::errorDescription, "title",
                    &T::title, "detail", &T::detail);
};

template <> struct glz::meta<javelin::jmap::auth::detail::TokenResponse>
{
    using T = javelin::jmap::auth::detail::TokenResponse;
    static constexpr auto value =
        glz::object("access_token", &T::accessToken, "refresh_token", &T::refreshToken,
                    "token_type", &T::tokenType, "scope", &T::scope, "expires_in", &T::expiresIn,
                    "error", &T::error, "error_description", &T::errorDescription);
};

namespace javelin::jmap::auth
{
    Q_LOGGING_CATEGORY(oauthLog, "javelin.oauth")

    QString detail::registrationRedirectUri(const QString& callbackUri)
    {
        return QUrl{callbackUri}.toString(QUrl::FullyEncoded);
    }

    bool detail::isSecureOAuthUrl(const QUrl& url)
    {
        return url.isValid() && url.scheme() == QStringLiteral("https") && !url.host().isEmpty() &&
               url.userInfo().isEmpty() && !url.hasFragment();
    }

    bool detail::resourceMetadataMatches(const QString& returnedResource,
                                         const QString& expectedResource)
    {
        return !returnedResource.isEmpty() && returnedResource == expectedResource;
    }

    javelin::app::OAuthRefreshFailureKind detail::refreshFailureKind(const QString& oauthErrorCode)
    {
        using Kind = javelin::app::OAuthRefreshFailureKind;
        return oauthErrorCode == QStringLiteral("invalid_grant") ||
                       oauthErrorCode == QStringLiteral("invalid_client") ||
                       oauthErrorCode == QStringLiteral("unauthorized_client") ||
                       oauthErrorCode == QStringLiteral("invalid_scope")
                   ? Kind::ReauthenticationRequired
                   : Kind::Transient;
    }

    bool detail::isUsableOAuthRefreshRequest(const javelin::app::OAuthRefreshRequest& request)
    {
        return !request.tokenEndpoint.isEmpty() && !request.clientId.isEmpty() &&
               !request.refreshToken.isEmpty() && isSecureOAuthUrl(QUrl{request.tokenEndpoint}) &&
               (request.resourceUrl.isEmpty() || isSecureOAuthUrl(QUrl{request.resourceUrl}));
    }

    namespace
    {
        struct HttpResult
        {
            int statusCode = 0;
            QByteArray body;
            QByteArray authenticateHeader;
            QString error;
            QUrl finalUrl;
        };

        enum class RedirectTrust : std::uint8_t
        {
            Resource,
            OAuthEndpoint,
        };

        [[nodiscard]] QCoro::Task<HttpResult>
        get(QNetworkAccessManager& manager, const QUrl& url, const QByteArray& bearer = {},
            const RedirectTrust redirectTrust = RedirectTrust::OAuthEndpoint)
        {
            QNetworkRequest request{url};
            request.setTransferTimeout(30'000);
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                 redirectTrust == RedirectTrust::Resource
                                     ? QNetworkRequest::NoLessSafeRedirectPolicy
                                     : QNetworkRequest::SameOriginRedirectPolicy);
            request.setRawHeader(QByteArrayLiteral("Accept"),
                                 QByteArrayLiteral("application/json"));
            if (!bearer.isEmpty())
                request.setRawHeader(QByteArrayLiteral("Authorization"),
                                     QByteArrayLiteral("Bearer ") + bearer);
            auto* reply = manager.get(request);
            co_await qCoro(reply).waitForFinished();
            const auto cleanup = qScopeGuard([reply] { reply->deleteLater(); });
            static_cast<void>(cleanup);
            const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            co_return HttpResult{
                .statusCode = status,
                .body = reply->readAll(),
                .authenticateHeader = reply->rawHeader(QByteArrayLiteral("WWW-Authenticate")),
                .error = status == 0 ? reply->errorString() : QString{},
                .finalUrl = reply->url(),
            };
        }

        [[nodiscard]] QCoro::Task<HttpResult> post(QNetworkAccessManager& manager, const QUrl& url,
                                                   const QByteArray& contentType,
                                                   const QByteArray& body)
        {
            QNetworkRequest request{url};
            request.setTransferTimeout(30'000);
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                 QNetworkRequest::SameOriginRedirectPolicy);
            request.setRawHeader(QByteArrayLiteral("Accept"),
                                 QByteArrayLiteral("application/json"));
            request.setRawHeader(QByteArrayLiteral("Content-Type"), contentType);
            auto* reply = manager.post(request, body);
            co_await qCoro(reply).waitForFinished();
            const auto cleanup = qScopeGuard([reply] { reply->deleteLater(); });
            static_cast<void>(cleanup);
            const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            co_return HttpResult{.statusCode = status,
                                 .body = reply->readAll(),
                                 .authenticateHeader = {},
                                 .error = status == 0 ? reply->errorString() : QString{},
                                 .finalUrl = reply->url()};
        }

        [[nodiscard]] QCoro::Task<HttpResult> remove(QNetworkAccessManager& manager,
                                                     const QUrl& url, const QByteArray& bearer)
        {
            QNetworkRequest request{url};
            request.setTransferTimeout(30'000);
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                 QNetworkRequest::SameOriginRedirectPolicy);
            request.setRawHeader(QByteArrayLiteral("Accept"),
                                 QByteArrayLiteral("application/json"));
            request.setRawHeader(QByteArrayLiteral("Authorization"),
                                 QByteArrayLiteral("Bearer ") + bearer);
            auto* reply = manager.deleteResource(request);
            co_await qCoro(reply).waitForFinished();
            const auto cleanup = qScopeGuard([reply] { reply->deleteLater(); });
            static_cast<void>(cleanup);
            const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            co_return HttpResult{.statusCode = status,
                                 .body = reply->readAll(),
                                 .authenticateHeader = {},
                                 .error = status == 0 ? reply->errorString() : QString{},
                                 .finalUrl = reply->url()};
        }

        template <typename Value>
        [[nodiscard]] std::optional<Value> parseJson(const QByteArray& body)
        {
            Value value;
            const auto error = glz::read<glz::opts{.error_on_unknown_keys = false}>(
                value, std::string_view{body.constData(), static_cast<std::size_t>(body.size())});
            return error ? std::nullopt : std::optional<Value>{std::move(value)};
        }

        struct ProtectedResourceMetadataCandidate
        {
            QUrl metadataUrl;
            QString expectedResource;
        };

        [[nodiscard]] QString resourceMetadataFromChallenge(const QByteArray& header);

        [[nodiscard]] QUrl canonicalHttpsResourceUrl(QUrl url)
        {
            if (url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0 &&
                url.port() == 443)
                url.setPort(-1);
            return url.adjusted(QUrl::NormalizePathSegments);
        }

        [[nodiscard]] std::vector<ProtectedResourceMetadataCandidate>
        protectedResourceMetadataUrls(const QUrl& sessionUrl, const QByteArray& challenge)
        {
            const auto canonicalSessionUrl = canonicalHttpsResourceUrl(sessionUrl);
            const auto sessionResource = canonicalSessionUrl.toString(QUrl::FullyEncoded);
            const auto advertised = QUrl{resourceMetadataFromChallenge(challenge)};
            if (!advertised.isEmpty())
            {
                if (!detail::isSecureOAuthUrl(advertised))
                    return {};
                return {{.metadataUrl = advertised, .expectedResource = sessionResource}};
            }

            QUrl origin;
            origin.setScheme(canonicalSessionUrl.scheme());
            origin.setHost(canonicalSessionUrl.host());
            origin.setPort(canonicalSessionUrl.port());
            QUrl metadataUrl = origin;
            metadataUrl.setPath(QStringLiteral("/.well-known/oauth-protected-resource"));

            std::vector<ProtectedResourceMetadataCandidate> candidates;
            if (!canonicalSessionUrl.path().isEmpty() &&
                canonicalSessionUrl.path() != QStringLiteral("/"))
            {
                auto resourcePath = canonicalSessionUrl.path();
                if (!resourcePath.startsWith(QLatin1Char('/')))
                    resourcePath.prepend(QLatin1Char('/'));
                auto pathUrl = metadataUrl;
                pathUrl.setPath(metadataUrl.path() + resourcePath);
                candidates.push_back(
                    {.metadataUrl = std::move(pathUrl), .expectedResource = sessionResource});
            }
            candidates.push_back({.metadataUrl = std::move(metadataUrl),
                                  .expectedResource = origin.toString(QUrl::FullyEncoded)});
            return candidates;
        }

        [[nodiscard]] QString authorizationMetadataUrl(const QString& issuer)
        {
            QUrl url{issuer};
            auto path = url.path();
            if (path.endsWith(QLatin1Char('/')))
                path.chop(1);
            url.setPath(QStringLiteral("/.well-known/oauth-authorization-server") + path);
            return url.toString();
        }

        [[nodiscard]] QString resourceMetadataFromChallenge(const QByteArray& header)
        {
            static const QRegularExpression expression{
                QStringLiteral("(?:^|[,\\s])resource_metadata=\"([^\"]+)\"")};
            const auto match = expression.match(QString::fromLatin1(header));
            return match.hasMatch() ? match.captured(1) : QString{};
        }

        [[nodiscard]] QString randomUrlSafe(const int bytes)
        {
            QByteArray value(bytes, Qt::Uninitialized);
            for (int index = 0; index < bytes; ++index)
                value[index] = static_cast<char>(QRandomGenerator::system()->bounded(256));
            return QString::fromLatin1(
                value.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
        }

        [[nodiscard]] bool isSecureServerUrl(const QUrl& url)
        {
            return detail::isSecureOAuthUrl(url);
        }

        [[nodiscard]] std::optional<QString> oauthErrorCode(const QByteArray& body)
        {
            const auto error = parseJson<detail::OAuthErrorResponse>(body);
            return error.has_value() && error->error.has_value()
                       ? std::optional{QString::fromStdString(*error->error)}
                       : std::nullopt;
        }

        [[nodiscard]] QString oauthErrorText(const QByteArray& body)
        {
            const auto error = parseJson<detail::OAuthErrorResponse>(body);
            if (!error.has_value())
                return QStringLiteral("unparseable error response");
            const auto code = error->error.has_value() ? QString::fromStdString(*error->error)
                                                       : QStringLiteral("unspecified_error");
            const auto description =
                error->errorDescription.has_value()
                    ? QString::fromStdString(*error->errorDescription).simplified().left(500)
                : error->detail.has_value()
                    ? QString::fromStdString(*error->detail).simplified().left(500)
                : error->title.has_value()
                    ? QString::fromStdString(*error->title).simplified().left(500)
                    : QString{};
            return description.isEmpty() ? code : code + QStringLiteral(": ") + description;
        }

        [[nodiscard]] bool validBearerTokenResponse(const detail::TokenResponse& token)
        {
            if (token.accessToken.empty() || !token.tokenType.has_value() ||
                QString::fromStdString(*token.tokenType)
                        .compare(QStringLiteral("Bearer"), Qt::CaseInsensitive) != 0)
                return false;
            return !token.expiresIn.has_value() || *token.expiresIn > 0;
        }

        [[nodiscard]] QString effectiveScope(const detail::TokenResponse& token,
                                             const QString& fallback)
        {
            return token.scope.has_value() ? QString::fromStdString(*token.scope).simplified()
                                           : fallback.simplified();
        }

        [[nodiscard]] QStringList strings(const std::vector<std::string>& values)
        {
            QStringList result;
            result.reserve(static_cast<qsizetype>(values.size()));
            for (const auto& value : values)
                result.push_back(QString::fromStdString(value));
            return result;
        }

        [[nodiscard]] bool contains(const QStringList& values, const QString& value)
        {
            return values.contains(value, Qt::CaseSensitive);
        }

        [[nodiscard]] QStringList requestedScopes(const QStringList& advertised)
        {
            QStringList result;
            const QStringList desired{
                QStringLiteral("urn:ietf:params:oauth:scope:mail"),
                QStringLiteral("urn:ietf:params:oauth:scope:contacts"),
                QStringLiteral("urn:ietf:params:oauth:scope:calendars"),
                QStringLiteral("urn:ietf:params:jmap:core"),
                QStringLiteral("urn:ietf:params:jmap:mail"),
                QStringLiteral("urn:ietf:params:jmap:submission"),
                QStringLiteral("urn:ietf:params:jmap:contacts"),
                QStringLiteral("offline_access"),
            };
            for (const auto& scope : desired)
            {
                if (advertised.contains(scope) && !result.contains(scope))
                    result.push_back(scope);
            }
            return result;
        }

        [[nodiscard]] std::vector<javelin::app::OnboardingFeature>
        sessionFeatures(const api::Session& session)
        {
            using Feature = javelin::app::OnboardingFeature;
            using Kind = javelin::app::OnboardingFeatureKind;
            const bool push =
                session.capabilities.websocket.has_value() || session.eventSourceUrl.has_value();
            const auto contactsAccount =
                session.primaryAccounts.contactsAccountId.has_value()
                    ? session.accounts.find(*session.primaryAccounts.contactsAccountId)
                    : session.accounts.end();
            const bool contacts = session.capabilities.contacts &&
                                  contactsAccount != session.accounts.end() &&
                                  contactsAccount->second.accountCapabilities.contacts.has_value();
            const auto calendarsAccount =
                session.primaryAccounts.calendarsAccountId.has_value()
                    ? session.accounts.find(*session.primaryAccounts.calendarsAccountId)
                    : session.accounts.end();
            const bool calendars =
                session.capabilities.calendars && calendarsAccount != session.accounts.end() &&
                calendarsAccount->second.accountCapabilities.calendars.has_value();
            return {
                Feature{.kind = Kind::Jmap, .available = session.capabilities.core, .detail = {}},
                Feature{.kind = Kind::Mail, .available = session.capabilities.mail, .detail = {}},
                Feature{.kind = Kind::Sending,
                        .available = session.capabilities.submission,
                        .detail = {}},
                Feature{.kind = Kind::Contacts, .available = contacts, .detail = {}},
                Feature{.kind = Kind::Calendars, .available = calendars, .detail = {}},
                Feature{.kind = Kind::Sieve, .available = session.capabilities.sieve, .detail = {}},
                Feature{.kind = Kind::Push, .available = push, .detail = {}},
            };
        }

        [[nodiscard]] std::vector<javelin::app::OnboardingFeature> protectedSessionFeatures()
        {
            using Feature = javelin::app::OnboardingFeature;
            using Kind = javelin::app::OnboardingFeatureKind;
            const auto pending = QStringLiteral("Checked after sign-in");
            return {
                Feature{.kind = Kind::Jmap, .available = true, .detail = {}},
                Feature{.kind = Kind::Mail, .available = false, .detail = pending},
                Feature{.kind = Kind::Sending, .available = false, .detail = pending},
                Feature{.kind = Kind::Contacts, .available = false, .detail = pending},
                Feature{.kind = Kind::Calendars, .available = false, .detail = pending},
                Feature{.kind = Kind::Sieve, .available = false, .detail = pending},
                Feature{.kind = Kind::Push, .available = false, .detail = pending},
            };
        }

        void appendOAuthFeatures(javelin::app::AccountDiscoveryResult& result,
                                 const bool dynamicRegistration)
        {
            using Feature = javelin::app::OnboardingFeature;
            using Kind = javelin::app::OnboardingFeatureKind;
            result.features.push_back(Feature{.kind = Kind::OAuth,
                                              .available = !result.authorizationEndpoint.isEmpty(),
                                              .detail = {}});
            result.features.push_back(Feature{.kind = Kind::DynamicClientRegistration,
                                              .available = dynamicRegistration,
                                              .detail = {}});
            result.features.push_back(
                Feature{.kind = Kind::OfflineAccess,
                        .available = contains(result.scopes, QStringLiteral("offline_access")),
                        .detail = {}});
        }

        [[nodiscard]] QByteArray formBody(const std::vector<std::pair<QString, QString>>& fields)
        {
            QByteArray result;
            for (const auto& [name, value] : fields)
            {
                if (!result.isEmpty())
                    result += '&';
                result += QUrl::toPercentEncoding(name);
                result += '=';
                result += QUrl::toPercentEncoding(value);
            }
            return result;
        }

        [[nodiscard]] QCoro::Task<std::optional<QString>>
        deleteClientRegistration(QNetworkAccessManager& manager, const QString& clientUri,
                                 const QString& accessToken)
        {
            if (clientUri.isEmpty() && accessToken.isEmpty())
                co_return std::nullopt;
            if (clientUri.isEmpty() || accessToken.isEmpty() ||
                !detail::isSecureOAuthUrl(QUrl{clientUri}))
                co_return QStringLiteral(
                    "OAuth client-registration cleanup information is incomplete.");

            const auto response = co_await remove(manager, QUrl{clientUri}, accessToken.toUtf8());
            if (response.statusCode >= 200 && response.statusCode < 300)
                co_return std::nullopt;
            co_return response.error.isEmpty() ? oauthErrorText(response.body) : response.error;
        }

        [[nodiscard]] javelin::app::OAuthRefreshResult
        refreshError(QString error, const javelin::app::OAuthRefreshFailureKind kind)
        {
            return {.succeeded = false,
                    .error = std::move(error),
                    .failureKind = kind,
                    .accessToken = {},
                    .refreshToken = {},
                    .scope = {},
                    .expiresAtEpochSeconds = 0};
        }

        [[nodiscard]] javelin::app::AccountAuthenticationResult authenticationError(QString error)
        {
            return {.succeeded = false,
                    .error = std::move(error),
                    .sessionUrl = {},
                    .accessToken = {},
                    .refreshToken = {},
                    .tokenEndpoint = {},
                    .clientId = {},
                    .issuer = {},
                    .resourceUrl = {},
                    .scope = {},
                    .revocationEndpoint = {},
                    .registrationClientUri = {},
                    .registrationAccessToken = {},
                    .expiresAtEpochSeconds = 0,
                    .features = {}};
        }
    } // namespace

    AccountOnboardingService::AccountOnboardingService(QNetworkAccessManager& networkAccessManager)
        : m_networkAccessManager(networkAccessManager)
    {
    }

    QCoro::Task<void> AccountOnboardingService::pruneExpiredFlows()
    {
        constexpr qint64 flowLifetimeSeconds = 10 * 60;
        const auto cutoff = QDateTime::currentSecsSinceEpoch() - flowLifetimeSeconds;
        std::vector<std::pair<QString, QString>> registrations;
        std::erase_if(m_pendingFlows,
                      [cutoff, &registrations](const auto& entry)
                      {
                          if (entry.second.createdAtEpochSeconds >= cutoff)
                              return false;
                          registrations.emplace_back(entry.second.registrationClientUri,
                                                     entry.second.registrationAccessToken);
                          return true;
                      });
        for (const auto& [clientUri, accessToken] : registrations)
        {
            if (const auto error = co_await deleteClientRegistration(m_networkAccessManager,
                                                                     clientUri, accessToken))
                qCWarning(oauthLog).noquote()
                    << "OAuth expired-flow registration cleanup failed" << *error;
        }
    }

    QCoro::Task<javelin::app::AccountDiscoveryResult>
    AccountOnboardingService::discover(javelin::app::AccountDiscoveryRequest request)
    {
        javelin::app::AccountDiscoveryResult result;
        result.emailAddress = request.emailAddress.trimmed();
        const auto sessionUrl =
            co_await api::discoverSessionUrl({}, result.emailAddress.toStdString());
        if (!sessionUrl.has_value())
        {
            result.error =
                QStringLiteral("Enter a complete email address so Javelin can find your server.");
            co_return result;
        }
        result.sessionUrl = sessionUrl->toString();

        const auto sessionResponse =
            co_await get(m_networkAccessManager, *sessionUrl, {}, RedirectTrust::Resource);
        if (sessionResponse.statusCode == 200)
        {
            const auto parsed = api::parseSession(sessionResponse.body.toStdString());
            if (!parsed.ok())
            {
                result.error =
                    QStringLiteral("The server responded, but its JMAP information was invalid.");
                co_return result;
            }
            result.features = sessionFeatures(*parsed.session);
        }
        else if (sessionResponse.statusCode != 401)
        {
            result.error =
                sessionResponse.error.isEmpty()
                    ? QStringLiteral("No compatible JMAP service was found for this address.")
                    : QStringLiteral("Javelin could not reach the mail server.");
            co_return result;
        }
        else
        {
            result.features = protectedSessionFeatures();
        }

        std::optional<detail::ProtectedResourceMetadata> resource;
        QString returnedResource;
        const auto protectedResourceUrl =
            sessionResponse.finalUrl.isEmpty() ? *sessionUrl : sessionResponse.finalUrl;
        for (const auto& candidate : protectedResourceMetadataUrls(
                 protectedResourceUrl, sessionResponse.authenticateHeader))
        {
            const auto response = co_await get(m_networkAccessManager, candidate.metadataUrl);
            if (response.statusCode != 200)
                continue;

            auto parsed = parseJson<detail::ProtectedResourceMetadata>(response.body);
            if (!parsed.has_value() || parsed->authorizationServers.empty())
                continue;

            const auto candidateResource = QString::fromStdString(parsed->resource);
            if (!detail::resourceMetadataMatches(candidateResource, candidate.expectedResource) ||
                !isSecureServerUrl(QUrl{candidateResource}))
                continue;

            returnedResource = candidateResource;
            resource = std::move(parsed);
            break;
        }
        if (!resource.has_value())
        {
            appendOAuthFeatures(result, false);
            result.succeeded = true;
            co_return result;
        }

        result.resourceUrl = returnedResource;
        result.scopes = strings(resource->scopesSupported);
        result.issuer = QString::fromStdString(resource->authorizationServers.front());
        const QUrl issuerUrl{result.issuer};
        if (!detail::isSecureOAuthUrl(issuerUrl) || issuerUrl.hasQuery())
        {
            appendOAuthFeatures(result, false);
            result.succeeded = true;
            co_return result;
        }
        const auto metadataResponse =
            co_await get(m_networkAccessManager, QUrl{authorizationMetadataUrl(result.issuer)});
        const auto metadata = parseJson<detail::AuthorizationServerMetadata>(metadataResponse.body);
        const auto authorizationEndpoint =
            metadata.has_value() ? QString::fromStdString(metadata->authorizationEndpoint)
                                 : QString{};
        const auto tokenEndpoint =
            metadata.has_value() ? QString::fromStdString(metadata->tokenEndpoint) : QString{};
        if (metadataResponse.statusCode == 200 && metadata.has_value() &&
            metadata->issuer == result.issuer &&
            detail::isSecureOAuthUrl(QUrl{authorizationEndpoint}) &&
            detail::isSecureOAuthUrl(QUrl{tokenEndpoint}))
        {
            result.issuer = QString::fromStdString(metadata->issuer);
            result.authorizationEndpoint = authorizationEndpoint;
            result.tokenEndpoint = tokenEndpoint;
            if (metadata->revocationEndpoint.has_value())
            {
                const auto revocationEndpoint =
                    QString::fromStdString(*metadata->revocationEndpoint);
                const bool publicRevocation =
                    metadata->revocationEndpointAuthMethodsSupported.empty() ||
                    std::ranges::contains(metadata->revocationEndpointAuthMethodsSupported,
                                          std::string{"none"});
                if (publicRevocation && detail::isSecureOAuthUrl(QUrl{revocationEndpoint}))
                    result.revocationEndpoint = revocationEndpoint;
            }
            if (result.scopes.isEmpty())
                result.scopes = strings(metadata->scopesSupported);
            const bool openPublicClient =
                metadata->registrationEndpoint.has_value() &&
                std::ranges::contains(metadata->responseTypesSupported, std::string{"code"}) &&
                std::ranges::contains(metadata->grantTypesSupported,
                                      std::string{"authorization_code"}) &&
                std::ranges::contains(metadata->grantTypesSupported,
                                      std::string{"refresh_token"}) &&
                std::ranges::contains(metadata->tokenEndpointAuthMethodsSupported,
                                      std::string{"none"}) &&
                std::ranges::contains(metadata->codeChallengeMethodsSupported,
                                      std::string{"S256"}) &&
                metadata->authorizationResponseIssParameterSupported &&
                !requestedScopes(result.scopes).isEmpty();
            if (openPublicClient)
            {
                const auto registrationEndpoint =
                    QString::fromStdString(*metadata->registrationEndpoint);
                if (detail::isSecureOAuthUrl(QUrl{registrationEndpoint}))
                {
                    result.registrationEndpoint = registrationEndpoint;
                    result.refreshTokensSupported = true;
                }
            }
        }
        appendOAuthFeatures(result, !result.registrationEndpoint.isEmpty());
        result.succeeded = true;
        co_return result;
    }

    QCoro::Task<javelin::app::OAuthStartResult>
    AccountOnboardingService::startOAuth(javelin::app::OAuthStartRequest request)
    {
        javelin::app::OAuthStartResult result;
        co_await pruneExpiredFlows();
        constexpr std::size_t maximumPendingFlows = 8;
        if (m_pendingFlows.size() >= maximumPendingFlows)
        {
            result.error = QStringLiteral(
                "Too many browser sign-in attempts are still open. Finish or retry one of them.");
            co_return result;
        }
        if (!request.discovery.succeeded || request.discovery.registrationEndpoint.isEmpty() ||
            request.discovery.resourceUrl.isEmpty() || request.redirectUri.isEmpty() ||
            !isSecureServerUrl(QUrl{request.discovery.registrationEndpoint}) ||
            !isSecureServerUrl(QUrl{request.discovery.authorizationEndpoint}) ||
            !isSecureServerUrl(QUrl{request.discovery.tokenEndpoint}) ||
            !isSecureServerUrl(QUrl{request.discovery.resourceUrl}))
        {
            result.error = QStringLiteral("This server requires manual application registration.");
            co_return result;
        }

        const auto scopes = requestedScopes(request.discovery.scopes);
        const auto registeredRedirectUri = detail::registrationRedirectUri(request.redirectUri);
        detail::RegistrationRequest registration{
            .redirectUris = {registeredRedirectUri.toStdString()},
            .clientName = "Javelin Mail",
            .tokenEndpointAuthMethod = "none",
            .grantTypes = {"authorization_code", "refresh_token"},
            .responseTypes = {"code"},
            .scope = scopes.join(QLatin1Char(' ')).toStdString(),
            .clientUri = "https://github.com/ripdog/Javelin-Mail",
            .logoUri = "https://github.com/ripdog/Javelin-Mail/raw/master/res/icon.svg",
            .tosUri = "https://github.com/ripdog/Javelin-Mail/blob/master/LICENSE",
            .policyUri = "https://github.com/ripdog/Javelin-Mail/blob/master/PRIVACY.md",
            .softwareId = "5f414a46-fef7-53b4-b82a-bdc4818fe0dc",
            .softwareVersion = QCoreApplication::applicationVersion().toStdString(),
        };
        std::string registrationJson;
        if (glz::write_json(registration, registrationJson))
        {
            result.error = QStringLiteral("Javelin could not prepare the sign-in request.");
            co_return result;
        }
        const auto registrationResponse = co_await post(
            m_networkAccessManager, QUrl{request.discovery.registrationEndpoint},
            QByteArrayLiteral("application/json"), QByteArray::fromStdString(registrationJson));
        const auto registered = parseJson<detail::RegistrationResponse>(registrationResponse.body);
        const bool usableRegistration =
            registered.has_value() && !registered->clientId.empty() &&
            (!registered->tokenEndpointAuthMethod.has_value() ||
             *registered->tokenEndpointAuthMethod == "none") &&
            (!registered->redirectUris.has_value() ||
             std::ranges::contains(*registered->redirectUris, registeredRedirectUri.toStdString()));
        if (registrationResponse.statusCode < 200 || registrationResponse.statusCode >= 300 ||
            !usableRegistration)
        {
            qCWarning(oauthLog).noquote()
                << "OAuth dynamic registration rejected"
                << QUrl{request.discovery.registrationEndpoint}.host() << "status"
                << registrationResponse.statusCode
                << (registrationResponse.error.isEmpty() ? oauthErrorText(registrationResponse.body)
                                                         : registrationResponse.error);
            result.error = QStringLiteral("The server did not accept automatic app registration. "
                                          "You can still sign in manually.");
            co_return result;
        }

        const auto registrationClientUri =
            registered->registrationClientUri.has_value()
                ? QString::fromStdString(*registered->registrationClientUri)
                : QString{};
        const auto registrationAccessToken =
            registered->registrationAccessToken.has_value()
                ? QString::fromStdString(*registered->registrationAccessToken)
                : QString{};
        const bool manageableRegistration = !registrationClientUri.isEmpty() &&
                                            !registrationAccessToken.isEmpty() &&
                                            detail::isSecureOAuthUrl(QUrl{registrationClientUri});
        PendingOAuthFlow flow{
            .discovery = std::move(request.discovery),
            .redirectUri = std::move(request.redirectUri),
            .clientId = QString::fromStdString(registered->clientId),
            .codeVerifier = randomUrlSafe(48),
            .state = randomUrlSafe(32),
            .registrationClientUri = manageableRegistration ? registrationClientUri : QString{},
            .registrationAccessToken = manageableRegistration ? registrationAccessToken : QString{},
            .createdAtEpochSeconds = QDateTime::currentSecsSinceEpoch(),
        };
        const auto challenge = QString::fromLatin1(
            QCryptographicHash::hash(flow.codeVerifier.toLatin1(), QCryptographicHash::Sha256)
                .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
        QUrl authorizationUrl{flow.discovery.authorizationEndpoint};
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
        query.addQueryItem(QStringLiteral("client_id"), flow.clientId);
        query.addQueryItem(QStringLiteral("redirect_uri"), flow.redirectUri);
        if (!scopes.isEmpty())
            query.addQueryItem(QStringLiteral("scope"), scopes.join(QLatin1Char(' ')));
        query.addQueryItem(QStringLiteral("state"), flow.state);
        query.addQueryItem(QStringLiteral("code_challenge"), challenge);
        query.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
        query.addQueryItem(QStringLiteral("resource"), flow.discovery.resourceUrl);
        query.addQueryItem(QStringLiteral("login_hint"), flow.discovery.emailAddress);
        authorizationUrl.setQuery(query);

        result.flowId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        result.authorizationUrl = authorizationUrl.toString();
        result.callbackState = flow.state;
        m_pendingFlows.insert_or_assign(result.flowId, std::move(flow));
        result.succeeded = true;
        co_return result;
    }

    QCoro::Task<javelin::app::AccountAuthenticationResult>
    AccountOnboardingService::finishOAuth(javelin::app::OAuthFinishRequest request)
    {
        co_await pruneExpiredFlows();
        const auto found = m_pendingFlows.find(request.flowId);
        if (found == m_pendingFlows.end())
        {
            qCWarning(oauthLog) << "Rejected OAuth callback for an expired flow";
            co_return authenticationError(
                QStringLiteral("This sign-in attempt has expired. Please try again."));
        }
        const auto& pending = found->second;

        if (request.code.isEmpty())
        {
            qCWarning(oauthLog) << "OAuth callback did not include an authorization code";
            co_return authenticationError(
                QStringLiteral("The mail service did not return an authorization code."));
        }
        if (request.state != pending.state)
        {
            qCWarning(oauthLog) << "OAuth callback state did not match the pending flow";
            co_return authenticationError(QStringLiteral(
                "Javelin could not verify that this browser response belongs to the current "
                "sign-in attempt."));
        }
        if (request.issuer.isEmpty())
        {
            qCWarning(oauthLog).noquote()
                << "OAuth callback did not include iss; expected" << pending.discovery.issuer;
            co_return authenticationError(QStringLiteral(
                "The mail service did not identify the authorization server in its response."));
        }
        if (request.issuer != pending.discovery.issuer)
        {
            qCWarning(oauthLog).noquote()
                << "OAuth callback issuer mismatch; expected" << pending.discovery.issuer
                << "but got" << request.issuer;
            co_return authenticationError(
                QStringLiteral("The authorization response came from an unexpected server."));
        }

        auto flow = std::move(found->second);
        m_pendingFlows.erase(found);
        const auto failAndCleanup =
            [this, &flow](QString error) -> QCoro::Task<javelin::app::AccountAuthenticationResult>
        {
            if (const auto cleanupError = co_await deleteClientRegistration(
                    m_networkAccessManager, flow.registrationClientUri,
                    flow.registrationAccessToken))
                qCWarning(oauthLog).noquote()
                    << "OAuth failed-flow registration cleanup failed" << *cleanupError;
            co_return authenticationError(std::move(error));
        };
        const auto body = formBody({
            {QStringLiteral("grant_type"), QStringLiteral("authorization_code")},
            {QStringLiteral("code"), request.code},
            {QStringLiteral("client_id"), flow.clientId},
            {QStringLiteral("redirect_uri"), flow.redirectUri},
            {QStringLiteral("code_verifier"), flow.codeVerifier},
            {QStringLiteral("resource"), flow.discovery.resourceUrl},
        });
        const auto tokenResponse =
            co_await post(m_networkAccessManager, QUrl{flow.discovery.tokenEndpoint},
                          QByteArrayLiteral("application/x-www-form-urlencoded"), body);
        const auto token = parseJson<detail::TokenResponse>(tokenResponse.body);
        if (tokenResponse.statusCode < 200 || tokenResponse.statusCode >= 300 ||
            !token.has_value() || !validBearerTokenResponse(*token))
        {
            const auto detail = token.has_value() && token->errorDescription.has_value()
                                    ? QString::fromStdString(*token->errorDescription)
                                    : QStringLiteral("The server did not issue an access token.");
            co_return co_await failAndCleanup(detail);
        }

        if (!token->refreshToken.has_value() || token->refreshToken->empty())
            co_return co_await failAndCleanup(QStringLiteral(
                "The mail service did not issue the refresh token required for background mail."));

        qCInfo(oauthLog).noquote()
            << "OAuth token response accepted"
            << "accessTokenPresent=" << !token->accessToken.empty() << "refreshTokenPresent="
            << (token->refreshToken.has_value() && !token->refreshToken->empty())
            << "expiresInPresent=" << token->expiresIn.has_value()
            << "tokenEndpointHost=" << QUrl{flow.discovery.tokenEndpoint}.host();

        const auto sessionResponse =
            co_await get(m_networkAccessManager, QUrl{flow.discovery.sessionUrl},
                         QByteArray::fromStdString(token->accessToken));
        const auto session = api::parseSession(sessionResponse.body.toStdString());
        if (sessionResponse.statusCode != 200 || !session.ok())
            co_return co_await failAndCleanup(
                QStringLiteral("Sign-in succeeded, but the JMAP account could not be verified."));

        auto result = javelin::app::AccountAuthenticationResult{
            .succeeded = true,
            .error = {},
            .sessionUrl = flow.discovery.sessionUrl,
            .accessToken = QString::fromStdString(token->accessToken),
            .refreshToken = token->refreshToken.has_value()
                                ? QString::fromStdString(*token->refreshToken)
                                : QString{},
            .tokenEndpoint = flow.discovery.tokenEndpoint,
            .clientId = flow.clientId,
            .issuer = flow.discovery.issuer,
            .resourceUrl = flow.discovery.resourceUrl,
            .scope = effectiveScope(*token,
                                    requestedScopes(flow.discovery.scopes).join(QLatin1Char(' '))),
            .revocationEndpoint = flow.discovery.revocationEndpoint,
            .registrationClientUri = flow.registrationClientUri,
            .registrationAccessToken = flow.registrationAccessToken,
            .expiresAtEpochSeconds = token->expiresIn.has_value()
                                         ? QDateTime::currentSecsSinceEpoch() + *token->expiresIn
                                         : 0,
            .features = sessionFeatures(*session.session),
        };
        qCInfo(oauthLog).noquote() << "OAuth authorization completed"
                                   << "accessTokenPresent=" << !result.accessToken.isEmpty()
                                   << "refreshTokenPresent=" << !result.refreshToken.isEmpty()
                                   << "clientIdPresent=" << !result.clientId.isEmpty()
                                   << "tokenEndpointHost=" << QUrl{result.tokenEndpoint}.host()
                                   << "expiresAtEpochSeconds=" << result.expiresAtEpochSeconds;
        co_return result;
    }

    QCoro::Task<javelin::app::AccountAuthenticationResult>
    AccountOnboardingService::authenticateManually(
        javelin::app::ManualAuthenticationRequest request)
    {
        auto sessionUrl = request.sessionUrl.trimmed();
        if (sessionUrl.isEmpty())
        {
            const auto discovered =
                co_await api::discoverSessionUrl({}, request.emailAddress.trimmed().toStdString());
            if (!discovered.has_value())
                co_return authenticationError(
                    QStringLiteral("Javelin could not find the JMAP server."));
            sessionUrl = discovered->toString();
        }
        if (!isSecureServerUrl(QUrl{sessionUrl}))
            co_return authenticationError(
                QStringLiteral("Use an HTTPS address for the JMAP server."));
        const auto response =
            co_await get(m_networkAccessManager, QUrl{sessionUrl}, request.accessToken.toUtf8());
        const auto session = api::parseSession(response.body.toStdString());
        if (response.statusCode != 200 || !session.ok())
            co_return authenticationError(
                QStringLiteral("Those details could not be used to open the JMAP account."));
        co_return javelin::app::AccountAuthenticationResult{
            .succeeded = true,
            .error = {},
            .sessionUrl = sessionUrl,
            .accessToken = request.accessToken,
            .refreshToken = {},
            .tokenEndpoint = {},
            .clientId = {},
            .issuer = {},
            .resourceUrl = {},
            .scope = {},
            .revocationEndpoint = {},
            .registrationClientUri = {},
            .registrationAccessToken = {},
            .expiresAtEpochSeconds = 0,
            .features = sessionFeatures(*session.session),
        };
    }

    QCoro::Task<javelin::app::OAuthRefreshResult>
    AccountOnboardingService::refreshOAuth(javelin::app::OAuthRefreshRequest request)
    {
        using FailureKind = javelin::app::OAuthRefreshFailureKind;
        if (!detail::isUsableOAuthRefreshRequest(request))
            co_return refreshError(QStringLiteral("OAuth refresh information is incomplete."),
                                   FailureKind::ReauthenticationRequired);
        std::vector<std::pair<QString, QString>> fields{
            {QStringLiteral("grant_type"), QStringLiteral("refresh_token")},
            {QStringLiteral("refresh_token"), request.refreshToken},
            {QStringLiteral("client_id"), request.clientId},
        };
        if (!request.resourceUrl.isEmpty())
            fields.emplace_back(QStringLiteral("resource"), request.resourceUrl);
        const auto body = formBody(fields);
        const auto response =
            co_await post(m_networkAccessManager, QUrl{request.tokenEndpoint},
                          QByteArrayLiteral("application/x-www-form-urlencoded"), body);
        const auto token = parseJson<detail::TokenResponse>(response.body);
        if (response.statusCode < 200 || response.statusCode >= 300)
        {
            const auto code = oauthErrorCode(response.body);
            co_return refreshError(response.error.isEmpty() ? oauthErrorText(response.body)
                                                            : response.error,
                                   detail::refreshFailureKind(code.value_or(QString{})));
        }
        if (!token.has_value() || !validBearerTokenResponse(*token))
            co_return refreshError(QStringLiteral("The account session response was invalid."),
                                   FailureKind::Transient);

        co_return javelin::app::OAuthRefreshResult{
            .succeeded = true,
            .error = {},
            .failureKind = FailureKind::None,
            .accessToken = QString::fromStdString(token->accessToken),
            .refreshToken = token->refreshToken.has_value()
                                ? QString::fromStdString(*token->refreshToken)
                                : request.refreshToken,
            .scope = effectiveScope(*token, request.scope),
            .expiresAtEpochSeconds = token->expiresIn.has_value()
                                         ? QDateTime::currentSecsSinceEpoch() + *token->expiresIn
                                         : 0,
        };
    }

    QCoro::Task<javelin::app::OAuthRevocationResult>
    AccountOnboardingService::revokeOAuth(javelin::app::OAuthRevocationRequest request)
    {
        const bool hasTokens = !request.accessToken.isEmpty() || !request.refreshToken.isEmpty();
        const bool hasRegistration =
            !request.registrationClientUri.isEmpty() || !request.registrationAccessToken.isEmpty();
        javelin::app::OAuthRevocationResult result{
            .attempted = hasTokens || hasRegistration,
            .succeeded = true,
            .error = {},
        };
        if (!result.attempted)
            co_return result;

        QStringList failures;
        if (hasTokens)
        {
            if (request.clientId.isEmpty() || request.revocationEndpoint.isEmpty() ||
                !isSecureServerUrl(QUrl{request.revocationEndpoint}))
            {
                failures.push_back(QStringLiteral("OAuth revocation information is incomplete."));
            }
            else
            {
                const auto revoke = [&](const QString& token,
                                        const QString& tokenTypeHint) -> QCoro::Task<void>
                {
                    if (token.isEmpty())
                        co_return;
                    const auto response =
                        co_await post(m_networkAccessManager, QUrl{request.revocationEndpoint},
                                      QByteArrayLiteral("application/x-www-form-urlencoded"),
                                      formBody({
                                          {QStringLiteral("token"), token},
                                          {QStringLiteral("token_type_hint"), tokenTypeHint},
                                          {QStringLiteral("client_id"), request.clientId},
                                      }));
                    if (response.statusCode < 200 || response.statusCode >= 300)
                    {
                        failures.push_back(response.error.isEmpty() ? oauthErrorText(response.body)
                                                                    : response.error);
                    }
                };

                co_await revoke(request.refreshToken, QStringLiteral("refresh_token"));
                co_await revoke(request.accessToken, QStringLiteral("access_token"));
            }
        }

        if (hasRegistration)
        {
            if (const auto error = co_await deleteClientRegistration(
                    m_networkAccessManager, request.registrationClientUri,
                    request.registrationAccessToken))
                failures.push_back(*error);
        }

        result.succeeded = failures.isEmpty();
        if (!result.succeeded)
            result.error = failures.join(QStringLiteral("; "));
        co_return result;
    }

    QCoro::Task<javelin::app::OAuthCancelResult>
    AccountOnboardingService::cancelOAuth(javelin::app::OAuthCancelRequest request)
    {
        co_await pruneExpiredFlows();
        const auto found = m_pendingFlows.find(request.flowId);
        if (found == m_pendingFlows.end())
            co_return javelin::app::OAuthCancelResult{};

        auto flow = std::move(found->second);
        m_pendingFlows.erase(found);
        if (const auto error = co_await deleteClientRegistration(
                m_networkAccessManager, flow.registrationClientUri, flow.registrationAccessToken))
        {
            co_return javelin::app::OAuthCancelResult{
                .registrationDeleted = false,
                .error = *error,
            };
        }
        co_return javelin::app::OAuthCancelResult{
            .registrationDeleted = !flow.registrationClientUri.isEmpty(),
            .error = {},
        };
    }
} // namespace javelin::jmap::auth
