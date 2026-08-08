#include "gui/developer/DeveloperOptionsDialog.h"
#include "app/DeveloperDiagnostics.h"
#include "app/DeveloperMaintenance.h"

#include <QAbstractItemModel>
#include <QCoreApplication>
#include <QPushButton>
#include <QTreeView>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <utility>

namespace
{
    class FakeDeveloperDiagnosticsPort final : public javelin::app::DeveloperDiagnosticsPort
    {
      public:
        explicit FakeDeveloperDiagnosticsPort(javelin::app::DeveloperDiagnosticsSnapshot snapshot)
            : m_snapshot(std::move(snapshot))
        {
        }

        [[nodiscard]] QCoro::Task<javelin::app::DeveloperDiagnosticsResult> snapshot() override
        {
            co_return javelin::app::DeveloperDiagnosticsResult{m_snapshot};
        }

      private:
        javelin::app::DeveloperDiagnosticsSnapshot m_snapshot;
    };

    class FakeDeveloperMaintenancePort final : public javelin::app::DeveloperMaintenancePort
    {
      public:
        [[nodiscard]] QCoro::Task<javelin::app::DeveloperMailboxClearResult>
        clearMailboxCache(javelin::app::DeveloperMailboxClearCommand command) override
        {
            Q_UNUSED(command)
            co_return javelin::app::DeveloperMailboxClearQueued{
                .jobId = QStringLiteral("mailbox-cache-cleanup:test"),
            };
        }
    };

    [[nodiscard]] javelin::app::DeveloperMailboxRecord
    mailbox(QString accountId, QString accountName, QString accountEmail, QString mailboxId,
            QString mailboxName, const std::uint64_t sqliteBytes)
    {
        javelin::app::DeveloperMailboxRecord value;
        value.accountId = std::move(accountId);
        value.accountName = std::move(accountName);
        value.accountEmailAddress = std::move(accountEmail);
        value.mailboxId = std::move(mailboxId);
        value.mailboxName = std::move(mailboxName);
        value.usage.sqliteEstimatedBytes = sqliteBytes;
        return value;
    }

    void processEventsUntilLoaded(const QAbstractItemModel& model)
    {
        for (int attempt = 0; attempt < 20 && model.rowCount() < 2; ++attempt)
            QCoreApplication::processEvents();
    }

    [[nodiscard]] QString mailboxName(const QAbstractItemModel& model, const int accountRow,
                                      const int mailboxRow)
    {
        const QModelIndex account = model.index(accountRow, 0);
        return model.index(mailboxRow, 0, account).data().toString();
    }
} // namespace

TEST_CASE("developer mailbox table sorts numerically within each account",
          "[gui][developer-options][sorting]")
{
    javelin::app::DeveloperDiagnosticsSnapshot snapshot;
    snapshot.mailboxes = {
        mailbox(QStringLiteral("account-z"), QStringLiteral("Zulu"),
                QStringLiteral("z@example.test"), QStringLiteral("z-small"),
                QStringLiteral("Zulu small"), 900),
        mailbox(QStringLiteral("account-z"), QStringLiteral("Zulu"),
                QStringLiteral("z@example.test"), QStringLiteral("z-large"),
                QStringLiteral("Zulu large"), 1024),
        mailbox(QStringLiteral("account-a"), QStringLiteral("Alpha"),
                QStringLiteral("a@example.test"), QStringLiteral("a-small"),
                QStringLiteral("Alpha small"), 700),
        mailbox(QStringLiteral("account-a"), QStringLiteral("Alpha"),
                QStringLiteral("a@example.test"), QStringLiteral("a-large"),
                QStringLiteral("Alpha large"), 2048),
    };
    FakeDeveloperDiagnosticsPort diagnostics{std::move(snapshot)};
    FakeDeveloperMaintenancePort maintenance;
    javelin::gui::developer::DeveloperOptionsDialog dialog{diagnostics, maintenance};

    auto* view = dialog.findChild<QTreeView*>();
    REQUIRE(view != nullptr);
    auto* model = view->model();
    REQUIRE(model != nullptr);
    processEventsUntilLoaded(*model);
    REQUIRE(model->rowCount() == 2);
    auto* clearSqlite =
        dialog.findChild<QPushButton*>(QStringLiteral("developerClearSqliteButton"));
    auto* clearBodies =
        dialog.findChild<QPushButton*>(QStringLiteral("developerClearBodiesButton"));
    REQUIRE(clearSqlite != nullptr);
    REQUIRE(clearBodies != nullptr);
    CHECK(clearSqlite->isEnabled());
    CHECK(clearBodies->isEnabled());

    view->sortByColumn(1, Qt::DescendingOrder);
    QCoreApplication::processEvents();

    CHECK(model->index(0, 0).data().toString() == QStringLiteral("Zulu — z@example.test"));
    CHECK(model->index(1, 0).data().toString() == QStringLiteral("Alpha — a@example.test"));
    CHECK(mailboxName(*model, 0, 0) == QStringLiteral("Zulu large"));
    CHECK(mailboxName(*model, 0, 1) == QStringLiteral("Zulu small"));
    CHECK(mailboxName(*model, 1, 0) == QStringLiteral("Alpha large"));
    CHECK(mailboxName(*model, 1, 1) == QStringLiteral("Alpha small"));

    view->sortByColumn(1, Qt::AscendingOrder);
    QCoreApplication::processEvents();

    CHECK(model->index(0, 0).data().toString() == QStringLiteral("Zulu — z@example.test"));
    CHECK(model->index(1, 0).data().toString() == QStringLiteral("Alpha — a@example.test"));
    CHECK(mailboxName(*model, 0, 0) == QStringLiteral("Zulu small"));
    CHECK(mailboxName(*model, 1, 0) == QStringLiteral("Alpha small"));
}
