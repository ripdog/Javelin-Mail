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
        std::vector<std::string> scopesSupported;
        std::vector<std::string> responseTypesSupported;
        std::vector<std::string> grantTypesSupported;
        std::vector<std::string> tokenEndpointAuthMethodsSupported;
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
        &T::tokenEndpoint, "registration_endpoint", &T::registrationEndpoint, "scopes_supported",
        &T::scopesSupported, "response_types_supported", &T::responseTypesSupported,
        "grant_types_supported", &T::grantTypesSupported, "token_endpoint_auth_methods_supported",
        &T::tokenEndpointAuthMethodsSupported, "code_challenge_methods_supported",
        &T::codeChallengeMethodsSupported, "authorization_response_iss_parameter_supported",
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
    static constexpr auto value = glz::object("client_id", &T::clientId);
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
    static constexpr auto value = glz::object(
        "access_token", &T::accessToken, "refresh_token", &T::refreshToken, "expires_in",
        &T::expiresIn, "error", &T::error, "error_description", &T::errorDescription);
};

namespace javelin::jmap::auth
{
    Q_LOGGING_CATEGORY(oauthLog, "javelin.oauth")

    QString detail::registrationRedirectUri(const QString& callbackUri)
    {
        QUrl redirect{callbackUri};
        if (redirect.scheme() == QStringLiteral("http") &&
            (redirect.host() == QStringLiteral("localhost") ||
             redirect.host() == QStringLiteral("127.0.0.1") ||
             redirect.host() == QStringLiteral("::1")))
        {
            redirect.setHost(QStringLiteral("localhost"));
            redirect.setPort(-1);
        }
        return redirect.toString();
    }

    namespace
    {
        struct HttpResult
        {
            int statusCode = 0;
            QByteArray body;
            QByteArray authenticateHeader;
            QString error;
        };

        [[nodiscard]] QCoro::Task<HttpResult> get(QNetworkAccessManager& manager, const QUrl& url,
                                                  const QByteArray& bearer = {})
        {
            QNetworkRequest request{url};
            request.setTransferTimeout(30'000);
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                 QNetworkRequest::NoLessSafeRedirectPolicy);
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
            };
        }

