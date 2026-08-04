#include "gui/translation/TranslationService.h"

#include "gui/translation/GoogleTranslationBackend.h"
#include "gui/translation/LanguageDetection.h"
#include "gui/translation/TranslationBackend.h"
#include "gui/translation/TranslationCache.h"
#include "gui/translation/TranslationSettingsStore.h"
#if JAVELIN_ENABLE_BERGAMOT_TRANSLATION
#include "gui/translation/TranslationModelManifest.h"
#include "gui/translation/TranslationModelStore.h"
#endif

#include <QHash>
#include <QSet>
#include <QtConcurrent>

#include <utility>

namespace javelin::gui::translation
{
    TranslationService::TranslationService(TranslationSettingsStore& settingsStore,
                                           TranslationCache& cache,
                                           GoogleTranslationBackend& googleBackend,
                                           std::string languageModelPath,
                                           TranslationBackend* localBackend,
                                           TranslationModelManifest* localManifest,
                                           TranslationModelStore* localModelStore, QObject* parent)
        : QObject(parent), m_settingsStore(settingsStore), m_cache(cache),
          m_googleBackend(googleBackend), m_localBackend(localBackend),
          m_localManifest(localManifest), m_localModelStore(localModelStore),
          m_languageModelPath(std::move(languageModelPath))
    {
        m_detectionPool.setMaxThreadCount(1);
        m_detectionPool.setExpiryTimeout(-1);

        const auto loaded = m_settingsStore.load();
        if (const auto* error = std::get_if<TranslationError>(&loaded))
        {
            m_initializationError = *error;
            applySettings(TranslationSettings{});
        }
        else
        {
            auto settings = std::get<TranslationSettings>(loaded);
            if (settings.provider == TranslationProvider::Local && m_localBackend == nullptr)
            {
                settings.provider = TranslationProvider::Google;
                if (const auto saveError = m_settingsStore.save(settings))
                {
                    m_initializationError = *saveError;
                }
            }
            applySettings(std::move(settings));
        }

        if (const auto error = m_cache.open())
        {
            m_initializationError = *error;
        }
#if JAVELIN_ENABLE_BERGAMOT_TRANSLATION
        if (m_localModelStore != nullptr)
        {
            connect(m_localModelStore, &TranslationModelStore::downloadProgress, this,
                    &TranslationService::localModelDownloadProgress);
            connect(m_localModelStore, &TranslationModelStore::installedModelsChanged, this,
                    &TranslationService::installedLocalModelsChanged);
        }
#endif
    }

    TranslationService::~TranslationService()
    {
        m_detectionPool.clear();
        m_detectionPool.waitForDone();
        m_languageDetector.reset();
        releaseLocalModels();
    }

    const TranslationSettings& TranslationService::settings() const
    {
        return m_settings;
    }

    bool TranslationService::isEnabled() const
    {
        return m_settings.provider != TranslationProvider::Disabled;
    }

    bool TranslationService::localProviderAvailable() const
    {
        return m_localBackend != nullptr;
    }

    QString TranslationService::targetLanguage() const
    {
        return m_settings.targetLanguage;
    }

    QStringList TranslationService::localSourceLanguages() const
    {
#if JAVELIN_ENABLE_BERGAMOT_TRANSLATION
        if (m_localManifest == nullptr)
        {
            return {};
        }
        QSet<QString> languages;
        for (const auto& direction : m_localManifest->directions())
        {
            languages.insert(direction.source);
            languages.insert(direction.target);
        }
        auto result = languages.values();
        result.sort(Qt::CaseInsensitive);
        return result;
#else
        return {};
#endif
    }

    QStringList TranslationService::localTargetLanguages(const QStringView sourceLanguage) const
    {
#if JAVELIN_ENABLE_BERGAMOT_TRANSLATION
        if (m_localManifest == nullptr)
        {
            return {};
        }
        QStringList targets;
        for (const auto& candidate : localSourceLanguages())
        {
            const auto route = m_localManifest->route(sourceLanguage, candidate);
            if (route.supported() && !route.isIdentity())
            {
                targets.push_back(candidate);
            }
        }
        return targets;
#else
        (void)sourceLanguage;
        return {};
#endif
    }

