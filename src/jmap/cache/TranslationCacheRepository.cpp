#include "jmap/cache/TranslationCacheRepository.h"

#include <QCryptographicHash>
#include <QSqlError>
#include <QSqlQuery>

namespace javelin::jmap::cache
{

    namespace
    {

        [[nodiscard]] DatabaseError makeQueryError(const QString& operation, const QSqlQuery& query)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = operation + QStringLiteral(": ") + query.lastError().text(),
            };
        }

        [[nodiscard]] QString cacheKey(const QString& inputText)
        {
            return QString::fromLatin1(
                QCryptographicHash::hash(inputText.toUtf8(), QCryptographicHash::Sha256).toHex());
        }

    } // namespace

    TranslationCacheRepository::TranslationCacheRepository(DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    std::variant<std::optional<QString>, DatabaseError>
    TranslationCacheRepository::find(const QString& sourceLanguage, const QString& targetLanguage,
                                     const QString& inputText) const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral("SELECT translated_text "
                                     "FROM translation_cache "
                                     "WHERE source_language = :source_language "
                                     "AND target_language = :target_language "
                                     "AND input_hash = :input_hash "
                                     "AND input_text = :input_text"));
        query.bindValue(QStringLiteral(":source_language"), sourceLanguage);
        query.bindValue(QStringLiteral(":target_language"), targetLanguage);
        query.bindValue(QStringLiteral(":input_hash"), cacheKey(inputText));
        query.bindValue(QStringLiteral(":input_text"), inputText);
        if (!query.exec())
        {
            return makeQueryError(QStringLiteral("Read translation cache"), query);
        }

        if (!query.next())
        {
            return std::optional<QString>{std::nullopt};
        }

        return std::optional<QString>{query.value(0).toString()};
    }

    std::optional<DatabaseError>
    TranslationCacheRepository::upsert(const TranslationCacheEntry& entry)
    {
        const DatabaseWriteScope writeScope{m_connection};
        if (const auto error = m_connection.validate())
        {
            return error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "INSERT INTO translation_cache ("
            "source_language, target_language, input_hash, input_text, translated_text, updated_at"
            ") VALUES ("
            ":source_language, :target_language, :input_hash, :input_text, :translated_text, "
            "CURRENT_TIMESTAMP"
            ") ON CONFLICT(source_language, target_language, input_hash) DO UPDATE SET "
            "input_text = excluded.input_text, "
            "translated_text = excluded.translated_text, "
            "updated_at = CURRENT_TIMESTAMP"));
        query.bindValue(QStringLiteral(":source_language"), entry.sourceLanguage);
        query.bindValue(QStringLiteral(":target_language"), entry.targetLanguage);
        query.bindValue(QStringLiteral(":input_hash"), cacheKey(entry.inputText));
        query.bindValue(QStringLiteral(":input_text"), entry.inputText);
        query.bindValue(QStringLiteral(":translated_text"), entry.translatedText);
        if (!query.exec())
        {
            return makeQueryError(QStringLiteral("Write translation cache"), query);
        }

        return std::nullopt;
    }

} // namespace javelin::jmap::cache
