#include "jmap/language/LanguageDetection.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

TEST_CASE("language detection policy compares confident text with the translation target",
          "[jmap][language]")
{
    using javelin::jmap::language::LanguageDetectionResult;
    using javelin::jmap::language::shouldOfferTranslation;

    const LanguageDetectionResult french{
        .languageCode = "fr",
        .confidence = 0.91,
        .englishConfidence = 0.04,
    };
    CHECK(shouldOfferTranslation(french, "en"));
    CHECK_FALSE(shouldOfferTranslation(french, "fr"));
    CHECK_FALSE(shouldOfferTranslation(french, "fr-CA"));

    CHECK(shouldOfferTranslation(
        LanguageDetectionResult{
            .languageCode = "en",
            .confidence = 0.97,
            .englishConfidence = 0.97,
        },
        "ja"));
    CHECK_FALSE(shouldOfferTranslation(
        LanguageDetectionResult{
            .languageCode = "en-US",
            .confidence = 0.97,
            .englishConfidence = 0.97,
        },
        "en-GB"));
    CHECK_FALSE(shouldOfferTranslation(
        LanguageDetectionResult{
            .languageCode = "de",
            .confidence = 0.52,
            .englishConfidence = 0.18,
        },
        "ja"));
    CHECK_FALSE(shouldOfferTranslation(
        LanguageDetectionResult{
            .languageCode = "es",
            .confidence = 0.88,
            .englishConfidence = 0.42,
        },
        "en"));
    CHECK(shouldOfferTranslation(
        LanguageDetectionResult{
            .languageCode = "es",
            .confidence = 0.88,
            .englishConfidence = 0.42,
        },
        "ja"));
}

TEST_CASE("language detection service ignores short snippets", "[jmap][language]")
{
    javelin::jmap::language::LanguageDetectionService service{std::string{}};

    CHECK_FALSE(service.detect("Bonjour").has_value());
}

TEST_CASE("language detection recognizes Japanese notification text", "[jmap][language]")
{
    javelin::jmap::language::LanguageDetectionService service{JAVELIN_FASTTEXT_LANGUAGE_MODEL_PATH};
    const auto detection = service.detect(
        "【Ci-en】 新着記事のお知らせ [2026/07/30] fuwafuwataimu様 4件の新着記事があります "
        "フォロー中のクリエイター whisp 🌟新作RPG体験版公開中🌟 しつけあい！"
        "発売カウントダウンイラスト 2026年07月29日");

    REQUIRE(detection.has_value());
    CHECK(detection->languageCode == "ja");
    CHECK(detection->confidence >= 0.75);
    CHECK(shouldOfferTranslation(*detection, "en"));
}
