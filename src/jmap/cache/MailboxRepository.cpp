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
                .message = operation + ": " + query.lastError().text(),
            };
        }

        [[nodiscard]] std::string
        serializeRights(const javelin::jmap::domain::MailboxRights& rights)
        {
            glz::json_t object;
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
            glz::json_t object;
            const auto readError = glz::read_json(object, buffer);
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
            query.bindValue(":account_id", QString::fromStdString(std::string{accountId}));
            query.bindValue(":mailbox_id", QString::fromStdString(mailbox.id));
            query.bindValue(":parent_mailbox_id",
                            mailbox.parentId.has_value()
                                ? QVariant{QString::fromStdString(*mailbox.parentId)}
                                : QVariant{});
            query.bindValue(":name", QString::fromStdString(mailbox.name));
            query.bindValue(":role", mailbox.role.has_value()
                                         ? QVariant{QString::fromStdString(*mailbox.role)}
                                         : QVariant{});
            query.bindValue(":sort_order", static_cast<qulonglong>(mailbox.sortOrder));
            query.bindValue(":total_emails", static_cast<qulonglong>(mailbox.totalEmails));
            query.bindValue(":unread_emails", static_cast<qulonglong>(mailbox.unreadEmails));
            query.bindValue(":total_threads", static_cast<qulonglong>(mailbox.totalThreads));
            query.bindValue(":unread_threads", static_cast<qulonglong>(mailbox.unreadThreads));
            query.bindValue(":is_subscribed", mailbox.isSubscribed ? 1 : 0);
            query.bindValue(":rights_json",
                            QString::fromStdString(serializeRights(mailbox.myRights)));
            query.bindValue(":state", QVariant{});
        }

    } // namespace

    MailboxRepository::MailboxRepository(DatabaseConnection& connection) : m_connection(connection)
    {
    }

    std::optional<DatabaseError>
    MailboxRepository::replaceAll(const std::string_view accountId,
                                  const std::vector<javelin::jmap::domain::Mailbox>& mailboxes)
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
                .message = "Begin mailbox replacement transaction: " + database.lastError().text(),
            };
        }

        QSqlQuery deleteQuery{database};
        deleteQuery.prepare("DELETE FROM mailboxes WHERE account_id = :account_id");
        deleteQuery.bindValue(":account_id", QString::fromStdString(std::string{accountId}));
        if (!deleteQuery.exec())
        {
            database.rollback();
            return makeQueryError("Delete account mailboxes", deleteQuery);
        }

        QSqlQuery insertQuery{database};
        insertQuery.prepare(
            "INSERT INTO mailboxes ("
            "account_id, mailbox_id, parent_mailbox_id, name, role, sort_order, total_emails, "
            "unread_emails, total_threads, unread_threads, is_subscribed, rights_json, state"
            ") VALUES ("
            ":account_id, :mailbox_id, :parent_mailbox_id, :name, :role, :sort_order, "
            ":total_emails, :unread_emails, :total_threads, :unread_threads, :is_subscribed, "
            ":rights_json, :state)");

        for (const auto& mailbox : mailboxes)
        {
            bindMailbox(insertQuery, accountId, mailbox);
            if (!insertQuery.exec())
            {
                database.rollback();
                return makeQueryError("Insert mailbox", insertQuery);
            }
        }

        if (!database.commit())
        {
            database.rollback();
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = "Commit mailbox replacement transaction: " + database.lastError().text(),
            };
        }

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
            query.prepare(
                "SELECT mailbox_id, name, parent_mailbox_id, role, sort_order, total_emails, "
                "unread_emails, total_threads, unread_threads, is_subscribed, rights_json "
                "FROM mailboxes "
                "WHERE account_id = :account_id AND parent_mailbox_id = :parent_mailbox_id "
                "ORDER BY sort_order, mailbox_id");
            query.bindValue(":parent_mailbox_id", QString::fromStdString(std::string{*parentId}));
        }
        else
        {
            query.prepare(
                "SELECT mailbox_id, name, parent_mailbox_id, role, sort_order, total_emails, "
                "unread_emails, total_threads, unread_threads, is_subscribed, rights_json "
                "FROM mailboxes "
                "WHERE account_id = :account_id AND parent_mailbox_id IS NULL "
                "ORDER BY sort_order, mailbox_id");
        }

        query.bindValue(":account_id", QString::fromStdString(std::string{accountId}));
        if (!query.exec())
        {
            return makeQueryError("Read mailboxes", query);
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
                .myRights = deserializeRights(query.value(10).toString()),
            });
        }

        return mailboxes;
    }

} // namespace javelin::jmap::cache
