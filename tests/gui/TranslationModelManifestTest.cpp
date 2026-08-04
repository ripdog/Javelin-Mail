#include "gui/translation/TranslationModelManifest.h"

#include <catch2/catch_test_macros.hpp>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <memory>

namespace
{
    using namespace javelin::gui::translation;

    [[nodiscard]] QJsonObject modelFile(const QString& type, const QString& installedName,
                                        const QChar hashCharacter = QLatin1Char('a'))
    {
        return {
            {QStringLiteral("type"), type},
            {QStringLiteral("url"),
             QStringLiteral("https://firefox-settings-attachments.cdn.mozilla.net/models/%1.zst")
                 .arg(installedName)},
            {QStringLiteral("compression"), QStringLiteral("zstd")},
            {QStringLiteral("compressedSize"), 10},
            {QStringLiteral("compressedSha256"), QString(64, hashCharacter)},
            {QStringLiteral("decompressedSize"), 20},
            {QStringLiteral("decompressedSha256"), QString(64, hashCharacter)},
            {QStringLiteral("installedName"), installedName},
        };
    }

    [[nodiscard]] QJsonObject direction(const QString& source, const QString& target)
    {
        return {
            {QStringLiteral("source"), source},
            {QStringLiteral("target"), target},
            {QStringLiteral("mozillaSource"), source},
            {QStringLiteral("mozillaTarget"), target},
            {QStringLiteral("modelVersion"), QStringLiteral("1.0")},
            {QStringLiteral("architecture"), QStringLiteral("base-memory")},
            {QStringLiteral("files"),
             QJsonArray{
                 modelFile(QStringLiteral("model"), QStringLiteral("model.bin")),
                 modelFile(QStringLiteral("lex"), QStringLiteral("lex.bin"), QLatin1Char('b')),
                 modelFile(QStringLiteral("vocab"), QStringLiteral("vocab.spm"), QLatin1Char('c')),
             }},
            {QStringLiteral("licenseFiles"), QJsonArray{QStringLiteral("MPL-2.0.txt")}},
        };
    }

    [[nodiscard]] QByteArray manifestJson(QJsonArray directions)
    {
        return QJsonDocument{QJsonObject{
                                 {QStringLiteral("schemaVersion"), 1},
                                 {QStringLiteral("manifestRevision"), QStringLiteral("test-1")},
                                 {QStringLiteral("engine"),
                                  QJsonObject{
                                      {QStringLiteral("name"), QStringLiteral("bergamot")},
                                      {QStringLiteral("version"), QStringLiteral("v0.6.0")},
                                      {QStringLiteral("sourceCommit"),
                                       QStringLiteral("4732dc947bc952abb019aabfe5582006d4fc3337")},
                                  }},
                                 {QStringLiteral("directions"), std::move(directions)},
                             }}
            .toJson(QJsonDocument::Compact);
    }
} // namespace

TEST_CASE("translation model manifest resolves direct and English-pivot routes",
          "[translation][manifest]")
{
    TranslationError error;
    auto manifest = TranslationModelManifest::fromJson(
        manifestJson(QJsonArray{
            direction(QStringLiteral("fr"), QStringLiteral("en")),
            direction(QStringLiteral("en"), QStringLiteral("de")),
            direction(QStringLiteral("zh_cn"), QStringLiteral("en")),
        }),
        error);
    INFO(error.message.toStdString());
    REQUIRE(manifest != nullptr);
    CHECK(manifest->directions().size() == 3);
    REQUIRE(manifest->direction(QStringLiteral("zh-Hans"), QStringLiteral("en")) != nullptr);

    const auto direct = manifest->route(QStringLiteral("fr"), QStringLiteral("en"));
    CHECK(direct.supported());
    CHECK_FALSE(direct.isIdentity());
    REQUIRE(direct.legs.size() == 1);
    CHECK(direct.legs.front()->source == QStringLiteral("fr"));

    const auto pivot = manifest->route(QStringLiteral("fr"), QStringLiteral("de"));
    CHECK(pivot.supported());
    CHECK_FALSE(pivot.isIdentity());
    REQUIRE(pivot.legs.size() == 2);
    CHECK(pivot.legs[0]->target == QStringLiteral("en"));
    CHECK(pivot.legs[1]->source == QStringLiteral("en"));

    const auto identity = manifest->route(QStringLiteral("de"), QStringLiteral("de-DE"));
    CHECK_FALSE(identity.supported());
    const auto exactIdentity = manifest->route(QStringLiteral("de"), QStringLiteral("de"));
    CHECK(exactIdentity.supported());
    CHECK(exactIdentity.isIdentity());
    CHECK_FALSE(manifest->route(QStringLiteral("ja"), QStringLiteral("de")).supported());
}

TEST_CASE("translation model manifest rejects untrusted attachment hosts",
          "[translation][manifest][security]")
{
    auto invalidDirection = direction(QStringLiteral("fr"), QStringLiteral("en"));
    auto files = invalidDirection.value(QStringLiteral("files")).toArray();
    auto file = files[0].toObject();
    file.insert(QStringLiteral("url"), QStringLiteral("https://example.test/model.zst"));
    files[0] = file;
    invalidDirection.insert(QStringLiteral("files"), files);

    TranslationError error;
    const auto manifest =
        TranslationModelManifest::fromJson(manifestJson(QJsonArray{invalidDirection}), error);
    CHECK(manifest == nullptr);
    CHECK(error.code == TranslationErrorCode::ManifestInvalid);
    CHECK_FALSE(error.message.isEmpty());
}
