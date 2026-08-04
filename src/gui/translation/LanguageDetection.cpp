#include "gui/translation/LanguageDetection.h"

#include "gui/translation/FastTextLanguageDetector.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <utility>

namespace javelin::gui::translation
{
    namespace
    {
        constexpr std::size_t minimumDetectionTextLength = 120;
        constexpr double minimumNonEnglishConfidence = 0.75;
        constexpr double maximumEnglishConfidence = 0.35;

        [[nodiscard]] std::string primaryLanguage(const std::string_view languageCode)
        {
            const auto separator = languageCode.find_first_of("-_");
            auto primary = std::string{languageCode.substr(0, separator)};
            std::ranges::transform(primary, primary.begin(), [](const unsigned char character)
                                   { return static_cast<char>(std::tolower(character)); });
            return primary;
        }
    } // namespace

    bool shouldOfferTranslation(const LanguageDetectionResult& detection,
                                const std::string_view targetLanguageCode)
    {
        const auto detectedLanguage = primaryLanguage(detection.languageCode);
        const auto targetLanguage = primaryLanguage(targetLanguageCode);
        if (detectedLanguage.empty() || targetLanguage.empty() ||
            detectedLanguage == targetLanguage ||
            detection.confidence < minimumNonEnglishConfidence)
        {
            return false;
        }
        return targetLanguage != "en" || detection.englishConfidence <= maximumEnglishConfidence;
    }

    LanguageDetectionService::LanguageDetectionService(std::string modelPath)
        : m_detector(std::make_unique<FastTextLanguageDetector>(std::move(modelPath)))
    {
    }

    LanguageDetectionService::~LanguageDetectionService() = default;

    std::optional<LanguageDetectionResult>
    LanguageDetectionService::detect(const std::string_view utf8Text)
    {
        if (utf8Text.size() < minimumDetectionTextLength)
        {
            return std::nullopt;
        }
        return m_detector->detect(utf8Text);
    }
} // namespace javelin::gui::translation
