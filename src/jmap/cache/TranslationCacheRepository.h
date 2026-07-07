#pragma once

#include "jmap/cache/Database.h"

#include <QString>

#include <optional>
#include <variant>

namespace javelin::jmap::cache
{

    struct TranslationCacheEntry
    {
        QString sourceLanguage;
        QString targetLanguage;
        QString inputText;
        QString translatedText;
    };

    class TranslationCacheRepository
    {
      public:
        explicit TranslationCacheRepository(DatabaseConnection& connection);

        [[nodiscard]] std::variant<std::optional<QString>, DatabaseError>
        find(const QString& sourceLanguage, const QString& targetLanguage,
             const QString& inputText) const;
        [[nodiscard]] std::optional<DatabaseError> upsert(const TranslationCacheEntry& entry);

      private:
        DatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::cache
