#pragma once

#include <QCoroTask>

#include <QString>
#include <QStringList>
#include <QVector>

#include <variant>

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

    using TranslationChunks = QVector<QStringList>;
    using TranslationResult = std::variant<TranslationChunks, TranslationUnavailable, QString>;

    [[nodiscard]] TranslationSettings normalizeTranslationSettings(TranslationSettings settings);
    [[nodiscard]] TranslationSettings loadTranslationSettings();
    void saveTranslationSettings(TranslationSettings settings);

    class TranslationPort
    {
      public:
        virtual ~TranslationPort() = default;

        virtual void reloadSettings() = 0;
        [[nodiscard]] virtual const TranslationSettings& settings() const = 0;
        [[nodiscard]] virtual bool isEnabled() const = 0;
        [[nodiscard]] virtual QString targetLanguage() const = 0;
        [[nodiscard]] virtual bool shouldAutoTranslate(const QString& sender,
                                                       const QString& domain) const = 0;
        virtual void setAutoTranslateSender(QString sender, bool enabled) = 0;
        virtual void setAutoTranslateDomain(QString domain, bool enabled) = 0;
        [[nodiscard]] virtual QCoro::Task<TranslationResult>
        translate(TranslationChunks sourceChunks, QString sourceLanguage,
                  bool allowNetwork = true) = 0;
    };
} // namespace javelin::app
