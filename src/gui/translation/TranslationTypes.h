#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>
#include <variant>

namespace javelin::gui::translation
{
    enum class TranslationProvider
    {
        Disabled,
        Google,
        Local,
    };

    struct TranslationSettings
    {
        TranslationProvider provider = TranslationProvider::Google;
        QString apiKeyOverride;
        QString targetLanguage = QStringLiteral("en");
        QStringList autoTranslateSenders;
        QStringList autoTranslateDomains;

        bool operator==(const TranslationSettings&) const = default;
    };

    enum class ExternalFetchPolicy
    {
        InstalledAndCachedOnly,
        AllowExternalFetch,
    };

    using TranslationChunks = QVector<QStringList>;

    struct TranslationUnavailable
    {
        enum class Reason
        {
            Disabled,
            SourceLanguageUnknown,
            UnsupportedLanguageRoute,
            RequiredModelNotInstalled,
            ExternalFetchNotAllowed,
        };

        Reason reason = Reason::UnsupportedLanguageRoute;
    };

    enum class TranslationErrorCode
    {
        SettingsReadFailed,
        SettingsWriteFailed,
        CacheOpenFailed,
        CacheReadFailed,
        CacheWriteFailed,
        GoogleRequestFailed,
        GoogleResponseInvalid,
        ManifestInvalid,
        ModelDownloadFailed,
        ModelVerificationFailed,
        ModelLoadFailed,
        InferenceFailed,
    };

    struct TranslationError
    {
        TranslationErrorCode code = TranslationErrorCode::InferenceFailed;
        QString message;
    };

    using TranslationResult =
        std::variant<TranslationChunks, TranslationUnavailable, TranslationError>;

    struct BackendRequest
    {
        QString sourceLanguage;
        QString targetLanguage;
        QVector<QString> texts;
        ExternalFetchPolicy fetchPolicy = ExternalFetchPolicy::InstalledAndCachedOnly;
    };

    struct BackendTranslation
    {
        QVector<QString> texts;
        QString backendRevision;
    };

    struct LocalModelInfo
    {
        QString sourceLanguage;
        QString targetLanguage;
        QString modelVersion;
        QString architecture;
        qint64 diskSize = 0;
    };

    using BackendResult =
        std::variant<BackendTranslation, TranslationUnavailable, TranslationError>;

    [[nodiscard]] QString canonicalLanguageTag(QString languageTag);
    [[nodiscard]] QString googleLanguageTag(QStringView canonicalTag);
    [[nodiscard]] QString mozillaLanguageTag(QStringView canonicalTag);
    [[nodiscard]] QString translationProviderStorageName(TranslationProvider provider);
    [[nodiscard]] std::optional<TranslationProvider>
    translationProviderFromStorage(QStringView value);
    [[nodiscard]] TranslationSettings normalizeTranslationSettings(TranslationSettings settings);
    void setTranslationListValue(QStringList& values, QString value, bool enabled);
} // namespace javelin::gui::translation
