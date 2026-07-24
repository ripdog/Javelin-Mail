#pragma once

#include <QCoroTask>

#include <QString>
#include <QStringList>
#include <QVector>

#include <variant>

class QNetworkAccessManager;

namespace javelin::jmap::cache
{
    class TranslationCacheRepository;
}

namespace javelin::app
{
    struct TranslationSettings
    {
        bool enabled = true;
        QString apiKeyOverride;
        QString targetLanguage = QStringLiteral("en");
        QStringList autoTranslateSenders;
        QStringList autoTranslateDomains;

        bool operator==(const TranslationSettings&) const = default;
    };

    struct TranslationUnavailable
    {
    };

    class TranslationService final
    {
      public:
        using TranslationChunks = QVector<QStringList>;
        using Result = std::variant<TranslationChunks, TranslationUnavailable, QString>;

        TranslationService(
            QNetworkAccessManager& networkAccessManager,
            javelin::jmap::cache::TranslationCacheRepository& translationCacheRepository);

        [[nodiscard]] static TranslationSettings loadSettings();
        static void saveSettings(TranslationSettings settings);

        void reloadSettings();
        [[nodiscard]] const TranslationSettings& settings() const;
        [[nodiscard]] bool isEnabled() const;
        [[nodiscard]] QString targetLanguage() const;
        [[nodiscard]] bool shouldAutoTranslate(const QString& sender, const QString& domain) const;
        void setAutoTranslateSender(QString sender, bool enabled);
        void setAutoTranslateDomain(QString domain, bool enabled);

        [[nodiscard]] QCoro::Task<Result>
        translate(TranslationChunks sourceChunks, QString sourceLanguage, bool allowNetwork = true);

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
        [[nodiscard]] static QString normalizedListValue(QString value);
        static void setListValue(QStringList& values, QString value, bool enabled);

        QNetworkAccessManager& m_networkAccessManager;
        javelin::jmap::cache::TranslationCacheRepository& m_translationCacheRepository;
        TranslationSettings m_settings;
    };

} // namespace javelin::app
