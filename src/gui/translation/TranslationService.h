#pragma once

#include "gui/translation/LanguageDetection.h"
#include "gui/translation/TranslationTypes.h"

#include <QCoroTask>

#include <QFuture>
#include <QObject>
#include <QThreadPool>

#include <memory>
#include <optional>
#include <string>

namespace javelin::gui::translation
{
    class GoogleTranslationBackend;
    class LanguageDetectionService;
    class TranslationBackend;
    class TranslationCache;
    class TranslationModelManifest;
    class TranslationModelStore;
    class TranslationSettingsStore;

    class TranslationService final : public QObject
    {
        Q_OBJECT

      public:
        TranslationService(TranslationSettingsStore& settingsStore, TranslationCache& cache,
                           GoogleTranslationBackend& googleBackend, std::string languageModelPath,
                           TranslationBackend* localBackend = nullptr,
                           TranslationModelManifest* localManifest = nullptr,
                           TranslationModelStore* localModelStore = nullptr,
                           QObject* parent = nullptr);
        ~TranslationService() override;

        [[nodiscard]] const TranslationSettings& settings() const;
        [[nodiscard]] bool isEnabled() const;
        [[nodiscard]] bool localProviderAvailable() const;
        [[nodiscard]] QString targetLanguage() const;
        [[nodiscard]] QStringList localSourceLanguages() const;
        [[nodiscard]] QStringList localTargetLanguages(QStringView sourceLanguage) const;
        [[nodiscard]] QVector<LocalModelInfo> installedLocalModels() const;
        [[nodiscard]] QCoro::Task<std::optional<TranslationError>>
        installLocalModels(QString sourceLanguage, QString targetLanguage);
        [[nodiscard]] std::optional<TranslationError> removeLocalModels(QString sourceLanguage,
                                                                        QString targetLanguage);
        [[nodiscard]] bool shouldAutoTranslate(QStringView sender, QStringView domain) const;
        [[nodiscard]] std::optional<TranslationError> setAutoTranslateSender(QString sender,
                                                                             bool enabled);
        [[nodiscard]] std::optional<TranslationError> setAutoTranslateDomain(QString domain,
                                                                             bool enabled);
        [[nodiscard]] QFuture<std::optional<LanguageDetectionResult>> detectLanguage(QString text);
        [[nodiscard]] QCoro::Task<TranslationResult> translate(TranslationChunks chunks,
                                                               QString sourceLanguage,
                                                               ExternalFetchPolicy fetchPolicy);
        [[nodiscard]] std::optional<TranslationError> reloadSettings();
        [[nodiscard]] std::optional<TranslationError> saveSettings(TranslationSettings settings);
        void releaseLocalModels();

      Q_SIGNALS:
        void settingsChanged();
        void localModelDownloadProgress(QString direction, qint64 received, qint64 total);
        void installedLocalModelsChanged();
        void diagnosticOccurred(QString message);

      private:
        void applySettings(TranslationSettings settings);
        [[nodiscard]] TranslationBackend* selectedBackend();
        [[nodiscard]] TranslationProvider selectedProvider() const;

        TranslationSettingsStore& m_settingsStore;
        TranslationCache& m_cache;
        GoogleTranslationBackend& m_googleBackend;
        TranslationBackend* m_localBackend = nullptr;
        TranslationModelManifest* m_localManifest = nullptr;
        TranslationModelStore* m_localModelStore = nullptr;
        TranslationSettings m_settings;
        std::optional<TranslationError> m_initializationError;
        std::string m_languageModelPath;
        QThreadPool m_detectionPool;
        std::unique_ptr<LanguageDetectionService> m_languageDetector;
    };
} // namespace javelin::gui::translation
