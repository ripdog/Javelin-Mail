#pragma once

#include "gui/translation/TranslationBackend.h"

class QNetworkAccessManager;

namespace javelin::gui::translation
{
    class GoogleTranslationBackend final : public TranslationBackend
    {
      public:
        explicit GoogleTranslationBackend(QNetworkAccessManager& networkAccessManager);

        void setApiKeyOverride(QString apiKeyOverride);
        [[nodiscard]] QString revision(QStringView sourceLanguage,
                                       QStringView targetLanguage) const override;
        [[nodiscard]] QCoro::Task<BackendResult> translate(BackendRequest request) override;

      private:
        [[nodiscard]] static QString builtInApiKey();

        QNetworkAccessManager& m_networkAccessManager;
        QString m_apiKeyOverride;
    };
} // namespace javelin::gui::translation
