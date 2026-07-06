#pragma once

#include "jmap/language/LanguageDetection.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace fasttext
{
    class FastText;
}

namespace javelin::jmap::language
{

    class FastTextLanguageDetector
    {
      public:
        explicit FastTextLanguageDetector(std::string modelPath);
        ~FastTextLanguageDetector();

        FastTextLanguageDetector(const FastTextLanguageDetector&) = delete;
        FastTextLanguageDetector& operator=(const FastTextLanguageDetector&) = delete;
        FastTextLanguageDetector(FastTextLanguageDetector&&) noexcept;
        FastTextLanguageDetector& operator=(FastTextLanguageDetector&&) noexcept;

        [[nodiscard]] std::optional<LanguageDetectionResult> detect(std::string_view utf8Text);

      private:
        std::string m_modelPath;
#if JAVELIN_ENABLE_FASTTEXT_LANGUAGE_DETECTION
        std::unique_ptr<fasttext::FastText> m_model;
#endif
    };

} // namespace javelin::jmap::language
