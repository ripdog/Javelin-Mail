#include "jmap/cache/MailboxRepository.h"

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

        [[nodiscard]] std::string
        serializeRights(const javelin::jmap::domain::MailboxRights& rights)
        {
            glz::generic object;
            object["mayReadItems"] = rights.mayReadItems;
            object["mayAddItems"] = rights.mayAddItems;
            object["mayRemoveItems"] = rights.mayRemoveItems;
            object["maySetSeen"] = rights.maySetSeen;
            object["maySetKeywords"] = rights.maySetKeywords;
            object["mayCreateChild"] = rights.mayCreateChild;
            object["mayRename"] = rights.mayRename;
            object["mayDelete"] = rights.mayDelete;
            object["maySubmit"] = rights.maySubmit;

            std::string buffer;
            const auto writeError = glz::write_json(object, buffer);
            if (writeError)
            {
                return "{}";
            }

            return buffer;
        }

        [[nodiscard]] javelin::jmap::domain::MailboxRights deserializeRights(const QString& json)
        {
            std::string buffer = json.toStdString();
            glz::generic object;
            const auto readError =
                glz::read<glz::opts{.error_on_unknown_keys = false}>(object, buffer);
            if (readError || !object.is_object())
            {
                return {};
            }

            auto readBool = [&object](const char* key)
            {
                const auto* value = object[key].get_if<bool>();
                return value != nullptr ? *value : false;
            };

            return javelin::jmap::domain::MailboxRights{
                .mayReadItems = readBool("mayReadItems"),
                .mayAddItems = readBool("mayAddItems"),
                .mayRemoveItems = readBool("mayRemoveItems"),
                .maySetSeen = readBool("maySetSeen"),
                .maySetKeywords = readBool("maySetKeywords"),
                .mayCreateChild = readBool("mayCreateChild"),
                .mayRename = readBool("mayRename"),
                .mayDelete = readBool("mayDelete"),
                .maySubmit = readBool("maySubmit"),
            };
        }

        void bindMailbox(QSqlQuery& query, std::string_view accountId,
                         const javelin::jmap::domain::Mailbox& mailbox)
        {
            query.bindValue(QStringLiteral(":account_id"),
                            QString::fromStdString(std::string{accountId}));
            query.bindValue(QStringLiteral(":mailbox_id"), QString::fromStdString(mailbox.id));
            query.bindValue(QStringLiteral(":parent_mailbox_id"),
                            mailbox.parentId.has_value()
                                ? QVariant{QString::fromStdString(*mailbox.parentId)}
                                : QVariant{});
            query.bindValue(QStringLiteral(":name"), QString::fromStdString(mailbox.name));
            query.bindValue(QStringLiteral(":role"),
                            mailbox.role.has_value()
                                ? QVariant{QString::fromStdString(*mailbox.role)}
                                : QVariant{});
            query.bindValue(QStringLiteral(":sort_order"),
                            static_cast<qulonglong>(mailbox.sortOrder));
            query.bindValue(QStringLiteral(":total_emails"),
                            static_cast<qulonglong>(mailbox.totalEmails));
            query.bindValue(QStringLiteral(":unread_emails"),
                            static_cast<qulonglong>(mailbox.unreadEmails));
            query.bindValue(QStringLiteral(":total_threads"),
                            static_cast<qulonglong>(mailbox.totalThreads));
            query.bindValue(QStringLiteral(":unread_threads"),
                            static_cast<qulonglong>(mailbox.unreadThreads));
            query.bindValue(QStringLiteral(":is_subscribed"), mailbox.isSubscribed ? 1 : 0);
            query.bindValue(QStringLiteral(":rights_json"),
                            QString::fromStdString(serializeRights(mailbox.myRights)));
            query.bindValue(QStringLiteral(":state"), QVariant{});
        }

    } // namespace

    javelin::jmap::domain::MailboxRights deserializeMailboxRights(const QString& json)
    {
        return deserializeRights(json);
    }

    MailboxRepository::MailboxRepository(DatabaseConnection& connection) : m_connection(connection)
    {
    }

    std::optional<DatabaseError>
    MailboxRepository::replaceAll(const std::string_view accountId,
                                  const std::vector<javelin::jmap::domain::Mailbox>& mailboxes)
    {
        auto transactionResult =
            DatabaseTransaction::begin(m_connection, QStringLiteral("Replace mailboxes"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));
        if (const auto error = replaceAll(transaction, accountId, mailboxes))
            return error;
        return transaction.commit();
    }

    std::optional<DatabaseError>
    MailboxRepository::replaceAll(DatabaseTransaction& transaction,
                                  const std::string_view accountId,
                                  const std::vector<javelin::jmap::domain::Mailbox>& mailboxes)
    {
        if (const auto error = m_connection.validate())
            return error;
        if (!transaction.isActive() || &transaction.connection() != &m_connection)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Mailbox replacement requires a matching transaction"),
            };
        }

        QSqlQuery deleteQuery{m_connection.database()};
        deleteQuery.prepare(QStringLiteral("DELETE FROM mailboxes WHERE account_id = :account_id"));
        deleteQuery.bindValue(QStringLiteral(":account_id"),
                              QString::fromStdString(std::string{accountId}));
        if (!deleteQuery.exec())
            return makeQueryError(QStringLiteral("Delete account mailboxes"), deleteQuery);

        QSqlQuery insertQuery{m_connection.database()};
        insertQuery.prepare(QStringLiteral(
            "INSERT INTO mailboxes ("
            "account_id, mailbox_id, parent_mailbox_id, name, role, sort_order, total_emails, "
            "unread_emails, total_threads, unread_threads, is_subscribed, rights_json, state"
            ") VALUES ("
            ":account_id, :mailbox_id, :parent_mailbox_id, :name, :role, :sort_order, "
            ":total_emails, :unread_emails, :total_threads, :unread_threads, :is_subscribed, "
            ":rights_json, :state)"));

        for (const auto& mailbox : mailboxes)
        {
            bindMailbox(insertQuery, accountId, mailbox);
            if (!insertQuery.exec())
                return makeQueryError(QStringLiteral("Insert mailbox"), insertQuery);
        }

        return std::nullopt;
    }

    std::optional<DatabaseError>
    MailboxRepository::upsertMany(const std::string_view accountId,
                                  const std::vector<javelin::jmap::domain::Mailbox>& mailboxes)
    {
        if (const auto error = m_connection.validate())
        {
            return error;
        }

        if (mailboxes.empty())
        {
            return std::nullopt;
        }

        auto transactionResult =
            DatabaseTransaction::begin(m_connection, QStringLiteral("Upsert mailboxes"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));
        if (const auto error = upsertMany(transaction, accountId, mailboxes))
            return error;
        return transaction.commit();
    }

    std::optional<DatabaseError>
    MailboxRepository::upsertMany(DatabaseTransaction& transaction,
                                  const std::string_view accountId,
                                  const std::vector<javelin::jmap::domain::Mailbox>& mailboxes)
    {
        if (const auto error = m_connection.validate())
            return error;
        if (!transaction.isActive() || &transaction.connection() != &m_connection)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Mailbox upsert requires a matching transaction"),
            };
        }
        if (mailboxes.empty())
            return std::nullopt;

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "INSERT INTO mailboxes ("
            "account_id, mailbox_id, parent_mailbox_id, name, role, sort_order, total_emails, "
            "unread_emails, total_threads, unread_threads, is_subscribed, rights_json, state"
            ") VALUES ("
            ":account_id, :mailbox_id, :parent_mailbox_id, :name, :role, :sort_order, "
            ":total_emails, :unread_emails, :total_threads, :unread_threads, :is_subscribed, "
            ":rights_json, :state"
            ") ON CONFLICT(account_id, mailbox_id) DO UPDATE SET "
            "parent_mailbox_id = excluded.parent_mailbox_id, "
            "name = excluded.name, "
            "role = excluded.role, "
            "sort_order = excluded.sort_order, "
            "total_emails = excluded.total_emails, "
            "unread_emails = excluded.unread_emails, "
            "total_threads = excluded.total_threads, "
            "unread_threads = excluded.unread_threads, "
            "is_subscribed = excluded.is_subscribed, "
            "rights_json = excluded.rights_json, "
            "state = excluded.state"));

        for (const auto& mailbox : mailboxes)
        {
            bindMailbox(query, accountId, mailbox);
            if (!query.exec())
                return makeQueryError(QStringLiteral("Upsert mailbox"), query);
        }
        return std::nullopt;
    }

    std::optional<DatabaseError>
    MailboxRepository::removeMany(const std::string_view accountId,
                                  const std::span<const std::string> mailboxIds)
    {
        if (const auto error = m_connection.validate())
        {
            return error;
        }

        if (mailboxIds.empty())
        {
            return std::nullopt;
        }

        auto transactionResult =
            DatabaseTransaction::begin(m_connection, QStringLiteral("Delete mailboxes"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));
        if (const auto error = removeMany(transaction, accountId, mailboxIds))
            return error;
        return transaction.commit();
    }

    std::optional<DatabaseError>
    MailboxRepository::removeMany(DatabaseTransaction& transaction,
                                  const std::string_view accountId,
                                  const std::span<const std::string> mailboxIds)
    {
        if (const auto error = m_connection.validate())
            return error;
        if (!transaction.isActive() || &transaction.connection() != &m_connection)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Mailbox removal requires a matching transaction"),
            };
        }
        if (mailboxIds.empty())
            return std::nullopt;

        QSqlQuery projectionQuery{m_connection.database()};
        projectionQuery.prepare(QStringLiteral(
            "INSERT INTO mail_vault_projection_jobs(account_id,email_id,mailbox_id,content_hash,"
            "operation) SELECT mr.account_id,mr.email_id,mr.mailbox_id,r.content_hash,'unlink' "
            "FROM mail_vault_mailbox_refs mr JOIN mail_vault_email_refs r ON "
            "r.account_id=mr.account_id AND r.email_id=mr.email_id WHERE "
            "mr.account_id=:account_id AND mr.mailbox_id=:mailbox_id"));
        QSqlQuery ownershipQuery{m_connection.database()};
        ownershipQuery.prepare(
            QStringLiteral("DELETE FROM mail_vault_mailbox_refs WHERE account_id=:account_id AND "
                           "mailbox_id=:mailbox_id"));
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "DELETE FROM mailboxes WHERE account_id = :account_id AND mailbox_id = :mailbox_id"));
        for (const auto& mailboxId : mailboxIds)
        {
            projectionQuery.bindValue(QStringLiteral(":account_id"),
                                      QString::fromStdString(std::string{accountId}));
            projectionQuery.bindValue(QStringLiteral(":mailbox_id"),
                                      QString::fromStdString(mailboxId));
            if (!projectionQuery.exec())
            {
                return makeQueryError(QStringLiteral("Queue deleted mailbox body projections"),
                                      projectionQuery);
            }
            projectionQuery.finish();
            ownershipQuery.bindValue(QStringLiteral(":account_id"),
                                     QString::fromStdString(std::string{accountId}));
            ownershipQuery.bindValue(QStringLiteral(":mailbox_id"),
                                     QString::fromStdString(mailboxId));
            if (!ownershipQuery.exec())
            {
                return makeQueryError(QStringLiteral("Delete mailbox body ownership"),
                                      ownershipQuery);
            }
            ownershipQuery.finish();
            query.bindValue(QStringLiteral(":account_id"),
                            QString::fromStdString(std::string{accountId}));
            query.bindValue(QStringLiteral(":mailbox_id"), QString::fromStdString(mailboxId));
            if (!query.exec())
                return makeQueryError(QStringLiteral("Delete mailbox"), query);
        }
        return std::nullopt;
    }

    std::variant<std::optional<javelin::jmap::domain::Mailbox>, DatabaseError>
    MailboxRepository::find(const std::string_view accountId,
                            const std::string_view mailboxId) const
    {
        if (const auto error = m_connection.validate())
            return *error;

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT mailbox_id,name,parent_mailbox_id,role,sort_order,total_emails,unread_emails,"
            "total_threads,unread_threads,is_subscribed,rights_json FROM mailboxes "
            "WHERE account_id=:account_id AND mailbox_id=:mailbox_id"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":mailbox_id"),
                        QString::fromStdString(std::string{mailboxId}));
        if (!query.exec())
            return makeQueryError(QStringLiteral("Read mailbox"), query);
        if (!query.next())
            return std::optional<javelin::jmap::domain::Mailbox>{};

        return std::optional<javelin::jmap::domain::Mailbox>{javelin::jmap::domain::Mailbox{
            .id = query.value(0).toString().toStdString(),
            .name = query.value(1).toString().toStdString(),
            .parentId = query.value(2).isNull()
                            ? std::nullopt
                            : std::optional{query.value(2).toString().toStdString()},
            .role = query.value(3).isNull()
                        ? std::nullopt
                        : std::optional{query.value(3).toString().toStdString()},
            .sortOrder = query.value(4).toULongLong(),
            .totalEmails = query.value(5).toULongLong(),
            .unreadEmails = query.value(6).toULongLong(),
            .totalThreads = query.value(7).toULongLong(),
            .unreadThreads = query.value(8).toULongLong(),
            .isSubscribed = query.value(9).toInt() != 0,
            .myRights = deserializeMailboxRights(query.value(10).toString()),
        }};
    }

    std::optional<DatabaseError> MailboxRepository::setSubscribed(DatabaseTransaction& transaction,
                                                                  const std::string_view accountId,
                                                                  const std::string_view mailboxId,
                                                                  const bool subscribed)
    {
        if (const auto error = m_connection.validate())
            return error;
        if (!transaction.isActive() || &transaction.connection() != &m_connection)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral(
                    "Mailbox subscription projection requires a matching transaction"),
            };
        }
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral("UPDATE mailboxes SET is_subscribed=:subscribed "
                                     "WHERE account_id=:account_id AND mailbox_id=:mailbox_id"));
        query.bindValue(QStringLiteral(":subscribed"), subscribed ? 1 : 0);
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":mailbox_id"),
                        QString::fromStdString(std::string{mailboxId}));
        if (!query.exec())
            return makeQueryError(QStringLiteral("Project mailbox subscription"), query);
        if (query.numRowsAffected() != 1)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Mailbox subscription projection target is missing."),
            };
        }
        return std::nullopt;
    }

    std::optional<DatabaseError> MailboxRepository::projectPendingCreate(
        DatabaseTransaction& transaction, const std::string_view accountId,
        const std::string_view creationId, const std::string_view mutationId,
        const std::string_view name, const std::optional<std::string_view> parentId,
        const std::uint64_t sortOrder, const bool subscribed)
    {
        if (const auto error = m_connection.validate())
            return error;
        if (!transaction.isActive() || &transaction.connection() != &m_connection)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message =
                    QStringLiteral("Mailbox create projection requires a matching transaction"),
            };
        }
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "INSERT INTO mailbox_create_projections(account_id,creation_id,mutation_id,name,"
            "parent_mailbox_id,sort_order,is_subscribed) VALUES(:account,:creation,:mutation,:name,"
            ":parent,:sort_order,:subscribed)"));
        query.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":creation"),
                        QString::fromStdString(std::string{creationId}));
        query.bindValue(QStringLiteral(":mutation"),
                        QString::fromStdString(std::string{mutationId}));
        query.bindValue(QStringLiteral(":name"), QString::fromStdString(std::string{name}));
        query.bindValue(QStringLiteral(":parent"),
                        parentId.has_value()
                            ? QVariant{QString::fromStdString(std::string{*parentId})}
                            : QVariant{});
        query.bindValue(QStringLiteral(":sort_order"), static_cast<qulonglong>(sortOrder));
        query.bindValue(QStringLiteral(":subscribed"), subscribed ? 1 : 0);
        if (!query.exec())
            return makeQueryError(QStringLiteral("Project mailbox creation"), query);
        return std::nullopt;
    }

    std::optional<DatabaseError>
    MailboxRepository::removePendingCreate(DatabaseTransaction& transaction,
                                           const std::string_view accountId,
                                           const std::string_view creationId)
    {
        if (const auto error = m_connection.validate())
            return error;
        if (!transaction.isActive() || &transaction.connection() != &m_connection)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral(
                    "Mailbox create projection removal requires a matching transaction"),
            };
        }
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral("DELETE FROM mailbox_create_projections WHERE "
                                     "account_id=:account AND creation_id=:creation"));
        query.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":creation"),
                        QString::fromStdString(std::string{creationId}));
        if (!query.exec())
            return makeQueryError(QStringLiteral("Remove mailbox create projection"), query);
        return std::nullopt;
    }

    std::variant<std::vector<javelin::jmap::domain::Mailbox>, DatabaseError>
    MailboxRepository::listByParent(const std::string_view accountId,
                                    const std::optional<std::string_view> parentId) const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        QSqlQuery query{m_connection.database()};
        if (parentId.has_value())
        {
            query.prepare(QStringLiteral(
                "SELECT mailbox_id, name, parent_mailbox_id, role, sort_order, total_emails, "
                "unread_emails, total_threads, unread_threads, is_subscribed, rights_json "
                "FROM mailboxes "
                "WHERE account_id = :account_id AND parent_mailbox_id = :parent_mailbox_id "
                "ORDER BY sort_order, mailbox_id"));
            query.bindValue(QStringLiteral(":parent_mailbox_id"),
                            QString::fromStdString(std::string{*parentId}));
        }
        else
        {
            query.prepare(QStringLiteral(
                "SELECT mailbox_id, name, parent_mailbox_id, role, sort_order, total_emails, "
                "unread_emails, total_threads, unread_threads, is_subscribed, rights_json "
                "FROM mailboxes "
                "WHERE account_id = :account_id AND parent_mailbox_id IS NULL "
                "ORDER BY sort_order, mailbox_id"));
        }

        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        if (!query.exec())
        {
            return makeQueryError(QStringLiteral("Read mailboxes"), query);
        }

        std::vector<javelin::jmap::domain::Mailbox> mailboxes;
        while (query.next())
        {
            mailboxes.push_back(javelin::jmap::domain::Mailbox{
                .id = query.value(0).toString().toStdString(),
                .name = query.value(1).toString().toStdString(),
                .parentId = query.value(2).isNull()
                                ? std::nullopt
                                : std::optional{query.value(2).toString().toStdString()},
                .role = query.value(3).isNull()
                            ? std::nullopt
                            : std::optional{query.value(3).toString().toStdString()},
                .sortOrder = query.value(4).toULongLong(),
                .totalEmails = query.value(5).toULongLong(),
                .unreadEmails = query.value(6).toULongLong(),
                .totalThreads = query.value(7).toULongLong(),
                .unreadThreads = query.value(8).toULongLong(),
                .isSubscribed = query.value(9).toInt() != 0,
                .myRights = deserializeMailboxRights(query.value(10).toString()),
            });
        }

        return mailboxes;
    }

} // namespace javelin::jmap::cache
