#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace javelin::jmap::language
{

    struct LanguageDetectionResult
    {
        std::string languageCode;
        double confidence = 0.0;
        double englishConfidence = 0.0;
    };

    [[nodiscard]] bool shouldOfferTranslation(const LanguageDetectionResult& detection);

    class LanguageDetectionService
    {
      public:
        explicit LanguageDetectionService(std::string modelPath);

        [[nodiscard]] std::optional<LanguageDetectionResult> detect(std::string_view utf8Text);

      private:
        std::string m_modelPath;
    };

} // namespace javelin::jmap::language
