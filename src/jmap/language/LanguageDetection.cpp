#include "jmap/language/LanguageDetection.h"

#include "jmap/language/FastTextLanguageDetector.h"

#include <cstddef>
#include <utility>

namespace javelin::jmap::language
{
    namespace
    {
        constexpr std::size_t minimumDetectionTextLength = 120;
        constexpr double minimumNonEnglishConfidence = 0.75;
        constexpr double maximumEnglishConfidence = 0.35;
    } // namespace

    bool shouldOfferTranslation(const LanguageDetectionResult& detection)
    {
        return detection.languageCode != "en" &&
               detection.confidence >= minimumNonEnglishConfidence &&
               detection.englishConfidence <= maximumEnglishConfidence;
    }

    LanguageDetectionService::LanguageDetectionService(std::string modelPath)
        : m_modelPath(std::move(modelPath))
    {
    }

    std::optional<LanguageDetectionResult>
    LanguageDetectionService::detect(const std::string_view utf8Text)
    {
        if (utf8Text.size() < minimumDetectionTextLength)
        {
            return std::nullopt;
        }

        FastTextLanguageDetector detector{m_modelPath};
        return detector.detect(utf8Text);
    }

} // namespace javelin::jmap::language
