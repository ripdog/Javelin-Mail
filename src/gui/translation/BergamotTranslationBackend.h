#pragma once

#include "gui/translation/TranslationBackend.h"

#include <QThreadPool>

#include <memory>

namespace javelin::gui::translation
{
    class TranslationModelManifest;
    class TranslationModelStore;
    struct InstalledTranslationRoute;

    class BergamotTranslationBackend final : public TranslationBackend
    {
      public:
        BergamotTranslationBackend(const TranslationModelManifest& manifest,
                                   TranslationModelStore& modelStore);
        ~BergamotTranslationBackend() override;

        BergamotTranslationBackend(const BergamotTranslationBackend&) = delete;
        BergamotTranslationBackend& operator=(const BergamotTranslationBackend&) = delete;
        BergamotTranslationBackend(BergamotTranslationBackend&&) = delete;
        BergamotTranslationBackend& operator=(BergamotTranslationBackend&&) = delete;

        [[nodiscard]] QString revision(QStringView sourceLanguage,
                                       QStringView targetLanguage) const override;
        [[nodiscard]] QCoro::Task<BackendResult> translate(BackendRequest request) override;
        void releaseResources() override;
        void releaseResourcesAndWait() override;

      private:
        struct WorkerState;

        [[nodiscard]] BackendResult translateOnWorker(InstalledTranslationRoute installedRoute,
                                                      QVector<QString> texts);
        void clearLoadedModels();
        void destroyWorkerState();

        const TranslationModelManifest& m_manifest;
        TranslationModelStore& m_modelStore;
        QThreadPool m_workerPool;
        WorkerState* m_workerState = nullptr;
    };
} // namespace javelin::gui::translation
