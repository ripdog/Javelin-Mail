#include "jmap/auth/AccountOnboardingService.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QNetworkAccessManager>

#include <catch2/catch_test_macros.hpp>

namespace
{
    void ensureApplication()
    {
        if (QCoreApplication::instance() != nullptr)
            return;

        static int argc = 1;
        static char appName[] = "javelin-onboarding-tests";
        static char* argv[] = {appName, nullptr};
        static QCoreApplication application(argc, argv);
        Q_UNUSED(application);
    }
} // namespace

TEST_CASE("manual onboarding refuses to send a token over an insecure connection",
          "[jmap][auth][onboarding]")
{
    ensureApplication();
    QNetworkAccessManager networkAccessManager;
    javelin::jmap::auth::AccountOnboardingService service{networkAccessManager};

    const auto result = QCoro::waitFor(service.authenticateManually({
        .emailAddress = QStringLiteral("alice@example.com"),
        .sessionUrl = QStringLiteral("http://mail.example.com/.well-known/jmap"),
        .accessToken = QStringLiteral("secret-token"),
    }));

    CHECK_FALSE(result.succeeded);
    CHECK(result.error == QStringLiteral("Use an HTTPS address for the JMAP server."));
}

TEST_CASE("OAuth refresh refuses an insecure token endpoint", "[jmap][auth][onboarding]")
{
    ensureApplication();
    QNetworkAccessManager networkAccessManager;
    javelin::jmap::auth::AccountOnboardingService service{networkAccessManager};

    const auto result = QCoro::waitFor(service.refreshOAuth({
        .sessionUrl = QStringLiteral("https://mail.example.com/.well-known/jmap"),
        .tokenEndpoint = QStringLiteral("http://auth.example.com/token"),
        .clientId = QStringLiteral("javelin"),
        .refreshToken = QStringLiteral("refresh-token"),
    }));

    CHECK_FALSE(result.succeeded);
    CHECK(result.error == QStringLiteral("OAuth refresh information is incomplete."));
}
