#include "app/PerformanceMetrics.h"
#include "protocol/actions/ActionCatalog.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>

TEST_CASE("performance metrics use stable machine-readable fields")
{
    const auto metric = javelin::app::PerformanceMetrics::formatMetric(
        QStringLiteral("gui"), QStringLiteral("remote_action_e2e"), std::chrono::microseconds{1234},
        QStringLiteral("completed"), QStringLiteral("kind=mail_queue_mark_read payload bytes"));

    CHECK(metric ==
          QStringLiteral("metric process=gui operation=remote_action_e2e outcome=completed "
                         "duration_us=1234 details=\"kind=mail_queue_mark_read payload bytes\""));
}

TEST_CASE("performance metrics identify every remote action through its protocol name")
{
    CHECK(javelin::app::PerformanceMetrics::remoteActionName(javelin::protocol::actions::actionName(
              javelin::protocol::actions::MailboxWindow::id)) == QStringLiteral("mailbox_window"));
    CHECK(javelin::app::PerformanceMetrics::remoteActionName(javelin::protocol::actions::actionName(
              javelin::protocol::actions::WorkSummary::id)) == QStringLiteral("work_summary"));
}
