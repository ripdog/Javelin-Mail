#include "jmap/cache/QueryWindowReadRepository.h"

#include "jmap/cache/MailboxMessageReader.h"
#include "jmap/cache/MailboxStatisticsReadRepository.h"
#include "jmap/cache/MailboxWindowReadRepository.h"
#include "jmap/cache/MessageSummaryReadRepository.h"
#include "jmap/cache/SearchWindowReadRepository.h"

#include <glaze/glaze.hpp>

#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QSqlError>
#include <QSqlQuery>

namespace javelin::jmap::cache
{
    Q_LOGGING_CATEGORY(logQueryWindowPerformance, "jmap.cache.query")

    namespace
    {
        [[nodiscard]] DatabaseError makeQueryError(const QString& operation, const QSqlQuery& query)
        {
            return databaseError(operation, query.lastError());
        }

        [[nodiscard]] std::optional<std::uint64_t> optionalCount(const QVariant& value)
        {
            return value.isNull() ? std::nullopt
                                  : std::optional<std::uint64_t>{value.toULongLong()};
        }
    } // namespace

    QueryWindowReadRepository::QueryWindowReadRepository(DatabaseConnection& connection,
                                                         const MailboxMessageReader& mailboxReader)
        : m_connection(connection), m_mailboxReader(mailboxReader)
    {
    }

    QueryWindowReadRepository::QueryWindowReadRepository(ReadOnlyDatabaseConnection& connection,
                                                         const MailboxMessageReader& mailboxReader)
        : m_connection(connection), m_mailboxReader(mailboxReader)
    {
    }

    QueryWindowReadRepository::QueryWindowReadRepository(DatabaseReadView connection,
                                                         const MailboxMessageReader& mailboxReader)
        : m_connection(connection), m_mailboxReader(mailboxReader)
    {
    }

    std::variant<std::optional<SearchWindowPage>, DatabaseError>
    QueryWindowReadRepository::loadSearchWindow(const std::string_view accountId,
                                                const std::string_view queryKey,
                                                const std::size_t offset,
                                                const std::size_t limit) const
    {
        const auto windowResult =
            SearchWindowReadRepository{m_connection}.find(accountId, queryKey, offset, limit);
        const auto* window = std::get_if<std::optional<SearchWindowRecord>>(&windowResult);
        if (window == nullptr)
            return std::get<DatabaseError>(windowResult);
        if (!window->has_value())
            return std::optional<SearchWindowPage>{std::nullopt};

        const auto messagesResult =
            MessageSummaryReadRepository{m_connection}.listMessagesByEmailIds(accountId,
                                                                              (*window)->emailIds);
        const auto* messages = std::get_if<std::vector<MessageListItem>>(&messagesResult);
        if (messages == nullptr)
            return std::get<DatabaseError>(messagesResult);

        return std::optional<SearchWindowPage>{SearchWindowPage{
            .offset = (*window)->offset,
            .limit = (*window)->limit,
            .position = (*window)->position,
            .returnedLimit = (*window)->returnedLimit,
            .total = (*window)->total,
            .queryState = (*window)->queryState,
            .coverage = (*window)->coverage,
            .materialization = (*window)->materialization,
            .items = *messages,
        }};
    }

    std::variant<std::optional<MailboxWindowPage>, DatabaseError>
    QueryWindowReadRepository::loadMailboxWindow(
        const std::string_view accountId, const std::string_view queryKey,
        const std::size_t requestedOffset, const std::size_t requestedLimit,
        const javelin::jmap::query::EmailListSort sort) const
    {
        QElapsedTimer timer;
        timer.start();
        const auto windowResult = MailboxWindowReadRepository{m_connection}.find(
            accountId, queryKey, requestedOffset, requestedLimit);
        const auto windowMilliseconds = timer.restart();
        const auto* window = std::get_if<std::optional<MailboxWindowRecord>>(&windowResult);
        if (window == nullptr)
            return std::get<DatabaseError>(windowResult);
        if (!window->has_value())
            return std::optional<MailboxWindowPage>{std::nullopt};

        if ((*window)->coverage == QueryWindowCoverage::LocallyProjected)
        {
            const auto projectedResult = m_mailboxReader.listMailboxMessages(
                accountId, (*window)->mailboxId, requestedLimit, requestedOffset, sort);
            const auto* projected = std::get_if<std::vector<MessageListItem>>(&projectedResult);
            if (projected == nullptr)
                return std::get<DatabaseError>(projectedResult);

            const auto totalResult =
                MailboxStatisticsReadRepository{m_connection}.countMailboxMessages(
                    accountId, (*window)->mailboxId);
            const auto* total = std::get_if<std::size_t>(&totalResult);
            if (total == nullptr)
                return std::get<DatabaseError>(totalResult);

            return std::optional<MailboxWindowPage>{MailboxWindowPage{
                .requestedOffset = requestedOffset,
                .requestedLimit = requestedLimit,
                .position = requestedOffset,
                .returnedLimit = projected->size(),
                .total = *total,
                .queryState = (*window)->queryState,
                .coverage = QueryWindowCoverage::LocallyProjected,
                .materialization = (*window)->materialization,
                .items = *projected,
            }};
        }

        const auto messagesResult = listMailboxWindowMessagesByEmailIds(
            accountId, (*window)->mailboxId, (*window)->emailIds);
        const auto messageMilliseconds = timer.elapsed();
        if (windowMilliseconds + messageMilliseconds >= 50)
        {
            qCWarning(logQueryWindowPerformance).noquote()
                << "Slow mailbox window load" << QString::fromStdString(std::string{accountId})
                << QString::fromStdString((*window)->mailboxId) << "windowMs" << windowMilliseconds
                << "messagesMs" << messageMilliseconds << "items"
                << static_cast<qulonglong>((*window)->emailIds.size());
        }
        const auto* messages = std::get_if<std::vector<MessageListItem>>(&messagesResult);
        if (messages == nullptr)
            return std::get<DatabaseError>(messagesResult);

        return std::optional<MailboxWindowPage>{MailboxWindowPage{
            .requestedOffset = (*window)->requestedOffset,
            .requestedLimit = (*window)->requestedLimit,
            .position = (*window)->position,
            .returnedLimit = (*window)->returnedLimit,
            .total = (*window)->total,
            .queryState = (*window)->queryState,
            .coverage = (*window)->coverage,
            .materialization = (*window)->materialization,
            .items = *messages,
        }};
    }

