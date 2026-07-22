#include "jmap/cache/MailSearchIndex.h"

#include "jmap/cache/MailVault.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace javelin::jmap::cache
{
    namespace
    {
        [[nodiscard]] DatabaseError error(const QString& operation, const QString& detail)
        {
            return {.code = DatabaseErrorCode::QueryFailed,
                    .message = operation + QStringLiteral(": ") + detail};
        }

        class IndexConnection final
        {
          public:
            explicit IndexConnection(const QString& path)
                : m_name(QStringLiteral("mail-search-%1")
                             .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
            {
                QDir{}.mkpath(QFileInfo(path).absolutePath());
                m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_name);
                m_database.setDatabaseName(path);
                if (!m_database.open())
                {
                    m_error = error(QStringLiteral("Open mail search index"),
                                    m_database.lastError().text());
                    return;
                }
                QSqlQuery query{m_database};
                const QStringList statements{
                    QStringLiteral("PRAGMA journal_mode=WAL"),
                    QStringLiteral("PRAGMA synchronous=NORMAL"),
                    QStringLiteral("PRAGMA busy_timeout=5000"),
                    QStringLiteral(
                        "CREATE TABLE IF NOT EXISTS search_documents(rowid INTEGER PRIMARY KEY,"
                        "email_id TEXT NOT NULL UNIQUE,source_hash TEXT NOT NULL,index_version "
                        "INTEGER NOT NULL DEFAULT 1) STRICT"),
                    QStringLiteral(
                        "CREATE VIRTUAL TABLE IF NOT EXISTS email_search_fts USING fts5(subject,"
                        "body,content='',contentless_delete=1,tokenize='unicode61')"),
                };
                for (const auto& statement : statements)
                {
                    if (!query.exec(statement))
                    {
                        m_error = error(QStringLiteral("Initialize mail search index"),
                                        query.lastError().text());
                        return;
                    }
                }
            }

            ~IndexConnection()
            {
                const QString name = m_name;
                m_database.close();
                m_database = {};
                QSqlDatabase::removeDatabase(name);
            }

            [[nodiscard]] QSqlDatabase& database()
            {
                return m_database;
            }
            [[nodiscard]] const std::optional<DatabaseError>& failure() const
            {
                return m_error;
            }

          private:
            QString m_name;
            QSqlDatabase m_database;
            std::optional<DatabaseError> m_error;
        };

        [[nodiscard]] QString indexPath(const DatabaseConnection& connection,
                                        const std::string_view accountId)
        {
            return MailVault::forDatabase(connection).searchIndexPath(accountId);
        }
    } // namespace

    MailSearchIndex::MailSearchIndex(const DatabaseConnection& cacheConnection)
        : m_cacheConnection(cacheConnection)
    {
    }

    std::optional<DatabaseError> MailSearchIndex::upsert(const std::string_view accountId,
                                                         const SearchIndexDocument& document) const
    {
        const QString path = indexPath(m_cacheConnection, accountId);
        const DatabaseWriteScope writeScope{path};
        IndexConnection connection{path};
        if (connection.failure())
            return connection.failure();
        auto& database = connection.database();
        if (!database.transaction())
            return error(QStringLiteral("Begin mail search update"), database.lastError().text());

        QSqlQuery documentQuery{database};
        documentQuery.prepare(QStringLiteral(
            "INSERT INTO search_documents(email_id,source_hash,index_version) VALUES(:email,"
            ":hash,1) ON CONFLICT(email_id) DO UPDATE SET source_hash=excluded.source_hash,"
            "index_version=excluded.index_version RETURNING rowid"));
        documentQuery.bindValue(QStringLiteral(":email"), QString::fromStdString(document.emailId));
        documentQuery.bindValue(QStringLiteral(":hash"),
                                QString::fromStdString(document.sourceHash));
        if (!documentQuery.exec() || !documentQuery.next())
        {
            database.rollback();
            return error(QStringLiteral("Record indexed mail document"),
                         documentQuery.lastError().text());
        }
        const qlonglong rowId = documentQuery.value(0).toLongLong();
        documentQuery.finish();

        QSqlQuery deleteIndexQuery{database};
        deleteIndexQuery.prepare(QStringLiteral("DELETE FROM email_search_fts WHERE rowid=:rowid"));
        deleteIndexQuery.bindValue(QStringLiteral(":rowid"), rowId);
        if (!deleteIndexQuery.exec())
        {
            database.rollback();
            return error(QStringLiteral("Replace indexed mail document"),
                         deleteIndexQuery.lastError().text());
        }

        QSqlQuery indexQuery{database};
        indexQuery.prepare(QStringLiteral(
            "INSERT INTO email_search_fts(rowid,subject,body) VALUES(:rowid,:subject,:body)"));
        indexQuery.bindValue(QStringLiteral(":rowid"), rowId);
        indexQuery.bindValue(QStringLiteral(":subject"), document.subject);
        indexQuery.bindValue(QStringLiteral(":body"), document.body);
        if (!indexQuery.exec())
        {
            database.rollback();
            return error(QStringLiteral("Index mail document"), indexQuery.lastError().text());
        }
        if (!database.commit())
            return error(QStringLiteral("Commit mail search update"), database.lastError().text());
        return std::nullopt;
    }

    std::optional<DatabaseError> MailSearchIndex::remove(const std::string_view accountId,
                                                         const std::string_view emailId) const
    {
        const QString path = indexPath(m_cacheConnection, accountId);
        const DatabaseWriteScope writeScope{path};
        IndexConnection connection{path};
        if (connection.failure())
            return connection.failure();
        QSqlQuery rowQuery{connection.database()};
        rowQuery.prepare(
            QStringLiteral("SELECT rowid FROM search_documents WHERE email_id=:email"));
        rowQuery.bindValue(QStringLiteral(":email"), QString::fromStdString(std::string{emailId}));
        if (!rowQuery.exec())
            return error(QStringLiteral("Find mail search document"), rowQuery.lastError().text());
        if (!rowQuery.next())
            return std::nullopt;
        const qlonglong rowId = rowQuery.value(0).toLongLong();
        rowQuery.finish();
        QSqlQuery indexQuery{connection.database()};
        indexQuery.prepare(QStringLiteral("DELETE FROM email_search_fts WHERE rowid=:rowid"));
        indexQuery.bindValue(QStringLiteral(":rowid"), rowId);
        if (!indexQuery.exec())
            return error(QStringLiteral("Remove indexed mail text"), indexQuery.lastError().text());
        QSqlQuery documentQuery{connection.database()};
        documentQuery.prepare(QStringLiteral("DELETE FROM search_documents WHERE email_id=:email"));
        documentQuery.bindValue(QStringLiteral(":email"),
                                QString::fromStdString(std::string{emailId}));
        if (!documentQuery.exec())
            return error(QStringLiteral("Remove mail search document"),
                         documentQuery.lastError().text());
        return std::nullopt;
    }

    std::variant<std::vector<std::string>, DatabaseError>
    MailSearchIndex::search(const std::string_view accountId, const std::string_view text,
                            const std::size_t limit) const
    {
        IndexConnection connection{indexPath(m_cacheConnection, accountId)};
        if (connection.failure())
            return *connection.failure();
        QString match = QStringLiteral("\"");
        match += QString::fromStdString(std::string{text})
                     .replace(QLatin1String("\""), QStringLiteral("\"\""));
        match += QLatin1Char('"');
        QSqlQuery query{connection.database()};
        query.prepare(QStringLiteral(
            "SELECT d.email_id FROM email_search_fts f JOIN search_documents d ON d.rowid=f.rowid "
            "WHERE email_search_fts MATCH :match ORDER BY bm25(email_search_fts) LIMIT :limit"));
        query.bindValue(QStringLiteral(":match"), match);
        query.bindValue(QStringLiteral(":limit"), static_cast<qulonglong>(limit));
        if (!query.exec())
            return error(QStringLiteral("Search local mail index"), query.lastError().text());
        std::vector<std::string> ids;
        while (query.next())
            ids.push_back(query.value(0).toString().toStdString());
        return ids;
    }

    std::optional<DatabaseError> MailSearchIndex::rebuild(const std::string_view accountId) const
    {
        const QString path = indexPath(m_cacheConnection, accountId);
        if (QFileInfo::exists(path) && !QFile::remove(path))
            return error(QStringLiteral("Remove old mail search index"), path);
        return std::nullopt;
    }
} // namespace javelin::jmap::cache