    QVector<LocalModelInfo> TranslationService::installedLocalModels() const
    {
#if JAVELIN_ENABLE_BERGAMOT_TRANSLATION
        QVector<LocalModelInfo> result;
        if (m_localModelStore == nullptr)
        {
            return result;
        }
        for (const auto& installed : m_localModelStore->installedModels())
        {
            result.push_back({
                .sourceLanguage = installed.direction->source,
                .targetLanguage = installed.direction->target,
                .modelVersion = installed.direction->modelVersion,
                .architecture = installed.direction->architecture,
                .diskSize = installed.diskSize,
            });
        }
        return result;
#else
        return {};
#endif
    }

    QCoro::Task<std::optional<TranslationError>>
    TranslationService::installLocalModels(QString sourceLanguage, QString targetLanguage)
    {
#if JAVELIN_ENABLE_BERGAMOT_TRANSLATION
        if (m_localManifest == nullptr || m_localModelStore == nullptr)
        {
            co_return TranslationError{
                .code = TranslationErrorCode::ModelDownloadFailed,
                .message = QStringLiteral("Local translation is not available in this build."),
            };
        }
        const auto route = m_localManifest->route(sourceLanguage, targetLanguage);
        if (!route.supported() || route.isIdentity())
        {
            co_return TranslationError{
                .code = TranslationErrorCode::ModelDownloadFailed,
                .message = QStringLiteral("That local translation route is not supported."),
            };
        }
        auto installed = co_await m_localModelStore->ensureInstalled(
            route, ExternalFetchPolicy::AllowExternalFetch);
        if (const auto* error = std::get_if<TranslationError>(&installed))
        {
            co_return *error;
        }
        if (std::holds_alternative<TranslationUnavailable>(installed))
        {
            co_return TranslationError{
                .code = TranslationErrorCode::ModelDownloadFailed,
                .message = QStringLiteral("The required local translation models are unavailable."),
            };
        }
        co_return std::nullopt;
#else
        (void)sourceLanguage;
        (void)targetLanguage;
        co_return TranslationError{
            .code = TranslationErrorCode::ModelDownloadFailed,
            .message = QStringLiteral("Local translation is not available in this build."),
        };
#endif
    }

    std::optional<TranslationError> TranslationService::removeLocalModels(QString sourceLanguage,
                                                                          QString targetLanguage)
    {
#if JAVELIN_ENABLE_BERGAMOT_TRANSLATION
        if (m_localManifest == nullptr || m_localModelStore == nullptr)
        {
            return TranslationError{
                .code = TranslationErrorCode::ModelVerificationFailed,
                .message = QStringLiteral("Local translation is not available in this build."),
            };
        }
        const auto route = m_localManifest->route(sourceLanguage, targetLanguage);
        if (!route.supported() || route.isIdentity())
        {
            return TranslationError{
                .code = TranslationErrorCode::ModelVerificationFailed,
                .message = QStringLiteral("That local translation route is not supported."),
            };
        }
        releaseLocalModels();
        return m_localModelStore->remove(route.legs);
#else
        (void)sourceLanguage;
        (void)targetLanguage;
        return TranslationError{
            .code = TranslationErrorCode::ModelVerificationFailed,
            .message = QStringLiteral("Local translation is not available in this build."),
        };
#endif
    }

    bool TranslationService::shouldAutoTranslate(const QStringView sender,
                                                 const QStringView domain) const
    {
        return isEnabled() && ((!sender.isEmpty() && m_settings.autoTranslateSenders.contains(
                                                         sender.toString(), Qt::CaseInsensitive)) ||
                               (!domain.isEmpty() && m_settings.autoTranslateDomains.contains(
                                                         domain.toString(), Qt::CaseInsensitive)));
    }

