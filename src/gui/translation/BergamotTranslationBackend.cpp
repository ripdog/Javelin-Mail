#include "gui/translation/BergamotTranslationBackend.h"

#include "gui/translation/TranslationModelManifest.h"
#include "gui/translation/TranslationModelStore.h"

#include <QCoroFuture>

#include <KLocalizedString>

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QList>
#include <QtConcurrent>

#include <translator/parser.h>
#include <translator/response_options.h>
#include <translator/service.h>
#include <translator/translation_model.h>

#include <exception>
#include <memory>
#include <utility>
#include <vector>

namespace javelin::gui::translation
{
    namespace
    {
        using BergamotModel = marian::bergamot::TranslationModel;

        [[nodiscard]] QString yamlQuoted(QString value)
        {
            value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
            value.replace(QLatin1Char('"'), QStringLiteral("\\\""));
            return QStringLiteral("\"%1\"").arg(value);
        }

        [[nodiscard]] const TranslationModelFile*
        fileOfType(const TranslationModelDirection& direction, const QStringView type)
        {
            const auto found =
                std::ranges::find(direction.files, type, &TranslationModelFile::type);
            return found == direction.files.end() ? nullptr : &*found;
        }

        [[nodiscard]] QString prefixPath(const TranslationModelDirection& direction)
        {
#ifdef JAVELIN_SSPLIT_PREFIX_DIR
            QDir directory{QStringLiteral(JAVELIN_SSPLIT_PREFIX_DIR)};
            auto path = directory.filePath(
                QStringLiteral("nonbreaking_prefix.%1").arg(direction.mozillaSource));
            if (QFileInfo::exists(path))
            {
                return path;
            }
            path = directory.filePath(QStringLiteral("nonbreaking_prefix.en"));
            if (QFileInfo::exists(path))
            {
                return path;
            }
#endif
            return {};
        }

        [[nodiscard]] QString modelConfig(const InstalledTranslationModel& installed)
        {
            const auto& direction = *installed.direction;
            const auto* model = fileOfType(direction, QStringLiteral("model"));
            const auto* lex = fileOfType(direction, QStringLiteral("lex"));
            const auto* sharedVocab = fileOfType(direction, QStringLiteral("vocab"));
            const auto* sourceVocab = fileOfType(direction, QStringLiteral("srcvocab"));
            const auto* targetVocab = fileOfType(direction, QStringLiteral("trgvocab"));
            const auto sourceVocabPath = QDir{installed.directory}.filePath(
                (sharedVocab != nullptr ? sharedVocab : sourceVocab)->installedName);
            const auto targetVocabPath = QDir{installed.directory}.filePath(
                (sharedVocab != nullptr ? sharedVocab : targetVocab)->installedName);

            QString config =
                QStringLiteral("beam-size: 1\n"
                               "normalize: 1.0\n"
                               "word-penalty: 0\n"
                               "max-length-break: 128\n"
                               "mini-batch-words: 1024\n"
                               "workspace: 128\n"
                               "max-length-factor: 2.0\n"
                               "skip-cost: true\n"
                               "cpu-threads: 1\n"
                               "quiet: true\n"
                               "quiet-translation: true\n"
                               "gemm-precision: int8shiftAlphaAll\n"
                               "models:\n  - %1\n"
                               "vocabs:\n  - %2\n  - %3\n"
                               "shortlist:\n  - %4\n")
                    .arg(yamlQuoted(QDir{installed.directory}.filePath(model->installedName)),
                         yamlQuoted(sourceVocabPath), yamlQuoted(targetVocabPath),
                         yamlQuoted(QDir{installed.directory}.filePath(lex->installedName)));
            const auto prefix = prefixPath(direction);
            if (!prefix.isEmpty())
            {
                config += QStringLiteral("ssplit-prefix-file: %1\n").arg(yamlQuoted(prefix));
            }
            return config;
        }

        [[nodiscard]] QString modelKey(const InstalledTranslationModel& installed)
        {
            return QStringLiteral("%1:%2:%3")
                .arg(installed.direction->id(), installed.direction->modelVersion,
                     installed.direction->architecture);
        }

        [[nodiscard]] QString directionRevision(const TranslationModelManifest& manifest,
                                                const TranslationModelDirection& direction)
        {
            const auto* model = fileOfType(direction, QStringLiteral("model"));
            return QStringLiteral("%1:%2:%3")
                .arg(manifest.revision(), direction.id(), model->decompressedSha256);
        }
    } // namespace

    struct BergamotTranslationBackend::WorkerState
    {
        WorkerState()
            : service(
                  []
                  {
                      marian::bergamot::BlockingService::Config config;
                      config.cacheSize = 0;
                      return config;
                  }())
        {
        }

        [[nodiscard]] std::shared_ptr<BergamotModel>
        model(const InstalledTranslationModel& installed)
        {
            const auto key = modelKey(installed);
            if (const auto found = models.constFind(key); found != models.cend())
            {
                lru.removeAll(key);
                lru.push_back(key);
                return *found;
            }

            auto options = marian::bergamot::parseOptionsFromString(
                modelConfig(installed).toStdString(), false, installed.directory.toStdString());
            auto loaded = std::make_shared<BergamotModel>(options);
            models.insert(key, loaded);
            lru.push_back(key);
            while (lru.size() > 2)
            {
                models.remove(lru.takeFirst());
            }
            return loaded;
        }

        marian::bergamot::BlockingService service;
        QHash<QString, std::shared_ptr<BergamotModel>> models;
        QList<QString> lru;
    };

