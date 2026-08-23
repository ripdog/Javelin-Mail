#include "app/MailImportPlanner.h"
#include "app/MailExportWriter.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <ranges>
#include <string>
#include <variant>
#include <vector>

namespace
{
    using javelin::app::MailImportOperationRecord;
    using javelin::app::MailImportScanPlan;
    using javelin::app::MailImportStatus;
    using javelin::jmap::OperationError;

    void writeFile(const QString& path, const QByteArray& bytes)
    {
        QFile file{path};
        REQUIRE(file.open(QIODevice::WriteOnly));
        REQUIRE(file.write(bytes) == bytes.size());
    }

    void writeMarker(const QString& root, const QString& format,
                     const QString& status = QStringLiteral("complete"))
    {
        writeFile(QDir{root}.filePath(QStringLiteral(".javelin-mail-export.json")),
                  QJsonDocument{QJsonObject{
                                    {QStringLiteral("version"), 1},
                                    {QStringLiteral("operationId"), QStringLiteral("export-1")},
                                    {QStringLiteral("status"), status},
                                    {QStringLiteral("format"), format},
                                }}
                      .toJson(QJsonDocument::Compact));
    }

    [[nodiscard]] MailImportOperationRecord operation(const QString& root)
    {
        return {
            .operationId = "import-1",
            .accountId = "account-1",
            .mailboxId = std::nullopt,
            .sourcePaths = {root},
            .recreateHierarchy = true,
            .status = MailImportStatus::Preparing,
            .scanSealed = false,
            .title = QStringLiteral("Import backup"),
            .createdAt = QStringLiteral("2026-08-22T10:00:00Z"),
            .lastError = std::nullopt,
        };
    }

    [[nodiscard]] std::vector<QString> mailboxPaths(const MailImportScanPlan& plan)
    {
        std::vector<QString> result;
        result.reserve(plan.mailboxes.size());
        for (const auto& mailbox : plan.mailboxes)
            result.push_back(mailbox.relativePath);
        return result;
    }
} // namespace

TEST_CASE("Javelin EML backup planning ignores metadata and preserves empty mailboxes",
          "[app][mail-import][planner][javelin-export]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    writeMarker(directory.path(), QStringLiteral("eml"));
    REQUIRE(QDir{directory.path()}.mkpath(QStringLiteral("Inbox/Empty Child")));
    REQUIRE(QDir{directory.path()}.mkpath(QStringLiteral("Archive")));
    writeFile(QDir{directory.path()}.filePath(QStringLiteral("Inbox/message.eml")),
              QByteArrayLiteral("From: alice@example.test\r\nSubject: Hello\r\n\r\nBody\r\n"));

    const auto result = javelin::app::planMailImportSources(operation(directory.path()));
    const auto* plan = std::get_if<MailImportScanPlan>(&result);
    REQUIRE(plan != nullptr);
    CHECK(mailboxPaths(*plan) == std::vector<QString>{QStringLiteral("Archive"),
                                                      QStringLiteral("Inbox"),
                                                      QStringLiteral("Inbox/Empty Child")});
    REQUIRE(plan->items.size() == 1);
    CHECK(plan->items.front().sourceRelativePath ==
          std::optional<QString>{QStringLiteral("Inbox/message.eml")});
    CHECK(plan->items.front().destinationRelativePath ==
          std::optional<QString>{QStringLiteral("Inbox")});
}

TEST_CASE("Javelin mbox backup planning preserves an empty mailbox file",
          "[app][mail-import][planner][javelin-export]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    writeMarker(directory.path(), QStringLiteral("mboxrd"));
    writeFile(QDir{directory.path()}.filePath(QStringLiteral("Empty.mbox")), {});

    const auto source = QDir{directory.path()}.filePath(QStringLiteral("message.eml"));
    const auto inbox = QDir{directory.path()}.filePath(QStringLiteral("Inbox.mbox"));
    writeFile(source,
              QByteArrayLiteral("From: bob@example.test\r\nSubject: Imported\r\n\r\nBody\r\n"));
    const auto written = javelin::app::appendMboxRdRecord(
        source, inbox, std::optional<std::string>{"bob@example.test"}, "2026-08-22T10:00:00Z");
    REQUIRE(written.error.isEmpty());
    REQUIRE(QFile::remove(source));

    const auto result = javelin::app::planMailImportSources(operation(directory.path()));
    const auto* plan = std::get_if<MailImportScanPlan>(&result);
    REQUIRE(plan != nullptr);
    CHECK(mailboxPaths(*plan) ==
          std::vector<QString>{QStringLiteral("Empty"), QStringLiteral("Inbox")});
    REQUIRE(plan->items.size() == 1);
    CHECK(plan->items.front().sourceRelativePath ==
          std::optional<QString>{QStringLiteral("Inbox.mbox")});
    CHECK(plan->items.front().destinationRelativePath ==
          std::optional<QString>{QStringLiteral("Inbox")});
}

TEST_CASE("unfinished Javelin backup is rejected before import planning",
          "[app][mail-import][planner][javelin-export]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    writeMarker(directory.path(), QStringLiteral("eml"), QStringLiteral("running"));
    REQUIRE(QDir{directory.path()}.mkpath(QStringLiteral("Inbox")));
    writeFile(QDir{directory.path()}.filePath(QStringLiteral("Inbox/message.eml")),
              QByteArrayLiteral("Subject: Incomplete\r\n\r\nBody\r\n"));

    const auto result = javelin::app::planMailImportSources(operation(directory.path()));
    CHECK(std::holds_alternative<OperationError>(result));
}

TEST_CASE("unfinished Javelin part file is rejected even without a marker",
          "[app][mail-import][planner][javelin-export]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    writeFile(QDir{directory.path()}.filePath(QStringLiteral("Inbox.mbox.javelin-part")),
              QByteArrayLiteral("partial"));

    const auto result = javelin::app::planMailImportSources(operation(directory.path()));
    CHECK(std::holds_alternative<OperationError>(result));
}
