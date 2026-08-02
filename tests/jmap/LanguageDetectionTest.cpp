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

TEST_CASE("language detection recognizes Japanese body text", "[jmap][language]")
{
    javelin::jmap::language::LanguageDetectionService service{JAVELIN_FASTTEXT_LANGUAGE_MODEL_PATH};
    const auto detection = service.detect(
        "クリエイター新着記事のお知らせ2026年07月30日版fuwafuwataimu様4件の新着記事があります"
        "フォロー中のクリエイターwhisp新作RPG体験版公開中しつけあいあと3日発売カウントダウン"
        "イラスト2026年07月29日本日発売新作朧里之巫女小悪魔妹巫女と汗だくエクササイズで"
        "すっきり爽やかリフレッシュ線消しイラスト川の妖精たちと川辺で遊ぶこのメールは"
        "お知らせメール機能をご利用の登録ユーザー様にお送りしていますお知らせ設定をご利用ください");

    REQUIRE(detection.has_value());
    CHECK(detection->languageCode == "ja");
    CHECK(detection->confidence >= 0.75);
    CHECK(shouldOfferTranslation(*detection, "en"));
}
