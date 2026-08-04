#pragma once

#include "gui/translation/TranslationTypes.h"

#include <QCoroTask>

namespace javelin::gui::translation
{
    class TranslationBackend
    {
      public:
        virtual ~TranslationBackend() = default;

        [[nodiscard]] virtual QString revision(QStringView sourceLanguage,
                                               QStringView targetLanguage) const = 0;
        [[nodiscard]] virtual QCoro::Task<BackendResult> translate(BackendRequest request) = 0;
        virtual void releaseResources()
        {
        }
    };
} // namespace javelin::gui::translation