    BergamotTranslationBackend::BergamotTranslationBackend(const TranslationModelManifest& manifest,
                                                           TranslationModelStore& modelStore)
        : m_manifest(manifest), m_modelStore(modelStore)
    {
        m_workerPool.setMaxThreadCount(1);
        m_workerPool.setExpiryTimeout(-1);
    }

    BergamotTranslationBackend::~BergamotTranslationBackend()
    {
        m_workerPool.clear();
        destroyWorkerState();
        m_workerPool.waitForDone();
    }

    QString BergamotTranslationBackend::revision(const QStringView sourceLanguage,
                                                 const QStringView targetLanguage) const
    {
        const auto route = m_manifest.route(sourceLanguage, targetLanguage);
        if (!route.supported())
        {
            return {};
        }
        if (route.isIdentity())
        {
            return QStringLiteral("bergamot-identity-v1");
        }
        QStringList legs;
        legs.reserve(route.legs.size());
        for (const auto* direction : route.legs)
        {
            legs.push_back(directionRevision(m_manifest, *direction));
        }
        return QStringLiteral("bergamot-v0.6.0:%1").arg(legs.join(QLatin1Char('|')));
    }

    QCoro::Task<BackendResult> BergamotTranslationBackend::translate(BackendRequest request)
    {
        const auto route = m_manifest.route(request.sourceLanguage, request.targetLanguage);
        if (!route.supported())
        {
            co_return TranslationUnavailable{
                .reason = TranslationUnavailable::Reason::UnsupportedLanguageRoute,
            };
        }
        if (route.isIdentity())
        {
            co_return BackendTranslation{
                .texts = std::move(request.texts),
                .backendRevision = QStringLiteral("bergamot-identity-v1"),
            };
        }

        auto installed = co_await m_modelStore.ensureInstalled(route, request.fetchPolicy);
        if (const auto* unavailable = std::get_if<TranslationUnavailable>(&installed))
        {
            co_return *unavailable;
        }
        if (const auto* error = std::get_if<TranslationError>(&installed))
        {
            co_return *error;
        }
        auto installedRoute = std::get<InstalledTranslationRoute>(std::move(installed));
        auto future = QtConcurrent::run(
            &m_workerPool, [this, installedRoute = std::move(installedRoute),
                            texts = std::move(request.texts)]() mutable
            { return translateOnWorker(std::move(installedRoute), std::move(texts)); });
        co_return co_await qCoro(future).takeResult();
    }

    void BergamotTranslationBackend::releaseResources()
    {
        static_cast<void>(QtConcurrent::run(&m_workerPool, [this] { clearLoadedModels(); }));
    }

    void BergamotTranslationBackend::releaseResourcesAndWait()
    {
        auto future = QtConcurrent::run(&m_workerPool, [this] { clearLoadedModels(); });
        future.waitForFinished();
    }

    BackendResult
    BergamotTranslationBackend::translateOnWorker(InstalledTranslationRoute installedRoute,
                                                  QVector<QString> texts)
    {
        try
        {
            if (m_workerState == nullptr)
            {
                m_workerState = new WorkerState{};
            }
            std::vector<std::string> sourceTexts;
            sourceTexts.reserve(static_cast<std::size_t>(texts.size()));
            for (const auto& text : texts)
            {
                sourceTexts.push_back(text.toUtf8().toStdString());
            }
            std::vector<marian::bergamot::ResponseOptions> options(
                static_cast<std::size_t>(texts.size()));
            std::vector<marian::bergamot::Response> responses;
            if (installedRoute.legs.size() == 1)
            {
                responses = m_workerState->service.translateMultiple(
                    m_workerState->model(installedRoute.legs[0]), std::move(sourceTexts), options);
            }
            else
            {
                responses = m_workerState->service.pivotMultiple(
                    m_workerState->model(installedRoute.legs[0]),
                    m_workerState->model(installedRoute.legs[1]), std::move(sourceTexts), options);
            }
            if (responses.size() != static_cast<std::size_t>(texts.size()))
            {
                return TranslationError{
                    .code = TranslationErrorCode::InferenceFailed,
                    .message = i18n("Local translation returned incomplete text."),
                };
            }

            QVector<QString> translated;
            translated.reserve(texts.size());
            for (const auto& response : responses)
            {
                translated.push_back(QString::fromUtf8(response.getTranslatedText()));
            }
            QStringList revisions;
            for (const auto& leg : installedRoute.legs)
            {
                revisions.push_back(directionRevision(m_manifest, *leg.direction));
            }
            return BackendTranslation{
                .texts = std::move(translated),
                .backendRevision =
                    QStringLiteral("bergamot-v0.6.0:%1").arg(revisions.join(QLatin1Char('|'))),
            };
        }
        catch (const std::exception& exception)
        {
            return TranslationError{
                .code = TranslationErrorCode::InferenceFailed,
                .message =
                    i18n("Local translation failed: %1", QString::fromUtf8(exception.what())),
            };
        }
        catch (...)
        {
            return TranslationError{
                .code = TranslationErrorCode::InferenceFailed,
                .message = i18n("Local translation failed unexpectedly."),
            };
        }
    }

    void BergamotTranslationBackend::clearLoadedModels()
    {
        if (m_workerState != nullptr)
        {
            m_workerState->models.clear();
            m_workerState->lru.clear();
        }
    }

    void BergamotTranslationBackend::destroyWorkerState()
    {
        auto future = QtConcurrent::run(&m_workerPool,
                                        [this]
                                        {
                                            delete m_workerState;
                                            m_workerState = nullptr;
                                        });
        future.waitForFinished();
    }
} // namespace javelin::gui::translation
