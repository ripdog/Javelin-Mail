#pragma once

#include "gui/translation/TranslationTypes.h"

#include <QSqlDatabase>
#include <QString>
#include <QVector>

#include <optional>
#include <variant>

namespace javelin::gui::translation
{
    struct TranslationCacheRecord
    {
        TranslationProvider provider = TranslationProvider::Google;
        QString sourceLanguage;
        QString targetLanguage;
        QString backendRevision;
        QString inputText;
        QString translatedText;
    };

    using TranslationCacheLookupResult = std::variant<std::optional<QString>, TranslationError>;

    class TranslationCache
    {
      public:
        explicit TranslationCache(QString databasePath = {});
        ~TranslationCache();

        TranslationCache(const TranslationCache&) = delete;
        TranslationCache& operator=(const TranslationCache&) = delete;
        TranslationCache(TranslationCache&&) = delete;
        TranslationCache& operator=(TranslationCache&&) = delete;

        [[nodiscard]] std::optional<TranslationError> open();
        [[nodiscard]] bool isOpen() const;
        [[nodiscard]] QString databasePath() const;
        [[nodiscard]] TranslationCacheLookupResult
        find(TranslationProvider provider, QStringView sourceLanguage, QStringView targetLanguage,
             QStringView backendRevision, QStringView inputText);
        [[nodiscard]] std::optional<TranslationError>
        upsert(const QVector<TranslationCacheRecord>& records);

      private:
        [[nodiscard]] std::optional<TranslationError> migrate();
        [[nodiscard]] std::optional<TranslationError> prune();
        [[nodiscard]] static QString inputHash(QStringView inputText);
        [[nodiscard]] TranslationError sqlError(TranslationErrorCode code,
                                                QStringView operation) const;

        QString m_databasePath;
        QString m_connectionName;
        QSqlDatabase m_database;
        qsizetype m_insertionsSincePrune = 0;
    };
} // namespace javelin::gui::translation