        [[nodiscard]] QCoro::Task<HttpResult> post(QNetworkAccessManager& manager, const QUrl& url,
                                                   const QByteArray& contentType,
                                                   const QByteArray& body)
        {
            QNetworkRequest request{url};
            request.setTransferTimeout(30'000);
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                 QNetworkRequest::NoLessSafeRedirectPolicy);
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
                                 .error = status == 0 ? reply->errorString() : QString{}};
        }

        template <typename Value>
        [[nodiscard]] std::optional<Value> parseJson(const QByteArray& body)
        {
            Value value;
            const auto error = glz::read<glz::opts{.error_on_unknown_keys = false}>(
                value, std::string_view{body.constData(), static_cast<std::size_t>(body.size())});
            return error ? std::nullopt : std::optional<Value>{std::move(value)};
        }

        [[nodiscard]] QString resourceMetadataFromChallenge(const QByteArray& header);

        [[nodiscard]] QStringList protectedResourceMetadataUrls(const QUrl& sessionUrl,
                                                                const QByteArray& challenge)
        {
            const auto advertised = resourceMetadataFromChallenge(challenge);
            if (!advertised.isEmpty())
                return {advertised};

            QUrl url;
            url.setScheme(sessionUrl.scheme());
            url.setHost(sessionUrl.host());
            url.setPort(sessionUrl.port());
            url.setPath(QStringLiteral("/.well-known/oauth-protected-resource"));
            QStringList candidates;
            if (!sessionUrl.path().isEmpty() && sessionUrl.path() != QStringLiteral("/"))
            {
                auto resourcePath = sessionUrl.path();
                if (!resourcePath.startsWith(QLatin1Char('/')))
                    resourcePath.prepend(QLatin1Char('/'));
                auto pathUrl = url;
                pathUrl.setPath(url.path() + resourcePath);
                candidates.push_back(pathUrl.toString());
            }
            candidates.push_back(url.toString());
            candidates.removeDuplicates();
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
            return url.isValid() && url.scheme() == QStringLiteral("https") &&
                   !url.host().isEmpty();
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
            const bool contacts =
                session.capabilities.contacts && contactsAccount != session.accounts.end() &&
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
                Feature{.kind = Kind::Contacts,
                        .available = contacts,
                        .detail = {}},
                Feature{.kind = Kind::Calendars,
                        .available = calendars,
                        .detail = {}},
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

        [[nodiscard]] javelin::app::AccountAuthenticationResult authenticationError(QString error)
        {
            return {.succeeded = false,
                    .error = std::move(error),
                    .sessionUrl = {},
                    .accessToken = {},
                    .refreshToken = {},
                    .tokenEndpoint = {},
                    .clientId = {},
                    .expiresAtEpochSeconds = 0,
                    .features = {}};
        }
    } // namespace

    AccountOnboardingService::AccountOnboardingService(QNetworkAccessManager& networkAccessManager)
        : m_networkAccessManager(networkAccessManager)
    {
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

        const auto sessionResponse = co_await get(m_networkAccessManager, *sessionUrl);
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

        HttpResult resourceResponse;
        for (const auto& metadataUrl :
             protectedResourceMetadataUrls(*sessionUrl, sessionResponse.authenticateHeader))
        {
            resourceResponse = co_await get(m_networkAccessManager, QUrl{metadataUrl});
            if (resourceResponse.statusCode == 200)
                break;
        }
        if (resourceResponse.statusCode != 200)
        {
            appendOAuthFeatures(result, false);
            result.succeeded = true;
            co_return result;
        }
        const auto resource = parseJson<detail::ProtectedResourceMetadata>(resourceResponse.body);
        if (!resource.has_value() || resource->resource.empty() ||
            resource->authorizationServers.empty() ||
            !isSecureServerUrl(QUrl{QString::fromStdString(resource->resource)}))
        {
            appendOAuthFeatures(result, false);
            result.succeeded = true;
            co_return result;
        }

        result.resourceUrl = QString::fromStdString(resource->resource);
        result.scopes = strings(resource->scopesSupported);
        result.issuer = QString::fromStdString(resource->authorizationServers.front());
        const auto metadataResponse =
            co_await get(m_networkAccessManager, QUrl{authorizationMetadataUrl(result.issuer)});
        const auto metadata = parseJson<detail::AuthorizationServerMetadata>(metadataResponse.body);
        if (metadataResponse.statusCode == 200 && metadata.has_value() &&
            !metadata->authorizationEndpoint.empty() && !metadata->tokenEndpoint.empty() &&
            metadata->issuer == result.issuer)
        {
            result.issuer = QString::fromStdString(metadata->issuer);
            result.authorizationEndpoint = QString::fromStdString(metadata->authorizationEndpoint);
            result.tokenEndpoint = QString::fromStdString(metadata->tokenEndpoint);
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
                result.registrationEndpoint =
                    QString::fromStdString(*metadata->registrationEndpoint);
                result.refreshTokensSupported = true;
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
        detail::RegistrationRequest registration{
            .redirectUris = {detail::registrationRedirectUri(request.redirectUri).toStdString()},
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
        if (registrationResponse.statusCode < 200 || registrationResponse.statusCode >= 300 ||
            !registered.has_value() || registered->clientId.empty())
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

        PendingOAuthFlow flow{
            .discovery = std::move(request.discovery),
            .redirectUri = std::move(request.redirectUri),
            .clientId = QString::fromStdString(registered->clientId),
            .codeVerifier = randomUrlSafe(48),
            .state = randomUrlSafe(32),
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
        m_pendingFlows.insert_or_assign(result.flowId, std::move(flow));
        result.succeeded = true;
        co_return result;
    }

    QCoro::Task<javelin::app::AccountAuthenticationResult>
    AccountOnboardingService::finishOAuth(javelin::app::OAuthFinishRequest request)
    {
        const auto found = m_pendingFlows.find(request.flowId);
        if (found == m_pendingFlows.end())
        {
            qCWarning(oauthLog) << "Rejected OAuth callback for an expired flow";
            co_return authenticationError(
                QStringLiteral("This sign-in attempt has expired. Please try again."));
        }
        auto flow = std::move(found->second);
        m_pendingFlows.erase(found);

        if (request.code.isEmpty())
        {
            qCWarning(oauthLog) << "OAuth callback did not include an authorization code";
            co_return authenticationError(
                QStringLiteral("The mail service did not return an authorization code."));
        }
        if (request.state != flow.state)
        {
            qCWarning(oauthLog) << "OAuth callback state did not match the pending flow";
            co_return authenticationError(QStringLiteral(
                "Javelin could not verify that this browser response belongs to the current "
                "sign-in attempt."));
        }
        if (request.issuer.isEmpty())
        {
            qCWarning(oauthLog).noquote()
                << "OAuth callback did not include iss; expected" << flow.discovery.issuer;
            co_return authenticationError(QStringLiteral(
                "The mail service did not identify the authorization server in its response."));
        }
        if (request.issuer != flow.discovery.issuer)
        {
            qCWarning(oauthLog).noquote() << "OAuth callback issuer mismatch; expected"
                                          << flow.discovery.issuer << "but got" << request.issuer;
            co_return authenticationError(
                QStringLiteral("The authorization response came from an unexpected server."));
        }

        const auto body = formBody({
            {QStringLiteral("grant_type"), QStringLiteral("authorization_code")},
            {QStringLiteral("code"), request.code},
            {QStringLiteral("client_id"), flow.clientId},
            {QStringLiteral("redirect_uri"), flow.redirectUri},
            {QStringLiteral("code_verifier"), flow.codeVerifier},
        });
        const auto tokenResponse =
            co_await post(m_networkAccessManager, QUrl{flow.discovery.tokenEndpoint},
                          QByteArrayLiteral("application/x-www-form-urlencoded"), body);
        const auto token = parseJson<detail::TokenResponse>(tokenResponse.body);
        if (tokenResponse.statusCode < 200 || tokenResponse.statusCode >= 300 ||
            !token.has_value() || token->accessToken.empty())
        {
            const auto detail = token.has_value() && token->errorDescription.has_value()
                                    ? QString::fromStdString(*token->errorDescription)
                                    : QStringLiteral("The server did not issue an access token.");
            co_return authenticationError(detail);
        }

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
            co_return authenticationError(
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
            .expiresAtEpochSeconds = 0,
            .features = sessionFeatures(*session.session),
        };
    }

    QCoro::Task<javelin::app::AccountAuthenticationResult>
    AccountOnboardingService::refreshOAuth(javelin::app::OAuthRefreshRequest request)
    {
        if (request.tokenEndpoint.isEmpty() || request.clientId.isEmpty() ||
            request.refreshToken.isEmpty() || !isSecureServerUrl(QUrl{request.tokenEndpoint}))
            co_return authenticationError(
                QStringLiteral("OAuth refresh information is incomplete."));
        const auto body = formBody({
            {QStringLiteral("grant_type"), QStringLiteral("refresh_token")},
            {QStringLiteral("refresh_token"), request.refreshToken},
            {QStringLiteral("client_id"), request.clientId},
        });
        const auto response =
            co_await post(m_networkAccessManager, QUrl{request.tokenEndpoint},
                          QByteArrayLiteral("application/x-www-form-urlencoded"), body);
        const auto token = parseJson<detail::TokenResponse>(response.body);
        if (response.statusCode < 200 || response.statusCode >= 300 || !token.has_value() ||
            token->accessToken.empty())
            co_return authenticationError(
                QStringLiteral("The account session could not be renewed."));

        co_return javelin::app::AccountAuthenticationResult{
            .succeeded = true,
            .error = {},
            .sessionUrl = request.sessionUrl,
            .accessToken = QString::fromStdString(token->accessToken),
            .refreshToken = token->refreshToken.has_value()
                                ? QString::fromStdString(*token->refreshToken)
                                : request.refreshToken,
            .tokenEndpoint = request.tokenEndpoint,
            .clientId = request.clientId,
            .expiresAtEpochSeconds = token->expiresIn.has_value()
                                         ? QDateTime::currentSecsSinceEpoch() + *token->expiresIn
                                         : 0,
            .features = {},
        };
    }
} // namespace javelin::jmap::auth
