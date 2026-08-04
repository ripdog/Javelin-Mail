#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace javelin::gui::translation
{
    class FastTextLanguageDetector;

    struct LanguageDetectionResult
    {
        std::string languageCode;
        double confidence = 0.0;
        double englishConfidence = 0.0;
    };

    [[nodiscard]] bool shouldOfferTranslation(const LanguageDetectionResult& detection,
                                              std::string_view targetLanguageCode);

    class LanguageDetectionService
    {
      public:
        explicit LanguageDetectionService(std::string modelPath);
        ~LanguageDetectionService();

        LanguageDetectionService(const LanguageDetectionService&) = delete;
        LanguageDetectionService& operator=(const LanguageDetectionService&) = delete;
        LanguageDetectionService(LanguageDetectionService&&) = delete;
        LanguageDetectionService& operator=(LanguageDetectionService&&) = delete;

        [[nodiscard]] std::optional<LanguageDetectionResult> detect(std::string_view utf8Text);

      private:
        std::unique_ptr<FastTextLanguageDetector> m_detector;
    };
} // namespace javelin::gui::translation
