#include "app/ApplicationErrorCoordinator.h"

#include "jmap/cache/AccountReadRepository.h"

#include <catch2/catch_test_macros.hpp>

#include <QMessageLogContext>
#include <QSettings>
#include <QTemporaryDir>
#include <QUuid>
#include <QtLogging>

namespace
{
    QString capturedWarning;

    void captureWarning(const QtMsgType type, const QMessageLogContext&, const QString& message)
    {
        if (type == QtWarningMsg)
            capturedWarning = message;
    }

    class StubAccountReader final : public javelin::jmap::cache::AccountReader
    {
      public:
        std::vector<javelin::jmap::cache::CachedAccount> accounts;

        [[nodiscard]] std::variant<std::vector<javelin::jmap::cache::CachedAccount>,
                                   javelin::jmap::cache::DatabaseError>
        listAll() const override
        {
            return accounts;
        }

        [[nodiscard]] std::variant<std::vector<javelin::jmap::cache::CachedAccount>,
                                   javelin::jmap::cache::DatabaseError>
        listOwnedBy(std::string_view) const override
        {
            return accounts;
        }

        [[nodiscard]] std::variant<std::optional<javelin::jmap::cache::CachedAccount>,
                                   javelin::jmap::cache::DatabaseError>
        findById(const std::string_view accountId) const override
        {
            const auto found = std::ranges::find(accounts, accountId,
                                                 &javelin::jmap::cache::CachedAccount::accountId);
            return found == accounts.end() ? std::optional<javelin::jmap::cache::CachedAccount>{}
                                           : std::optional{*found};
        }
    };

    class WarningCapture final
    {
      public:
        WarningCapture() : m_previous(qInstallMessageHandler(captureWarning))
        {
            capturedWarning.clear();
        }

        ~WarningCapture()
        {
            qInstallMessageHandler(m_previous);
        }

        WarningCapture(const WarningCapture&) = delete;
        WarningCapture& operator=(const WarningCapture&) = delete;

      private:
        QtMessageHandler m_previous;
    };
} // namespace

TEST_CASE("application errors log all available diagnostic context")
{
    StubAccountReader accountReader;
    javelin::app::ApplicationErrorCoordinator coordinator{accountReader};
    const javelin::app::AccountConnectionSettings settings{
        .connectionId = "connection-a",
        .revision = 1,
        .sessionUrl = "https://example.test/jmap",
        .loginEmail = "user@example.test",
        .apiKey = "secret",
        .refreshToken = {},
        .tokenEndpoint = {},
        .oauthClientId = {},
    };
    const WarningCapture warningCapture;

    coordinator.reportFailure(settings, "account-a", QStringLiteral("Save contact"),
                              {
                                  .code = javelin::jmap::OperationErrorCode::ServerUnavailable,
                                  .message = QStringLiteral("diagnostic detail"),
                                  .httpStatus = 503,
                                  .retryAfter = std::chrono::seconds{17},
                                  .protocolType = "serverUnavailable",
                              });

    CHECK(capturedWarning.contains(QStringLiteral("Save contact failed")));
    CHECK(capturedWarning.contains(QStringLiteral("connection=connection-a")));
    CHECK(capturedWarning.contains(QStringLiteral("account=account-a")));
    CHECK(capturedWarning.contains(QStringLiteral("code=server_unavailable")));
    CHECK(capturedWarning.contains(QStringLiteral("message=diagnostic detail")));
    CHECK(capturedWarning.contains(QStringLiteral("httpStatus=503")));
    CHECK(capturedWarning.contains(QStringLiteral("protocolType=serverUnavailable")));
    CHECK(capturedWarning.contains(QStringLiteral("retryAfterSeconds=17")));
}

