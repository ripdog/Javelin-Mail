#include "app/GuiMailApplicationEvents.h"
#include "app/GuiDaemonSession.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>

TEST_CASE("GUI mail events retain daemon status published before construction", "[app][gui]")
{
    javelin::app::GuiDaemonSession session{
        {.runtimeDirectory = QStringLiteral("/tmp"),
         .socketPath = QStringLiteral("/tmp/unused-javelin-test.sock"),
         .daemonExecutable = {},
         .protocol = {.major = 2, .minor = 0},
         .build = {.application = QStringLiteral("Javelin-Mail"),
                   .revision = QStringLiteral("test")},
         .startTimeoutMilliseconds = 10,
         .startDaemonIfMissing = false}};

    session.onBoundaryEvent(javelin::protocol::DaemonStatusChanged{
        .status = {.lifecycle = javelin::protocol::DaemonLifecycle::Ready,
                   .accounts = {{.accountId = QStringLiteral("account-1"),
                                 .state = javelin::protocol::AccountState::Ready,
                                 .detail = {}}}}});

    javelin::app::GuiMailApplicationEvents events{session};
    const auto statuses = events.accountStatuses();
    REQUIRE(statuses.contains("account-1"));
    CHECK(statuses.at("account-1") == javelin::app::MailAccountStatus::Connected);

    std::optional<javelin::app::MailAccountStatus> recoveryStatus;
    QObject::connect(
        &events, &javelin::app::MailApplicationEventsPort::accountStatusChanged, &events,
        [&recoveryStatus](const QString& accountId, const javelin::app::MailAccountStatus status)
        {
            if (accountId == QStringLiteral("account-1"))
                recoveryStatus = status;
        });
    Q_EMIT session.recoveryStarted(QStringLiteral("daemon disconnected"));
    REQUIRE(recoveryStatus.has_value());
    CHECK(*recoveryStatus == javelin::app::MailAccountStatus::Disconnected);
    CHECK(events.accountStatuses().empty());
}