    std::variant<std::vector<MessageListItem>, DatabaseError>
    QueryWindowReadRepository::listMailboxWindowMessagesByEmailIds(
        const std::string_view accountId, const std::string_view mailboxId,
        const std::vector<std::string>& emailIds) const
    {
        if (const auto error = m_connection.validate())
            return *error;
        if (emailIds.empty())
            return std::vector<MessageListItem>{};

        std::string emailIdsJson;
        if (const auto writeError = glz::write_json(emailIds, emailIdsJson))
        {
            Q_UNUSED(writeError);
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Serialize mailbox-window ids failed."),
            };
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "WITH requested AS MATERIALIZED ("
            "  SELECT value AS email_id,CAST(key AS INTEGER) AS window_position "
            "  FROM json_each(:email_ids_json)"
            ") SELECT r.email_id,"
            "  CASE WHEN t.membership_freshness='current' AND t.member_count=("
            "    SELECT COUNT(cached.email_id) FROM thread_email_members member "
            "    LEFT JOIN emails cached ON cached.account_id=member.account_id "
            "      AND cached.email_id=member.email_id "
            "      AND NOT EXISTS(SELECT 1 FROM email_summary_refresh_requests refresh "
            "        WHERE refresh.account_id=member.account_id "
            "          AND refresh.email_id=member.email_id) "
            "    WHERE member.account_id=e.account_id AND member.thread_id=e.thread_id"
            "  ) THEN ("
            "    SELECT COUNT(mailbox_member.email_id) FROM thread_email_members member "
            "    INNER JOIN email_mailboxes mailbox_member "
            "      ON mailbox_member.account_id=member.account_id "
            "      AND mailbox_member.email_id=member.email_id "
            "      AND mailbox_member.mailbox_id=:mailbox_id "
            "    WHERE member.account_id=e.account_id AND member.thread_id=e.thread_id"
            "  ) ELSE NULL END "
            "FROM requested r "
            "CROSS JOIN emails e ON e.account_id=:account_id AND e.email_id=r.email_id "
            "CROSS JOIN email_mailboxes selected_membership "
            "  ON selected_membership.account_id=e.account_id "
            "  AND selected_membership.email_id=e.email_id "
            "  AND selected_membership.mailbox_id=:mailbox_id "
            "LEFT JOIN threads t ON t.account_id=e.account_id AND t.thread_id=e.thread_id "
            "ORDER BY r.window_position"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":mailbox_id"),
                        QString::fromStdString(std::string{mailboxId}));
        query.bindValue(QStringLiteral(":email_ids_json"), QString::fromStdString(emailIdsJson));
        if (!query.exec())
            return makeQueryError(QStringLiteral("Project mailbox-window membership"), query);

        std::vector<std::string> projectedIds;
        std::vector<std::optional<std::uint64_t>> mailboxThreadMessageCounts;
        projectedIds.reserve(emailIds.size());
        mailboxThreadMessageCounts.reserve(emailIds.size());
        while (query.next())
        {
            projectedIds.push_back(query.value(0).toString().toStdString());
            mailboxThreadMessageCounts.push_back(optionalCount(query.value(1)));
        }

        auto messagesResult = MessageSummaryReadRepository{m_connection}.listMessagesByEmailIds(
            accountId, projectedIds);
        auto* messages = std::get_if<std::vector<MessageListItem>>(&messagesResult);
        if (messages == nullptr)
            return std::get<DatabaseError>(messagesResult);
        if (messages->size() != mailboxThreadMessageCounts.size())
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message =
                    QStringLiteral("Mailbox-window projection returned inconsistent row counts."),
            };
        }
        for (std::size_t index = 0; index < messages->size(); ++index)
            (*messages)[index].mailboxThreadMessageCount = mailboxThreadMessageCounts[index];
        return std::move(*messages);
    }

} // namespace javelin::jmap::cache
