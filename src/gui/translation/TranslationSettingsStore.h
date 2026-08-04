#pragma once

#include "gui/translation/TranslationTypes.h"

#include <optional>
#include <variant>

namespace javelin::gui::translation
{
    using TranslationSettingsResult = std::variant<TranslationSettings, TranslationError>;

    class TranslationSettingsStore
    {
      public:
        [[nodiscard]] TranslationSettingsResult load() const;
        [[nodiscard]] std::optional<TranslationError> save(TranslationSettings settings) const;

      private:
        [[nodiscard]] TranslationSettingsResult migrateLegacySettings() const;
    };
} // namespace javelin::gui::translation
