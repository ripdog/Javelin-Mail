#include "jmap/cache/ThreadRepository.h"

#include <QSqlError>
#include <QSqlQuery>

#include <utility>

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

        [[nodiscard]] ThreadMembershipFreshness freshnessFromValue(const QString& value)
        {
            return value == QStringLiteral("current") ? ThreadMembershipFreshness::Current
                                                      : ThreadMembershipFreshness::Stale;
        }

        void bindState(QSqlQuery& query, const std::optional<std::string_view> state)
        {
            query.bindValue(QStringLiteral(":state"),
                            state.has_value()
                                ? QVariant{QString::fromStdString(std::string{*state})}
                                : QVariant{});
        }

    } // namespace

    ThreadRepository::ThreadRepository(DatabaseConnection& connection) : m_connection(connection)
    {
    }

    std::optional<DatabaseError>
    ThreadRepository::replaceAll(const std::string_view accountId,
                                 const std::vector<javelin::jmap::domain::Thread>& threads,
                                 const std::optional<std::string_view> state)
    {
        auto transactionResult = DatabaseTransaction::begin(
            m_connection, QStringLiteral("Begin thread replacement transaction"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));

        QSqlQuery deleteQuery{m_connection.database()};
        deleteQuery.prepare(QStringLiteral("DELETE FROM threads WHERE account_id=:account_id"));
        deleteQuery.bindValue(QStringLiteral(":account_id"),
                              QString::fromStdString(std::string{accountId}));
        if (!deleteQuery.exec())
            return makeQueryError(QStringLiteral("Delete account threads"), deleteQuery);
        deleteQuery.finish();

        if (const auto error = upsertMany(transaction, accountId, threads, state))
            return error;
        return transaction.commit();
    }

    std::optional<DatabaseError>
    ThreadRepository::upsertMany(const std::string_view accountId,
                                 const std::vector<javelin::jmap::domain::Thread>& threads,
                                 const std::optional<std::string_view> state)
    {
        if (threads.empty())
            return std::nullopt;
        auto transactionResult = DatabaseTransaction::begin(
            m_connection, QStringLiteral("Begin thread upsert transaction"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));
        if (const auto error = upsertMany(transaction, accountId, threads, state))
            return error;
        return transaction.commit();
    }

    std::optional<DatabaseError>
    ThreadRepository::upsertMany(DatabaseTransaction& transaction, const std::string_view accountId,
                                 const std::vector<javelin::jmap::domain::Thread>& threads,
                                 const std::optional<std::string_view> state)
    {
        if (const auto error = m_connection.validate())
            return error;
        if (!transaction.isActive() || &transaction.connection() != &m_connection)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Thread upsert requires an active matching transaction"),
            };
        }
        if (threads.empty())
            return std::nullopt;

        QSqlQuery threadQuery{m_connection.database()};
        threadQuery.prepare(QStringLiteral(
            "INSERT INTO threads(account_id,thread_id,state,membership_freshness,member_count) "
            "VALUES(:account_id,:thread_id,:state,'current',:member_count) "
            "ON CONFLICT(account_id,thread_id) DO UPDATE SET state=excluded.state,"
            "membership_freshness='current',member_count=excluded.member_count"));
        QSqlQuery deleteMembers{m_connection.database()};
        deleteMembers.prepare(
            QStringLiteral("DELETE FROM thread_email_members WHERE account_id=:account_id AND "
                           "thread_id=:thread_id"));
        QSqlQuery insertMember{m_connection.database()};
        insertMember.prepare(QStringLiteral(
            "INSERT INTO thread_email_members(account_id,thread_id,position,email_id) "
            "VALUES(:account_id,:thread_id,:position,:email_id)"));

        const auto account = QString::fromStdString(std::string{accountId});
        for (const auto& thread : threads)
        {
            const auto threadId = QString::fromStdString(thread.id);
            threadQuery.bindValue(QStringLiteral(":account_id"), account);
            threadQuery.bindValue(QStringLiteral(":thread_id"), threadId);
            bindState(threadQuery, state);
            threadQuery.bindValue(QStringLiteral(":member_count"),
                                  static_cast<qulonglong>(thread.emailIds.size()));
            if (!threadQuery.exec())
                return makeQueryError(QStringLiteral("Upsert thread membership metadata"),
                                      threadQuery);
            threadQuery.finish();

            deleteMembers.bindValue(QStringLiteral(":account_id"), account);
            deleteMembers.bindValue(QStringLiteral(":thread_id"), threadId);
            if (!deleteMembers.exec())
                return makeQueryError(QStringLiteral("Delete old thread membership"),
                                      deleteMembers);
            deleteMembers.finish();

            for (std::size_t position = 0; position < thread.emailIds.size(); ++position)
            {
                insertMember.bindValue(QStringLiteral(":account_id"), account);
                insertMember.bindValue(QStringLiteral(":thread_id"), threadId);
                insertMember.bindValue(QStringLiteral(":position"),
                                       static_cast<qulonglong>(position));
                insertMember.bindValue(QStringLiteral(":email_id"),
                                       QString::fromStdString(thread.emailIds[position]));
                if (!insertMember.exec())
                    return makeQueryError(QStringLiteral("Insert thread member"), insertMember);
                insertMember.finish();
            }
        }
        return std::nullopt;
    }

    std::optional<DatabaseError>
    ThreadRepository::markStale(const std::string_view accountId,
                                const std::span<const std::string> threadIds)
    {
        if (threadIds.empty())
            return std::nullopt;
        auto transactionResult = DatabaseTransaction::begin(
            m_connection, QStringLiteral("Begin stale thread membership transaction"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));
        if (const auto error = markStale(transaction, accountId, threadIds))
            return error;
        return transaction.commit();
    }

    std::optional<DatabaseError>
    ThreadRepository::markStale(DatabaseTransaction& transaction, const std::string_view accountId,
                                const std::span<const std::string> threadIds)
    {
        if (const auto error = m_connection.validate())
            return error;
        if (!transaction.isActive() || &transaction.connection() != &m_connection)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message =
                    QStringLiteral("Marking thread stale requires an active matching transaction"),
            };
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "UPDATE threads SET membership_freshness='stale' WHERE account_id=:account_id AND "
            "thread_id=:thread_id"));
        const auto account = QString::fromStdString(std::string{accountId});
        for (const auto& threadId : threadIds)
        {
            query.bindValue(QStringLiteral(":account_id"), account);
            query.bindValue(QStringLiteral(":thread_id"), QString::fromStdString(threadId));
            if (!query.exec())
                return makeQueryError(QStringLiteral("Mark thread membership stale"), query);
            query.finish();
        }
        return std::nullopt;
    }

    std::variant<std::optional<ThreadMembershipRecord>, DatabaseError>
    ThreadRepository::findMembership(const std::string_view accountId,
                                     const std::string_view threadId) const
    {
        if (const auto error = m_connection.validate())
            return *error;

        const auto account = QString::fromStdString(std::string{accountId});
        const auto id = QString::fromStdString(std::string{threadId});
        QSqlQuery threadQuery{m_connection.database()};
        threadQuery.prepare(
            QStringLiteral("SELECT membership_freshness,member_count,state FROM threads WHERE "
                           "account_id=:account_id AND thread_id=:thread_id"));
        threadQuery.bindValue(QStringLiteral(":account_id"), account);
        threadQuery.bindValue(QStringLiteral(":thread_id"), id);
        if (!threadQuery.exec())
            return makeQueryError(QStringLiteral("Read thread membership metadata"), threadQuery);
        if (!threadQuery.next())
            return std::optional<ThreadMembershipRecord>{std::nullopt};

        ThreadMembershipRecord record{
            .thread = {.id = std::string{threadId}, .emailIds = {}},
            .freshness = freshnessFromValue(threadQuery.value(0).toString()),
            .globalMemberCount = threadQuery.value(1).toULongLong(),
            .state = threadQuery.value(2).isNull()
                         ? std::nullopt
                         : std::optional{threadQuery.value(2).toString().toStdString()},
        };
        threadQuery.finish();

        QSqlQuery memberQuery{m_connection.database()};
        memberQuery.prepare(QStringLiteral(
            "SELECT email_id FROM thread_email_members WHERE account_id=:account_id AND "
            "thread_id=:thread_id ORDER BY position"));
        memberQuery.bindValue(QStringLiteral(":account_id"), account);
        memberQuery.bindValue(QStringLiteral(":thread_id"), id);
        if (!memberQuery.exec())
            return makeQueryError(QStringLiteral("Read ordered thread membership"), memberQuery);
        record.thread.emailIds.reserve(record.globalMemberCount);
        while (memberQuery.next())
            record.thread.emailIds.push_back(memberQuery.value(0).toString().toStdString());
        return std::optional<ThreadMembershipRecord>{std::move(record)};
    }

    std::variant<std::optional<javelin::jmap::domain::Thread>, DatabaseError>
    ThreadRepository::find(const std::string_view accountId, const std::string_view threadId) const
    {
        const auto result = findMembership(accountId, threadId);
        if (const auto* error = std::get_if<DatabaseError>(&result))
            return *error;
        const auto& membership = std::get<std::optional<ThreadMembershipRecord>>(result);
        if (!membership.has_value())
            return std::optional<javelin::jmap::domain::Thread>{std::nullopt};
        return std::optional<javelin::jmap::domain::Thread>{membership->thread};
    }

    std::variant<std::optional<std::string>, DatabaseError>
    ThreadRepository::findThreadIdByEmailId(const std::string_view accountId,
                                            const std::string_view emailId) const
    {
        if (const auto error = m_connection.validate())
            return *error;
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT m.thread_id FROM thread_email_members m INNER JOIN threads t ON "
            "t.account_id=m.account_id AND t.thread_id=m.thread_id WHERE "
            "m.account_id=:account_id AND m.email_id=:email_id ORDER BY "
            "CASE t.membership_freshness WHEN 'current' THEN 0 ELSE 1 END,m.thread_id LIMIT 1"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":email_id"), QString::fromStdString(std::string{emailId}));
        if (!query.exec())
            return makeQueryError(QStringLiteral("Resolve thread by member email"), query);
        if (!query.next())
            return std::optional<std::string>{std::nullopt};
        return std::optional<std::string>{query.value(0).toString().toStdString()};
    }

    std::variant<std::vector<std::string>, DatabaseError>
    ThreadRepository::missingEmailIds(const std::string_view accountId,
                                      const std::string_view threadId,
                                      const std::size_t limit) const
    {
        if (const auto error = m_connection.validate())
            return *error;
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT m.email_id FROM thread_email_members m LEFT JOIN emails e ON "
            "e.account_id=m.account_id AND e.email_id=m.email_id AND e.thread_id=m.thread_id WHERE "
            "m.account_id=:account_id AND m.thread_id=:thread_id AND e.email_id IS NULL ORDER BY "
            "m.position LIMIT :limit"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":thread_id"),
                        QString::fromStdString(std::string{threadId}));
        query.bindValue(QStringLiteral(":limit"), static_cast<qulonglong>(limit));
        if (!query.exec())
            return makeQueryError(QStringLiteral("Read missing thread email ids"), query);
        std::vector<std::string> ids;
        while (query.next())
            ids.push_back(query.value(0).toString().toStdString());
        return ids;
    }

    std::variant<std::optional<ThreadCoverage>, DatabaseError>
    ThreadRepository::coverage(const std::string_view accountId,
                               const std::string_view threadId) const
    {
        if (const auto error = m_connection.validate())
            return *error;
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT t.membership_freshness,t.member_count,COUNT(e.email_id) FROM threads t LEFT "
            "JOIN thread_email_members m ON m.account_id=t.account_id AND "
            "m.thread_id=t.thread_id LEFT JOIN emails e ON e.account_id=m.account_id AND "
            "e.email_id=m.email_id AND e.thread_id=m.thread_id WHERE t.account_id=:account_id AND "
            "t.thread_id=:thread_id "
            "GROUP BY t.account_id,t.thread_id,t.membership_freshness,t.member_count"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":thread_id"),
                        QString::fromStdString(std::string{threadId}));
        if (!query.exec())
            return makeQueryError(QStringLiteral("Read thread child coverage"), query);
        if (!query.next())
            return std::optional<ThreadCoverage>{std::nullopt};

        const auto freshness = freshnessFromValue(query.value(0).toString());
        const auto globalCount = query.value(1).toULongLong();
        const auto materializedCount = query.value(2).toULongLong();
        return std::optional<ThreadCoverage>{ThreadCoverage{
            .freshness = freshness,
            .globalMemberCount = globalCount,
            .materializedMemberCount = materializedCount,
            .childEmailsComplete =
                freshness == ThreadMembershipFreshness::Current && materializedCount == globalCount,
        }};
    }

    std::variant<std::optional<std::size_t>, DatabaseError>
    ThreadRepository::countMailboxMembersIfComplete(const std::string_view accountId,
                                                    const std::string_view mailboxId,
                                                    const std::string_view threadId) const
    {
        if (const auto error = m_connection.validate())
            return *error;
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT CASE WHEN t.membership_freshness='current' AND "
            "t.member_count=COUNT(e.email_id) THEN COUNT(em.email_id) ELSE NULL END FROM threads t "
            "LEFT JOIN thread_email_members m ON m.account_id=t.account_id AND "
            "m.thread_id=t.thread_id LEFT JOIN emails e ON e.account_id=m.account_id AND "
            "e.email_id=m.email_id AND e.thread_id=m.thread_id LEFT JOIN email_mailboxes em ON "
            "em.account_id=e.account_id AND "
            "em.email_id=e.email_id AND em.mailbox_id=:mailbox_id WHERE "
            "t.account_id=:account_id AND t.thread_id=:thread_id GROUP BY "
            "t.account_id,t.thread_id,t.membership_freshness,t.member_count"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":mailbox_id"),
                        QString::fromStdString(std::string{mailboxId}));
        query.bindValue(QStringLiteral(":thread_id"),
                        QString::fromStdString(std::string{threadId}));
        if (!query.exec())
            return makeQueryError(QStringLiteral("Count complete mailbox thread membership"),
                                  query);
        if (!query.next() || query.value(0).isNull())
            return std::optional<std::size_t>{std::nullopt};
        return std::optional<std::size_t>{query.value(0).toULongLong()};
    }

} // namespace javelin::jmap::cache