TEST_CASE("application errors are deduplicated and rearmed after recovery")
{
    QTemporaryDir settingsDirectory;
    REQUIRE(settingsDirectory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());

    StubAccountReader accountReader;
    accountReader.accounts.push_back({.accountId = "account-a",
                                      .connectionId = "connection-a",
                                      .remoteAccountId = "account-a",
                                      .name = "Personal mail",
                                      .isPrimary = true,
                                      .ownerAccountId = "account-a",
                                      .hasSubmissionCapability = true});
    javelin::app::ApplicationErrorCoordinator coordinator{accountReader};
    const javelin::app::AccountConnectionSettings settings{
        .connectionId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),
        .revision = 4,
        .displayName = "Configured personal mail",
        .sessionUrl = "https://example.test/jmap",
        .loginEmail = "user@example.test",
        .apiKey = "secret",
        .refreshToken = {},
        .tokenEndpoint = {},
        .oauthClientId = {},
    };
    int incidents = 0;
    QString userMessage;
    QObject::connect(&coordinator, &javelin::app::ApplicationErrorCoordinator::incidentRaised,
                     [&incidents, &userMessage](const QString&, const QString&, const QString&,
                                                const QString& message, const bool, const bool)
                     {
                         ++incidents;
                         userMessage = message;
                     });

    const javelin::jmap::OperationError timeout{
        .code = javelin::jmap::OperationErrorCode::Timeout,
        .message = QStringLiteral("diagnostic timeout"),
    };
    coordinator.reportFailure(settings, "account-a", QStringLiteral("Synchronize mail"), timeout);
    coordinator.reportFailure(settings, "account-a", QStringLiteral("Synchronize mail"), timeout);
    CHECK(incidents == 1);
    CHECK(userMessage.contains(QStringLiteral("example.test")));
    CHECK_FALSE(userMessage.contains(QStringLiteral("Configured personal mail")));
    CHECK_FALSE(userMessage.contains(QStringLiteral("Personal mail")));
    CHECK_FALSE(userMessage.contains(QStringLiteral("account-a")));
    CHECK_FALSE(userMessage.contains(QStringLiteral("diagnostic timeout")));

    coordinator.reportSuccess(settings.connectionId);
    coordinator.reportFailure(settings, "account-a", QStringLiteral("Synchronize mail"), timeout);
    CHECK(incidents == 2);
    coordinator.forgetConnection(settings.connectionId);
}

TEST_CASE("transport outages are coalesced across accounts sharing a mail service")
{
    QTemporaryDir settingsDirectory;
    REQUIRE(settingsDirectory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());

    StubAccountReader accountReader;
    javelin::app::ApplicationErrorCoordinator coordinator{accountReader};
    const javelin::app::AccountConnectionSettings first{
        .connectionId = "connection-a",
        .revision = 1,
        .displayName = "First account",
        .sessionUrl = "https://mail.example.test/.well-known/jmap",
        .loginEmail = "first@example.test",
        .apiKey = "secret",
        .refreshToken = {},
        .tokenEndpoint = {},
        .oauthClientId = {},
    };
    const javelin::app::AccountConnectionSettings second{
        .connectionId = "connection-b",
        .revision = 1,
        .displayName = "Second account",
        .sessionUrl = "https://mail.example.test/jmap/session",
        .loginEmail = "second@example.test",
        .apiKey = "secret",
        .refreshToken = {},
        .tokenEndpoint = {},
        .oauthClientId = {},
    };

    int incidents = 0;
    QString message;
    QObject::connect(&coordinator, &javelin::app::ApplicationErrorCoordinator::incidentRaised,
                     [&incidents, &message](const QString&, const QString&, const QString&,
                                            const QString& value, const bool, const bool)
                     {
                         ++incidents;
                         message = value;
                     });

    coordinator.reportFailure(first, "account-a", QStringLiteral("Synchronize mailbox"),
                              {.code = javelin::jmap::OperationErrorCode::Timeout,
                               .message = QStringLiteral("request timed out")});
    coordinator.reportFailure(second, "account-b", QStringLiteral("Maintain account connection"),
                              {.code = javelin::jmap::OperationErrorCode::ServerUnavailable,
                               .message = QStringLiteral("gateway unavailable"),
                               .httpStatus = 503});

    CHECK(incidents == 1);
    CHECK(message.contains(QStringLiteral("mail.example.test")));
    CHECK_FALSE(message.contains(QStringLiteral("First account")));
    CHECK_FALSE(message.contains(QStringLiteral("Second account")));

    coordinator.reportSuccess(second.connectionId);
    coordinator.reportFailure(first, "account-a", QStringLiteral("Synchronize mailbox"),
                              {.code = javelin::jmap::OperationErrorCode::NetworkUnavailable,
                               .message = QStringLiteral("connection refused")});
    CHECK(incidents == 1);

    coordinator.reportSuccess(first.connectionId);
    coordinator.reportFailure(first, "account-a", QStringLiteral("Synchronize mailbox"),
                              {.code = javelin::jmap::OperationErrorCode::NetworkUnavailable,
                               .message = QStringLiteral("connection refused again")});
    CHECK(incidents == 2);
}

