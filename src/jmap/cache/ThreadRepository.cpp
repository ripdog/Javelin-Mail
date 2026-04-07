#include "jmap/cache/ThreadRepository.h"

#include <glaze/glaze.hpp>

#include <QSqlError>
#include <QSqlQuery>

#include <string>

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

        [[nodiscard]] std::string serializeEmailIds(const std::vector<std::string>& emailIds)
        {
            std::string buffer;
            const auto writeError = glz::write_json(emailIds, buffer);
            if (writeError)
            {
                return "[]";
            }

            return buffer;
        }

        [[nodiscard]] std::vector<std::string> deserializeEmailIds(const QString& json)
        {
            std::string buffer = json.toStdString();
            std::vector<std::string> emailIds;
            const auto readError =
                glz::read<glz::opts{.error_on_unknown_keys = false}>(emailIds, buffer);
            if (readError)
            {
                return {};
            }

            return emailIds;
        }

    } // namespace

    ThreadRepository::ThreadRepository(DatabaseConnection& connection) : m_connection(connection)
    {
    }

    std::optional<DatabaseError>
    ThreadRepository::replaceAll(const std::string_view accountId,
                                 const std::vector<javelin::jmap::domain::Thread>& threads)
    {
        if (const auto error = m_connection.validate())
        {
            return error;
        }

        QSqlDatabase& database = m_connection.database();
        if (!database.transaction())
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Begin thread replacement transaction: ") + database.lastError().text(),
            };
        }

        QSqlQuery deleteQuery{database};
        deleteQuery.prepare(QStringLiteral("DELETE FROM threads WHERE account_id = :account_id"));
        deleteQuery.bindValue(QStringLiteral(":account_id"), QString::fromStdString(std::string{accountId}));
        if (!deleteQuery.exec())
        {
            database.rollback();
            return makeQueryError(QStringLiteral("Delete account threads"), deleteQuery);
        }

        QSqlQuery insertQuery{database};
        insertQuery.prepare(QStringLiteral("INSERT INTO threads (account_id, thread_id, email_ids_json, state) "
                            "VALUES (:account_id, :thread_id, :email_ids_json, :state)"));
        for (const auto& thread : threads)
        {
            insertQuery.bindValue(QStringLiteral(":account_id"), QString::fromStdString(std::string{accountId}));
            insertQuery.bindValue(QStringLiteral(":thread_id"), QString::fromStdString(thread.id));
            insertQuery.bindValue(QStringLiteral(":email_ids_json"),
                                  QString::fromStdString(serializeEmailIds(thread.emailIds)));
            insertQuery.bindValue(QStringLiteral(":state"), QVariant{});
            if (!insertQuery.exec())
            {
                database.rollback();
                return makeQueryError(QStringLiteral("Insert thread"), insertQuery);
            }
        }

        if (!database.commit())
        {
            database.rollback();
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Commit thread replacement transaction: ") + database.lastError().text(),
            };
        }

        return std::nullopt;
    }

    std::variant<std::optional<javelin::jmap::domain::Thread>, DatabaseError>
    ThreadRepository::find(const std::string_view accountId, const std::string_view threadId) const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral("SELECT email_ids_json "
                      "FROM threads "
                      "WHERE account_id = :account_id AND thread_id = :thread_id"));
        query.bindValue(QStringLiteral(":account_id"), QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":thread_id"), QString::fromStdString(std::string{threadId}));
        if (!query.exec())
        {
            return makeQueryError(QStringLiteral("Read thread"), query);
        }

        if (!query.next())
        {
            return std::optional<javelin::jmap::domain::Thread>{std::nullopt};
        }

        return std::optional<javelin::jmap::domain::Thread>{javelin::jmap::domain::Thread{
            .id = std::string{threadId},
            .emailIds = deserializeEmailIds(query.value(0).toString()),
        }};
    }

} // namespace javelin::jmap::cache
