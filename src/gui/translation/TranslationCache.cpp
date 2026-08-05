#include "gui/translation/TranslationCache.h"

#include <KLocalizedString>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>

#include <utility>

namespace javelin::gui::translation
{
    namespace
    {
        constexpr qsizetype maximumRows = 100000;
        constexpr qsizetype pruneInterval = 500;

        [[nodiscard]] QString defaultDatabasePath()
        {
            return QDir{QStandardPaths::writableLocation(QStandardPaths::CacheLocation)}.filePath(
                QStringLiteral("translations/cache-v1.sqlite3"));
        }

        [[nodiscard]] bool execute(QSqlDatabase& database, const QString& statement)
        {
            QSqlQuery query{database};
            return query.exec(statement);
        }
    } // namespace

    TranslationCache::TranslationCache(QString databasePath)
        : m_databasePath(databasePath.isEmpty() ? defaultDatabasePath() : std::move(databasePath)),
          m_connectionName(QStringLiteral("javelin-translation-cache-%1")
                               .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
    {
    }

    TranslationCache::~TranslationCache()
    {
        if (m_database.isValid())
        {
            m_database.close();
            m_database = QSqlDatabase{};
        }
        if (!m_connectionName.isEmpty())
        {
            QSqlDatabase::removeDatabase(m_connectionName);
        }
    }

    std::optional<TranslationError> TranslationCache::open()
    {
        if (m_database.isOpen())
        {
            return std::nullopt;
        }

        const QFileInfo databaseInfo{m_databasePath};
        if (!QDir{}.mkpath(databaseInfo.absolutePath()))
        {
            return TranslationError{
                .code = TranslationErrorCode::CacheOpenFailed,
                .message = i18n("Could not create the translation cache directory."),
            };
        }

        m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
        m_database.setDatabaseName(m_databasePath);
        if (!m_database.open())
        {
            return sqlError(TranslationErrorCode::CacheOpenFailed,
                            QStringLiteral("open the translation cache"));
        }

        const QStringList pragmas{
            QStringLiteral("PRAGMA journal_mode = WAL"),
            QStringLiteral("PRAGMA synchronous = NORMAL"),
            QStringLiteral("PRAGMA busy_timeout = 5000"),
            QStringLiteral("PRAGMA foreign_keys = ON"),
        };
        for (const auto& pragma : pragmas)
        {
            if (!execute(m_database, pragma))
            {
                return sqlError(TranslationErrorCode::CacheOpenFailed,
                                QStringLiteral("configure the translation cache"));
            }
        }
        if (const auto error = migrate())
        {
            return error;
        }
        return prune();
    }

    bool TranslationCache::isOpen() const
    {
        return m_database.isOpen();
    }

    QString TranslationCache::databasePath() const
    {
        return m_databasePath;
    }

    TranslationCacheLookupResult TranslationCache::find(const TranslationProvider provider,
                                                        const QStringView sourceLanguage,
                                                        const QStringView targetLanguage,
                                                        const QStringView backendRevision,
                                                        const QStringView inputText)
    {
        if (const auto error = open())
        {
            return *error;
        }

        QSqlQuery query{m_database};
        query.prepare(
            QStringLiteral("SELECT input_text, translated_text FROM translations "
                           "WHERE provider = ? AND source_language = ? AND target_language = ? "
                           "AND backend_revision = ? AND input_hash = ?"));
        query.addBindValue(translationProviderStorageName(provider));
        query.addBindValue(sourceLanguage.toString());
        query.addBindValue(targetLanguage.toString());
        query.addBindValue(backendRevision.toString());
        query.addBindValue(inputHash(inputText));
        if (!query.exec())
        {
            return sqlError(TranslationErrorCode::CacheReadFailed,
                            QStringLiteral("read the translation cache"));
        }
        if (!query.next())
        {
            return std::optional<QString>{};
        }
        if (query.value(0).toString() != inputText)
        {
            return std::optional<QString>{};
        }
        return std::optional<QString>{query.value(1).toString()};
    }

    std::optional<TranslationError>
    TranslationCache::upsert(const QVector<TranslationCacheRecord>& records)
    {
        if (records.empty())
        {
            return std::nullopt;
        }
        if (const auto error = open())
        {
            return error;
        }
        if (!m_database.transaction())
        {
            return sqlError(TranslationErrorCode::CacheWriteFailed,
                            QStringLiteral("start a translation cache transaction"));
        }

        QSqlQuery query{m_database};
        query.prepare(QStringLiteral(
            "INSERT INTO translations (provider, source_language, target_language, "
            "backend_revision, input_hash, input_text, translated_text, updated_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(provider, source_language, target_language, backend_revision, input_hash) "
            "DO UPDATE SET input_text = excluded.input_text, "
            "translated_text = excluded.translated_text, updated_at = excluded.updated_at"));
        const auto updatedAt = QDateTime::currentSecsSinceEpoch();
        for (const auto& record : records)
        {
            query.bindValue(0, translationProviderStorageName(record.provider));
            query.bindValue(1, record.sourceLanguage);
            query.bindValue(2, record.targetLanguage);
            query.bindValue(3, record.backendRevision);
            query.bindValue(4, inputHash(record.inputText));
            query.bindValue(5, record.inputText);
            query.bindValue(6, record.translatedText);
            query.bindValue(7, updatedAt);
            if (!query.exec())
            {
                m_database.rollback();
                return sqlError(TranslationErrorCode::CacheWriteFailed,
                                QStringLiteral("write the translation cache"));
            }
        }
        if (!m_database.commit())
        {
            m_database.rollback();
            return sqlError(TranslationErrorCode::CacheWriteFailed,
                            QStringLiteral("commit the translation cache"));
        }

        m_insertionsSincePrune += records.size();
        if (m_insertionsSincePrune >= pruneInterval)
        {
            m_insertionsSincePrune = 0;
            return prune();
        }
        return std::nullopt;
    }

    std::optional<TranslationError> TranslationCache::migrate()
    {
        if (!m_database.transaction())
        {
            return sqlError(TranslationErrorCode::CacheOpenFailed,
                            QStringLiteral("start translation cache migration"));
        }
        const QStringList statements{
            QStringLiteral("CREATE TABLE IF NOT EXISTS schema_migrations ("
                           "version INTEGER PRIMARY KEY, name TEXT NOT NULL, applied_at INTEGER "
                           "NOT NULL) STRICT"),
            QStringLiteral("CREATE TABLE IF NOT EXISTS translations ("
                           "provider TEXT NOT NULL, source_language TEXT NOT NULL, "
                           "target_language TEXT NOT NULL, backend_revision TEXT NOT NULL, "
                           "input_hash TEXT NOT NULL, input_text TEXT NOT NULL, "
                           "translated_text TEXT NOT NULL, updated_at INTEGER NOT NULL, "
                           "PRIMARY KEY (provider, source_language, target_language, "
                           "backend_revision, input_hash)) STRICT"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS translations_updated_at "
                           "ON translations(updated_at ASC)"),
            QStringLiteral("INSERT OR IGNORE INTO schema_migrations(version, name, applied_at) "
                           "VALUES (1, 'initial_translation_cache', unixepoch())"),
        };
        for (const auto& statement : statements)
        {
            if (!execute(m_database, statement))
            {
                m_database.rollback();
                return sqlError(TranslationErrorCode::CacheOpenFailed,
                                QStringLiteral("migrate the translation cache"));
            }
        }
        if (!m_database.commit())
        {
            m_database.rollback();
            return sqlError(TranslationErrorCode::CacheOpenFailed,
                            QStringLiteral("commit translation cache migration"));
        }
        return std::nullopt;
    }

    std::optional<TranslationError> TranslationCache::prune()
    {
        QSqlQuery count{m_database};
        if (!count.exec(QStringLiteral("SELECT COUNT(*) FROM translations")) || !count.next())
        {
            return sqlError(TranslationErrorCode::CacheWriteFailed,
                            QStringLiteral("count translation cache rows"));
        }
        const auto excess = count.value(0).toLongLong() - maximumRows;
        if (excess <= 0)
        {
            return std::nullopt;
        }

        QSqlQuery remove{m_database};
        remove.prepare(
            QStringLiteral("DELETE FROM translations WHERE rowid IN ("
                           "SELECT rowid FROM translations ORDER BY updated_at ASC LIMIT ?)"));
        remove.addBindValue(excess);
        if (!remove.exec())
        {
            return sqlError(TranslationErrorCode::CacheWriteFailed,
                            QStringLiteral("prune the translation cache"));
        }
        return std::nullopt;
    }

    QString TranslationCache::inputHash(const QStringView inputText)
    {
        return QString::fromLatin1(
            QCryptographicHash::hash(inputText.toUtf8(), QCryptographicHash::Sha256).toHex());
    }

    TranslationError TranslationCache::sqlError(const TranslationErrorCode code,
                                                const QStringView operation) const
    {
        return {
            .code = code,
            .message =
                i18n("Could not %1: %2", operation.toString(), m_database.lastError().text()),
        };
    }
} // namespace javelin::gui::translation
