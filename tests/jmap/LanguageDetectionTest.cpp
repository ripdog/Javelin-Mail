#include "jmap/language/LanguageDetection.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

TEST_CASE("language detection policy offers translation only for confident non-English text",
          "[jmap][language]")
{
    using javelin::jmap::language::LanguageDetectionResult;
    using javelin::jmap::language::shouldOfferTranslation;

    CHECK(shouldOfferTranslation(LanguageDetectionResult{
        .languageCode = "fr",
        .confidence = 0.91,
        .englishConfidence = 0.04,
    }));
    CHECK_FALSE(shouldOfferTranslation(LanguageDetectionResult{
        .languageCode = "en",
        .confidence = 0.97,
        .englishConfidence = 0.97,
    }));
    CHECK_FALSE(shouldOfferTranslation(LanguageDetectionResult{
        .languageCode = "de",
        .confidence = 0.52,
        .englishConfidence = 0.18,
    }));
    CHECK_FALSE(shouldOfferTranslation(LanguageDetectionResult{
        .languageCode = "es",
        .confidence = 0.88,
        .englishConfidence = 0.42,
    }));
}

TEST_CASE("language detection service ignores short snippets", "[jmap][language]")
{
    javelin::jmap::language::LanguageDetectionService service{std::string{}};

    CHECK_FALSE(service.detect("Bonjour").has_value());
}