    std::optional<TranslationError> TranslationService::setAutoTranslateSender(QString sender,
                                                                               const bool enabled)
    {
        auto updated = m_settings;
        setTranslationListValue(updated.autoTranslateSenders, std::move(sender), enabled);
        return saveSettings(std::move(updated));
    }

    std::optional<TranslationError> TranslationService::setAutoTranslateDomain(QString domain,
                                                                               const bool enabled)
    {
        auto updated = m_settings;
        setTranslationListValue(updated.autoTranslateDomains, std::move(domain), enabled);
        return saveSettings(std::move(updated));
    }

    QFuture<std::optional<LanguageDetectionResult>> TranslationService::detectLanguage(QString text)
    {
        if (!isEnabled())
        {
            return QtFuture::makeReadyValueFuture(std::optional<LanguageDetectionResult>{});
        }
        const auto utf8Text = text.toUtf8().toStdString();
        return QtConcurrent::run(&m_detectionPool,
                                 [this, utf8Text]() -> std::optional<LanguageDetectionResult>
                                 {
                                     if (!m_languageDetector)
                                     {
                                         m_languageDetector =
                                             std::make_unique<LanguageDetectionService>(
                                                 m_languageModelPath);
                                     }
                                     return m_languageDetector->detect(utf8Text);
                                 });
    }

    QCoro::Task<TranslationResult>
    TranslationService::translate(TranslationChunks chunks, QString sourceLanguage,
                                  const ExternalFetchPolicy fetchPolicy)
    {
        if (!isEnabled())
        {
            co_return TranslationUnavailable{
                .reason = TranslationUnavailable::Reason::Disabled,
            };
        }
        if (m_initializationError.has_value())
        {
            co_return *m_initializationError;
        }
        if (chunks.empty())
        {
            co_return TranslationChunks{};
        }

        sourceLanguage = canonicalLanguageTag(std::move(sourceLanguage));
        if (sourceLanguage.isEmpty())
        {
            sourceLanguage = QStringLiteral("auto");
        }
        if (sourceLanguage == targetLanguage())
        {
            co_return chunks;
        }
        if (m_settings.provider == TranslationProvider::Local &&
            sourceLanguage == QStringLiteral("auto"))
        {
            co_return TranslationUnavailable{
                .reason = TranslationUnavailable::Reason::SourceLanguageUnknown,
            };
        }

        auto* backend = selectedBackend();
        if (backend == nullptr)
        {
            co_return TranslationUnavailable{
                .reason = TranslationUnavailable::Reason::UnsupportedLanguageRoute,
            };
        }

        const auto provider = selectedProvider();
        const auto targetLanguage = m_settings.targetLanguage;
        const auto revision = backend->revision(sourceLanguage, targetLanguage);
        if (revision.isEmpty())
        {
            co_return TranslationUnavailable{
                .reason = TranslationUnavailable::Reason::UnsupportedLanguageRoute,
            };
        }

        QVector<QString> uniqueTexts;
        QHash<QString, qsizetype> textIndexes;
        for (const auto& chunk : chunks)
        {
            for (const auto& text : chunk)
            {
                if (text.isEmpty() || textIndexes.contains(text))
                {
                    continue;
                }
                const auto index = uniqueTexts.size();
                uniqueTexts.push_back(text);
                textIndexes.insert(text, index);
            }
        }
        if (uniqueTexts.empty())
        {
            co_return chunks;
        }

        QVector<QString> translations;
        translations.resize(uniqueTexts.size());
        QVector<QString> misses;
        QVector<qsizetype> missIndexes;
        for (qsizetype index = 0; index < uniqueTexts.size(); ++index)
        {
            const auto cached = m_cache.find(provider, sourceLanguage, targetLanguage, revision,
                                             uniqueTexts[index]);
            if (const auto* error = std::get_if<TranslationError>(&cached))
            {
                co_return *error;
            }
            const auto& value = std::get<std::optional<QString>>(cached);
            if (value.has_value())
            {
                translations[index] = *value;
            }
            else
            {
                misses.push_back(uniqueTexts[index]);
                missIndexes.push_back(index);
            }
        }

        if (!misses.empty())
        {
            const auto backendResult = co_await backend->translate({
                .sourceLanguage = sourceLanguage,
                .targetLanguage = targetLanguage,
                .texts = misses,
                .fetchPolicy = fetchPolicy,
            });
            if (const auto* unavailable = std::get_if<TranslationUnavailable>(&backendResult))
            {
                co_return *unavailable;
            }
            if (const auto* error = std::get_if<TranslationError>(&backendResult))
            {
                co_return *error;
            }
            const auto& translated = std::get<BackendTranslation>(backendResult);
            if (translated.texts.size() != misses.size())
            {
                co_return TranslationError{
                    .code = TranslationErrorCode::InferenceFailed,
                    .message = QStringLiteral("The translation provider returned incomplete text."),
                };
            }

            QVector<TranslationCacheRecord> records;
            records.reserve(misses.size());
            for (qsizetype index = 0; index < misses.size(); ++index)
            {
                translations[missIndexes[index]] = translated.texts[index];
                records.push_back({
                    .provider = provider,
                    .sourceLanguage = sourceLanguage,
                    .targetLanguage = targetLanguage,
                    .backendRevision = translated.backendRevision,
                    .inputText = misses[index],
                    .translatedText = translated.texts[index],
                });
            }
            if (const auto error = m_cache.upsert(records))
            {
                Q_EMIT diagnosticOccurred(error->message);
            }
        }

        TranslationChunks result;
        result.reserve(chunks.size());
        for (const auto& chunk : chunks)
        {
            QStringList translatedChunk;
            translatedChunk.reserve(chunk.size());
            for (const auto& text : chunk)
            {
                if (text.isEmpty())
                {
                    translatedChunk.push_back(QString{});
                }
                else
                {
                    translatedChunk.push_back(translations[textIndexes.value(text)]);
                }
            }
            result.push_back(std::move(translatedChunk));
        }
        co_return result;
    }

