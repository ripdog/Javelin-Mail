#pragma once

#include "app/TranslationApplicationPorts.h"

#include <QCoroTask>

class QNetworkAccessManager;

namespace javelin::jmap::cache
{
    class TranslationCacheRepository;
}

namespace javelin::app
{
    class TranslationService final : public TranslationPort
    {
      public:
        using TranslationChunks = javelin::app::TranslationChunks;
        using Result = javelin::app::TranslationResult;

        TranslationService(
            QNetworkAccessManager& networkAccessManager,
            javelin::jmap::cache::TranslationCacheRepository& translationCacheRepository);

        [[nodiscard]] static TranslationSettings loadSettings();
        static void saveSettings(TranslationSettings settings);

        void applySettings(TranslationSettings settings);
        void reloadSettings() override;
        [[nodiscard]] const TranslationSettings& settings() const override;
        [[nodiscard]] bool isEnabled() const override;
        [[nodiscard]] QString targetLanguage() const override;
        [[nodiscard]] bool shouldAutoTranslate(const QString& sender,
                                               const QString& domain) const override;
        void setAutoTranslateSender(QString sender, bool enabled) override;
        void setAutoTranslateDomain(QString domain, bool enabled) override;

        [[nodiscard]] QCoro::Task<Result> translate(TranslationChunks sourceChunks,
                                                    QString sourceLanguage,
                                                    bool allowNetwork = true) override;

        struct PendingRequest
        {
            qsizetype chunkIndex = 0;
            QString requestText;
        };

      private:
        [[nodiscard]] static QString builtInApiKey();
        [[nodiscard]] static QVector<QVector<PendingRequest>>
        makeRequestBatches(const TranslationChunks& sourceChunks);
        [[nodiscard]] static QString transformRequest(const QStringList& sourceArray);
        [[nodiscard]] static QStringList transformResponse(QString result);

        QNetworkAccessManager& m_networkAccessManager;
        javelin::jmap::cache::TranslationCacheRepository& m_translationCacheRepository;
        TranslationSettings m_settings;
    };

} // namespace javelin::app
