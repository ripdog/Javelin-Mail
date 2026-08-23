#include "app/MailImportSource.h"
#include "app/MailExportWriter.h"

#include <QFile>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <utility>
#include <vector>

namespace
{
    void writeFile(const QString& path, const QByteArray& bytes)
    {
        QFile file{path};
        REQUIRE(file.open(QIODevice::WriteOnly));
        REQUIRE(file.write(bytes) == bytes.size());
    }

    QByteArray readAll(QIODevice& device)
    {
        QByteArray result;
        QByteArray chunk;
        chunk.resize(7);
        while (true)
        {
            const auto count = device.read(chunk.data(), chunk.size());
            REQUIRE(count >= 0);
            if (count == 0)
                break;
            result.append(chunk.constData(), count);
        }
        return result;
    }
} // namespace

TEST_CASE("mail import detection uses mbox framing rather than extension",
          "[app][mail-import][source]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto mbox = directory.filePath(QStringLiteral("foreign.data"));
    writeFile(mbox, QByteArrayLiteral("From user@example.test Sat Aug 22 10:20:30 2026\n"
                                      "Subject: hello\n\nbody\n\n"));

    const auto result = javelin::app::detectMailImportFile(mbox);
    REQUIRE(std::holds_alternative<javelin::app::MailImportFileKind>(result));
    CHECK(std::get<javelin::app::MailImportFileKind>(result) ==
          javelin::app::MailImportFileKind::Mbox);

    const auto eml = directory.filePath(QStringLiteral("message.mbox"));
    writeFile(eml, QByteArrayLiteral("Subject: actually eml\r\n\r\nbody"));
    const auto emlResult = javelin::app::detectMailImportFile(eml);
    REQUIRE(std::holds_alternative<javelin::app::MailImportFileKind>(emlResult));
    CHECK(std::get<javelin::app::MailImportFileKind>(emlResult) ==
          javelin::app::MailImportFileKind::Eml);
}

TEST_CASE("empty mbox files scan as zero records", "[app][mail-import][source]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("empty.mbox"));
    writeFile(path, {});

    const auto result = javelin::app::scanMailImportMbox(path);
    const auto* scan = std::get_if<javelin::app::MailImportMboxScan>(&result);
    REQUIRE(scan != nullptr);
    CHECK(scan->records.empty());
}

TEST_CASE("Javelin mbox export decodes back to RFC 5322 content",
          "[app][mail-import][source][mboxrd]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto firstSource = directory.filePath(QStringLiteral("first.eml"));
    const auto secondSource = directory.filePath(QStringLiteral("second.eml"));
    const auto mbox = directory.filePath(QStringLiteral("mailbox.mbox"));
    const QByteArray first = "Subject: first\r\n\r\n"
                             "From plain\r\n"
                             ">From quoted\r\n"
                             ">>From twice\r\n";
    const QByteArray second = "Subject: second\n\nbody\n";
    writeFile(firstSource, first);
    writeFile(secondSource, second);

    auto written = javelin::app::appendMboxRdRecord(
        firstSource, mbox, std::optional<std::string>{"first@example.test"},
        "2026-08-19T07:00:00Z");
    REQUIRE(written.error.isEmpty());
    written = javelin::app::appendMboxRdRecord(secondSource, mbox,
                                               std::optional<std::string>{"second@example.test"},
                                               "2026-08-20T08:30:00Z");
    REQUIRE(written.error.isEmpty());

    const auto result = javelin::app::scanMailImportMbox(mbox);
    const auto* scan = std::get_if<javelin::app::MailImportMboxScan>(&result);
    REQUIRE(scan != nullptr);
    REQUIRE(scan->records.size() == 2);
    CHECK(scan->records[0].receivedAt == std::optional<std::string>{"2026-08-19T07:00:00Z"});
    CHECK(scan->records[1].receivedAt == std::optional<std::string>{"2026-08-20T08:30:00Z"});

    javelin::app::MailImportMboxRecordDevice firstDevice{mbox, scan->records[0]};
    REQUIRE(firstDevice.openReadOnly());
    CHECK(readAll(firstDevice) == first);
    REQUIRE(firstDevice.seek(0));
    CHECK(readAll(firstDevice) == first);

    javelin::app::MailImportMboxRecordDevice secondDevice{mbox, scan->records[1]};
    REQUIRE(secondDevice.openReadOnly());
    CHECK(readAll(secondDevice) == second);
}

TEST_CASE("mbox import tolerates a missing final newline", "[app][mail-import][source][mbox]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto mbox = directory.filePath(QStringLiteral("mailbox.mbox"));
    writeFile(mbox, QByteArrayLiteral("From sender@example.test Sat Aug 22 10:20:30 2026\n"
                                      "Subject: final\n\nbody"));

    const auto result = javelin::app::scanMailImportMbox(mbox);
    const auto* scan = std::get_if<javelin::app::MailImportMboxScan>(&result);
    REQUIRE(scan != nullptr);
    REQUIRE(scan->records.size() == 1);

    javelin::app::MailImportMboxRecordDevice device{mbox, scan->records.front()};
    REQUIRE(device.openReadOnly());
    CHECK(readAll(device) == QByteArrayLiteral("Subject: final\n\nbody"));
}

TEST_CASE("mail import accepts extensionless RFC 5322 messages", "[app][mail-import][source]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("message"));
    writeFile(path,
              QByteArrayLiteral("X-Custom-Header: value\r\n Subject continuation\r\n\r\nbody"));
    const auto result = javelin::app::detectMailImportFile(path);
    REQUIRE(std::holds_alternative<javelin::app::MailImportFileKind>(result));
    CHECK(std::get<javelin::app::MailImportFileKind>(result) ==
          javelin::app::MailImportFileKind::Eml);
}

TEST_CASE("mail import rejects obvious non-message text and JSON", "[app][mail-import][source]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    for (const auto& [name, contents] : std::vector<std::pair<QString, QByteArray>>{
             {QStringLiteral("README.txt"),
              QByteArrayLiteral("This directory contains exported mail.\n")},
             {QStringLiteral("metadata.json"),
              QByteArrayLiteral("{\"format\":\"mail\",\"version\":1}\n")}})
    {
        const auto path = directory.filePath(name);
        writeFile(path, contents);
        const auto result = javelin::app::detectMailImportFile(path);
        CHECK(std::holds_alternative<javelin::jmap::OperationError>(result));
    }
}

TEST_CASE("mail import rejects obvious binary input", "[app][mail-import][source]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("binary.dat"));
    writeFile(path, QByteArray{"abc\0def", 7});
    const auto result = javelin::app::detectMailImportFile(path);
    CHECK(std::holds_alternative<javelin::jmap::OperationError>(result));
}
