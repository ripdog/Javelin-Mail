#include "jmap/language/FastTextLanguageDetector.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <sstream>
#include <utility>
#include <vector>

#if JAVELIN_ENABLE_FASTTEXT_LANGUAGE_DETECTION
#include <fasttext.h>
#endif

namespace javelin::jmap::language
{
    namespace
    {
        constexpr std::int32_t languagePredictionCount = 5;

        [[nodiscard]] std::string normalizeFastTextLabel(std::string label)
        {
            constexpr std::string_view prefix = "__label__";
            if (label.starts_with(prefix))
            {
                label.erase(0, prefix.size());
            }
            return label;
        }

        [[nodiscard]] std::string normalizeInputText(std::string_view utf8Text)
        {
            std::string normalized;
            normalized.reserve(utf8Text.size());

            bool previousWasSpace = true;
            for (std::size_t index = 0; index < utf8Text.size();)
            {
                const auto byte = static_cast<unsigned char>(utf8Text[index]);
                if (byte == 0xef && index + 2 < utf8Text.size() &&
                    static_cast<unsigned char>(utf8Text[index + 1]) == 0xbf &&
                    static_cast<unsigned char>(utf8Text[index + 2]) == 0xbc)
                {
                    index += 3;
                    continue;
                }

                if (std::isspace(byte) != 0)
                {
                    if (!previousWasSpace)
                    {
                        normalized.push_back(' ');
                        previousWasSpace = true;
                    }
                    ++index;
                    continue;
                }

                normalized.push_back(utf8Text[index]);
                previousWasSpace = false;
                ++index;
            }

            if (!normalized.empty() && normalized.back() == ' ')
            {
                normalized.pop_back();
            }
            return normalized;
        }
    } // namespace

    FastTextLanguageDetector::FastTextLanguageDetector(std::string modelPath)
        : m_modelPath(std::move(modelPath))
    {
    }

    FastTextLanguageDetector::~FastTextLanguageDetector() = default;
    FastTextLanguageDetector::FastTextLanguageDetector(FastTextLanguageDetector&&) noexcept =
        default;
    FastTextLanguageDetector&
    FastTextLanguageDetector::operator=(FastTextLanguageDetector&&) noexcept = default;

    std::optional<LanguageDetectionResult>
    FastTextLanguageDetector::detect(const std::string_view utf8Text)
    {
#if JAVELIN_ENABLE_FASTTEXT_LANGUAGE_DETECTION
        if (!m_model)
        {
            try
            {
                m_model = std::make_unique<fasttext::FastText>();
                m_model->loadModel(m_modelPath);
            }
            catch (const std::exception&)
            {
                m_model.reset();
                return std::nullopt;
            }
        }

        auto normalizedInput = normalizeInputText(utf8Text);
        if (normalizedInput.empty())
        {
            return std::nullopt;
        }

        std::istringstream input{std::move(normalizedInput)};
        std::vector<std::pair<fasttext::real, std::string>> predictions;
        m_model->predictLine(input, predictions, languagePredictionCount, 0.0F);

        if (predictions.empty())
        {
            return std::nullopt;
        }

        const auto top = std::ranges::max_element(predictions, {}, [](const auto& prediction)
                                                  { return prediction.first; });
        if (top == predictions.end())
        {
            return std::nullopt;
        }

        const auto english =
            std::ranges::find_if(predictions, [](const auto& prediction)
                                 { return normalizeFastTextLabel(prediction.second) == "en"; });

        return LanguageDetectionResult{
            .languageCode = normalizeFastTextLabel(top->second),
            .confidence = static_cast<double>(top->first),
            .englishConfidence =
                english == predictions.end() ? 0.0 : static_cast<double>(english->first),
        };
#else
        (void)utf8Text;
        return std::nullopt;
#endif
    }

} // namespace javelin::jmap::language