    std::optional<TranslationError> TranslationService::reloadSettings()
    {
        const auto loaded = m_settingsStore.load();
        if (const auto* error = std::get_if<TranslationError>(&loaded))
        {
            return *error;
        }
        applySettings(std::get<TranslationSettings>(loaded));
        Q_EMIT settingsChanged();
        return std::nullopt;
    }

    std::optional<TranslationError> TranslationService::saveSettings(TranslationSettings settings)
    {
        settings = normalizeTranslationSettings(std::move(settings));
        if (settings.provider == TranslationProvider::Local && m_localBackend == nullptr)
        {
            return TranslationError{
                .code = TranslationErrorCode::SettingsWriteFailed,
                .message = QStringLiteral("Local translation is not available in this build."),
            };
        }
        if (const auto error = m_settingsStore.save(settings))
        {
            return error;
        }
        applySettings(std::move(settings));
        m_initializationError.reset();
        Q_EMIT settingsChanged();
        return std::nullopt;
    }

    void TranslationService::releaseLocalModels()
    {
        if (m_localBackend != nullptr)
        {
            m_localBackend->releaseResources();
        }
    }

    void TranslationService::applySettings(TranslationSettings settings)
    {
        const auto previousProvider = m_settings.provider;
        m_settings = normalizeTranslationSettings(std::move(settings));
        m_googleBackend.setApiKeyOverride(m_settings.apiKeyOverride);
        if (previousProvider == TranslationProvider::Local &&
            m_settings.provider != TranslationProvider::Local)
        {
            releaseLocalModels();
        }
    }

    TranslationBackend* TranslationService::selectedBackend()
    {
        switch (m_settings.provider)
        {
        case TranslationProvider::Disabled:
            return nullptr;
        case TranslationProvider::Google:
            return &m_googleBackend;
        case TranslationProvider::Local:
            return m_localBackend;
        }
        return nullptr;
    }

    TranslationProvider TranslationService::selectedProvider() const
    {
        return m_settings.provider;
    }
} // namespace javelin::gui::translation
