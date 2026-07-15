#include "app/ApplicationErrorCoordinator.h"

#include <catch2/catch_test_macros.hpp>

#include <QSettings>
#include <QTemporaryDir>
#include <QUuid>

TEST_CASE("application errors are deduplicated and rearmed after recovery")
{
    QTemporaryDir settingsDirectory;
    REQUIRE(settingsDirectory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());

    javelin::app::ApplicationErrorCoordinator coordinator;
    const javelin::app::AccountConnectionSettings settings{
        .connectionId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),
        .revision = 4,
        .sessionUrl = "https://example.test/jmap",
        .loginEmail = "user@example.test",
        .apiKey = "secret",
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
    CHECK_FALSE(userMessage.contains(QStringLiteral("diagnostic timeout")));

    coordinator.reportSuccess(settings.connectionId);
    coordinator.reportFailure(settings, "account-a", QStringLiteral("Synchronize mail"), timeout);
    CHECK(incidents == 2);
    coordinator.forgetConnection(settings.connectionId);
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
    };
    {
        javelin::app::ApplicationErrorCoordinator coordinator;
        coordinator.reportFailure(
            settings, "account-a", QStringLiteral("Synchronize contacts"),
            {.code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
             .message = QStringLiteral("unauthorized")});
        CHECK(coordinator.authenticationPaused(settings.connectionId, settings.revision));
    }

    javelin::app::ApplicationErrorCoordinator restored;
    CHECK(restored.authenticationPaused(settings.connectionId, settings.revision));
    restored.settingsApplied(settings.connectionId, settings.revision);
    CHECK(restored.authenticationPaused(settings.connectionId, settings.revision));
    restored.settingsApplied(settings.connectionId, settings.revision + 1);
    CHECK_FALSE(restored.authenticationPaused(settings.connectionId, settings.revision + 1));
    restored.forgetConnection(settings.connectionId);
}
