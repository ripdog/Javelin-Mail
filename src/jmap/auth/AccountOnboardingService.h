#pragma once

#include "app/OnboardingTypes.h"

#include <QCoroTask>

#include <QNetworkAccessManager>
#include <QUrl>

#include <unordered_map>

namespace javelin::jmap::auth
{
    namespace detail
    {
        [[nodiscard]] QString registrationRedirectUri(const QString& callbackUri);
        [[nodiscard]] bool isSecureOAuthUrl(const QUrl& url);
        [[nodiscard]] bool resourceMetadataMatches(const QString& returnedResource,
                                                   const QString& expectedResource);
        [[nodiscard]] javelin::app::OAuthRefreshFailureKind
        refreshFailureKind(const QString& oauthErrorCode);
        [[nodiscard]] bool
        isUsableOAuthRefreshRequest(const javelin::app::OAuthRefreshRequest& request);
    } // namespace detail

    class AccountOnboardingService final
    {
      public:
        explicit AccountOnboardingService(QNetworkAccessManager& networkAccessManager);

        [[nodiscard]] QCoro::Task<javelin::app::AccountDiscoveryResult>
        discover(javelin::app::AccountDiscoveryRequest request);
        [[nodiscard]] QCoro::Task<javelin::app::OAuthStartResult>
        startOAuth(javelin::app::OAuthStartRequest request);
        [[nodiscard]] QCoro::Task<javelin::app::AccountAuthenticationResult>
        finishOAuth(javelin::app::OAuthFinishRequest request);
        [[nodiscard]] QCoro::Task<javelin::app::OAuthRefreshResult>
        refreshOAuth(javelin::app::OAuthRefreshRequest request);
        [[nodiscard]] QCoro::Task<javelin::app::OAuthRevocationResult>
        revokeOAuth(javelin::app::OAuthRevocationRequest request);
        [[nodiscard]] QCoro::Task<javelin::app::OAuthCancelResult>
        cancelOAuth(javelin::app::OAuthCancelRequest request);
        [[nodiscard]] QCoro::Task<javelin::app::AccountAuthenticationResult>
        authenticateManually(javelin::app::ManualAuthenticationRequest request);

      private:
        struct PendingOAuthFlow
        {
            javelin::app::AccountDiscoveryResult discovery;
            QString redirectUri;
            QString clientId;
            QString codeVerifier;
            QString state;
            QString registrationClientUri;
            QString registrationAccessToken;
            qint64 createdAtEpochSeconds = 0;
        };

        [[nodiscard]] QCoro::Task<void> pruneExpiredFlows();

        QNetworkAccessManager& m_networkAccessManager;
        std::unordered_map<QString, PendingOAuthFlow> m_pendingFlows;
    };
} // namespace javelin::jmap::auth
