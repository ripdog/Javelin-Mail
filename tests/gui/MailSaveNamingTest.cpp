#include "app/MailSaveNaming.h"
#include "app/FileNameUtils.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("generated mail save filenames are bounded in UTF-8 bytes")
{
    javelin::jmap::domain::Email email{
        .id = "email-id-with-stable-identity",
        .blobId = "blob",
        .threadId = "thread",
        .mailboxIds = {"inbox"},
        .keywords = {},
        .size = 10,
        .receivedAt = "2026-08-19T07:00:00+12:00",
        .sentAt = std::nullopt,
        .messageId = {},
        .inReplyTo = {},
        .references = {},
        .hasAttachment = false,
        .subject = std::string(200, 'x') + " 日本語日本語日本語日本語日本語",
        .from = {{.name = std::string(120, 'y') + " 山田太郎", .email = "sender@example.com"}},
        .to = {},
        .cc = {},
        .bcc = {},
        .replyTo = {},
        .preview = std::nullopt,
    };

    const auto name = javelin::app::suggestedMailSaveFileName(email);
    CHECK(name.endsWith(QStringLiteral(".eml")));
    CHECK(name.toUtf8().size() <= 240);
    CHECK_FALSE(name.contains(QLatin1Char('/')));
    CHECK_FALSE(name.contains(QLatin1Char('\\')));
}

TEST_CASE("generic generated filename truncation counts UTF-8 bytes")
{
    const QString input = QString{100, QChar{0x754c}} + QStringLiteral(".vcf");
    const auto truncated = javelin::app::truncateGeneratedFileName(input, 64);

    CHECK(truncated.endsWith(QStringLiteral(".vcf")));
    CHECK(truncated.toUtf8().size() <= 64);
    CHECK(truncated.size() > 4);
}

TEST_CASE("generated filename truncation honors pathological byte budgets")
{
    const auto longExtension = javelin::app::truncateGeneratedFileName(
        QStringLiteral("name.abcdefghijklmnopqrstuvwxyz"), 8);
    const auto fallback =
        javelin::app::truncateGeneratedFileName(QString::fromUtf8("😀.x"), 1);
    const auto zeroBudget =
        javelin::app::truncateGeneratedFileName(QStringLiteral("name.ext"), 0);

    CHECK(longExtension.toUtf8().size() <= 8);
    CHECK(fallback.toUtf8().size() <= 1);
    CHECK(fallback == QStringLiteral("f"));
    CHECK(zeroBudget.isEmpty());
}

TEST_CASE("mail save collision names reserve space for their discriminator")
{
    const QString input = QString{200, QChar{0x754c}} + QStringLiteral(".eml");

    const auto second = javelin::app::collisionMailSaveFileName(input, 2, 64);
    const auto later = javelin::app::collisionMailSaveFileName(input, 123456, 64);

    CHECK(second.endsWith(QStringLiteral("-2.eml")));
    CHECK(later.endsWith(QStringLiteral("-123456.eml")));
    CHECK(second.toUtf8().size() <= 64);
    CHECK(later.toUtf8().size() <= 64);
    CHECK(second != later);
}