TEST_CASE("server method rejections are not hidden by outage coalescing")
{
    StubAccountReader accountReader;
    javelin::app::ApplicationErrorCoordinator coordinator{accountReader};
    const javelin::app::AccountConnectionSettings settings{
        .connectionId = "connection-a",
        .revision = 1,
        .displayName = "Personal mail",
        .sessionUrl = "https://mail.example.test/jmap",
        .loginEmail = "user@example.test",
        .apiKey = "secret",
        .refreshToken = {},
        .tokenEndpoint = {},
        .oauthClientId = {},
    };

    int incidents = 0;
    QObject::connect(&coordinator, &javelin::app::ApplicationErrorCoordinator::incidentRaised,
                     [&incidents](const QString&, const QString&, const QString&, const QString&,
                                  const bool, const bool) { ++incidents; });

    const javelin::jmap::OperationError methodUnavailable{
        .code = javelin::jmap::OperationErrorCode::ServerUnavailable,
        .message = QStringLiteral("this method is temporarily unavailable"),
        .protocolType = "serverUnavailable",
    };
    coordinator.reportFailure(settings, "account-a", QStringLiteral("Save calendar event"),
                              methodUnavailable);
    coordinator.reportFailure(settings, "account-a", QStringLiteral("Save calendar event"),
                              methodUnavailable);

    const javelin::jmap::OperationError permissionDenied{
        .code = javelin::jmap::OperationErrorCode::PermissionDenied,
        .message = QStringLiteral("forbidden"),
        .protocolType = "forbidden",
    };
    coordinator.reportFailure(settings, "account-a", QStringLiteral("Move message"),
                              permissionDenied);
    coordinator.reportFailure(settings, "account-a", QStringLiteral("Delete message"),
                              permissionDenied);

    CHECK(incidents == 4);
}

TEST_CASE("authentication pause persists until a newer connection revision is applied")
{
    QTemporaryDir settingsDirectory;
    REQUIRE(settingsDirectory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());

    const javelin::app::AccountConnectionSettings settings{
        .connectionId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),
        .revision = 8,
        .sessionUrl = "https://example.test/jmap",
        .loginEmail = "user@example.test",
        .apiKey = "bad-secret",
        .refreshToken = {},
        .tokenEndpoint = {},
        .oauthClientId = {},
    };
    StubAccountReader accountReader;
    {
        javelin::app::ApplicationErrorCoordinator coordinator{accountReader};
        coordinator.reportFailure(
            settings, "account-a", QStringLiteral("Synchronize contacts"),
            {.code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
             .message = QStringLiteral("unauthorized")});
        CHECK(coordinator.authenticationPaused(settings.connectionId, settings.revision));
    }

    javelin::app::ApplicationErrorCoordinator restored{accountReader};
    CHECK(restored.authenticationPaused(settings.connectionId, settings.revision));
    restored.settingsApplied(settings.connectionId, settings.revision);
    CHECK(restored.authenticationPaused(settings.connectionId, settings.revision));
    restored.settingsApplied(settings.connectionId, settings.revision + 1);
    CHECK_FALSE(restored.authenticationPaused(settings.connectionId, settings.revision + 1));
    restored.forgetConnection(settings.connectionId);
}
