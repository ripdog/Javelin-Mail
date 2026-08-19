#include "app/MailExportWriter.h"

#include <QFile>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

namespace
{
    void writeFile(const QString& path, const QByteArray& bytes)
    {
        QFile file{path};
        REQUIRE(file.open(QIODevice::WriteOnly));
        REQUIRE(file.write(bytes) == bytes.size());
    }

    QByteArray readFile(const QString& path)
    {
        QFile file{path};
        REQUIRE(file.open(QIODevice::ReadOnly));
        return file.readAll();
    }
} // namespace

TEST_CASE("EML export preserves raw message bytes exactly", "[app][mail-export][eml]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto source = directory.filePath(QStringLiteral("source.eml"));
    const auto target = directory.filePath(QStringLiteral("target.eml"));
    const QByteArray payload{"Header: value\r\n\r\nbody\0with\nbytes", 32};
    writeFile(source, payload);

    const auto result = javelin::app::copyEmlFile(source, target);
    INFO(result.error.toStdString());
    REQUIRE(result.error.isEmpty());
    CHECK(readFile(target) == payload);
}

TEST_CASE("mboxrd export quotes every separator-like source line", "[app][mail-export][mboxrd]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto source = directory.filePath(QStringLiteral("source.eml"));
    const auto target = directory.filePath(QStringLiteral("mailbox.mbox.javelin-part"));
    const QByteArray payload = "Subject: escaping\r\n\r\n"
                               "From plain\r\n"
                               ">From quoted\r\n"
                               ">>From twice\r\n"
                               "not From here\r\n";
    writeFile(source, payload);

    const auto result = javelin::app::appendMboxRdRecord(
        source, target, std::optional<std::string>{"sender@example.test"},
        "2026-08-19T07:00:00+12:00");
    INFO(result.error.toStdString());
    REQUIRE(result.error.isEmpty());

    const auto output = readFile(target);
    CHECK(output.startsWith("From sender@example.test "));
    CHECK(output.contains("Subject: escaping\r\n\r\n"));
    CHECK(output.contains("\r\n>From plain\r\n"));
    CHECK(output.contains("\r\n>>From quoted\r\n"));
    CHECK(output.contains("\r\n>>>From twice\r\n"));
    CHECK(output.contains("\r\nnot From here\r\n"));
    CHECK(output.endsWith("\r\n\n"));
}

TEST_CASE("mboxrd export frames a message without a final newline", "[app][mail-export][mboxrd]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const auto source = directory.filePath(QStringLiteral("source.eml"));
    const auto target = directory.filePath(QStringLiteral("mailbox.mbox.javelin-part"));
    writeFile(source, QByteArrayLiteral("Subject: no-newline\n\nbody"));

    const auto result =
        javelin::app::appendMboxRdRecord(source, target, std::nullopt, "2026-08-19T07:00:00+12:00");
    INFO(result.error.toStdString());
    REQUIRE(result.error.isEmpty());

    const auto output = readFile(target);
    CHECK(output.contains("Subject: no-newline\n\nbody\n\n"));
}
