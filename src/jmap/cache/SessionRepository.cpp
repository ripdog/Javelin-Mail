#include "jmap/cache/SessionRepository.h"

#include <glaze/glaze.hpp>

#include <QSqlError>
#include <QSqlQuery>

#include <string>
#include <unordered_set>

namespace javelin::jmap::cache
{

    namespace detail
    {

        struct RawCoreCapability
        {
            std::optional<std::uint64_t> maxSizeUpload;
            std::optional<std::uint64_t> maxConcurrentUpload;
            std::optional<std::uint64_t> maxConcurrentRequests;
            std::optional<std::uint64_t> maxCallsInRequest;
            std::optional<std::uint64_t> maxObjectsInGet;
            std::optional<std::uint64_t> maxObjectsInSet;
            std::vector<std::string> collationAlgorithms;
        };

    } // namespace detail

} // namespace javelin::jmap::cache

template <> struct glz::meta<javelin::jmap::cache::detail::RawCoreCapability>
{
    using T = javelin::jmap::cache::detail::RawCoreCapability;

    static constexpr auto value = glz::object(
        "maxSizeUpload", &T::maxSizeUpload, "maxConcurrentUpload", &T::maxConcurrentUpload,
        "maxConcurrentRequests", &T::maxConcurrentRequests, "maxCallsInRequest",
        &T::maxCallsInRequest, "maxObjectsInGet", &T::maxObjectsInGet, "maxObjectsInSet",
        &T::maxObjectsInSet, "collationAlgorithms", &T::collationAlgorithms);
};

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
        serializeCoreCapability(const std::optional<javelin::jmap::api::CoreCapability>& capability)
        {
            if (!capability.has_value())
            {
                return "null";
            }

            detail::RawCoreCapability raw{
                .maxSizeUpload = capability->maxSizeUpload,
                .maxConcurrentUpload = capability->maxConcurrentUpload,
                .maxConcurrentRequests = capability->maxConcurrentRequests,
                .maxCallsInRequest = capability->maxCallsInRequest,
                .maxObjectsInGet = capability->maxObjectsInGet,
                .maxObjectsInSet = capability->maxObjectsInSet,
                .collationAlgorithms = capability->collationAlgorithms,
            };

            std::string buffer;
            const auto writeError = glz::write_json(raw, buffer);
            if (writeError)
            {
                return "null";
            }

            return buffer;
        }

        [[nodiscard]] std::optional<javelin::jmap::api::CoreCapability>
        deserializeCoreCapability(const QString& json)
        {
            std::string buffer = json.toStdString();
            if (buffer == "null")
            {
                return std::nullopt;
            }

            detail::RawCoreCapability raw;
            const auto readError =
                glz::read<glz::opts{.error_on_unknown_keys = false}>(raw, buffer);
            if (readError)
            {
                return std::nullopt;
            }

            return javelin::jmap::api::CoreCapability{
                .maxSizeUpload = raw.maxSizeUpload,
                .maxConcurrentUpload = raw.maxConcurrentUpload,
                .maxConcurrentRequests = raw.maxConcurrentRequests,
                .maxCallsInRequest = raw.maxCallsInRequest,
                .maxObjectsInGet = raw.maxObjectsInGet,
                .maxObjectsInSet = raw.maxObjectsInSet,
                .collationAlgorithms = std::move(raw.collationAlgorithms),
            };
        }

        void bindAccount(QSqlQuery& query, const javelin::jmap::api::Account& account)
        {
            query.bindValue(QStringLiteral(":account_id"), QString::fromStdString(account.id));
            query.bindValue(QStringLiteral(":email_address"), QStringLiteral(""));
            query.bindValue(QStringLiteral(":session_url"), QStringLiteral(""));
            query.bindValue(QStringLiteral(":is_primary"), 0);
            query.bindValue(QStringLiteral(":name"), QString::fromStdString(account.name));
            query.bindValue(QStringLiteral(":is_personal"), account.isPersonal ? 1 : 0);
            query.bindValue(QStringLiteral(":is_read_only"), account.isReadOnly ? 1 : 0);
            query.bindValue(QStringLiteral(":cap_mail"), account.accountCapabilities.mail ? 1 : 0);
            query.bindValue(QStringLiteral(":cap_submission"),
                            account.accountCapabilities.submission ? 1 : 0);
        }

    } // namespace

    SessionRepository::SessionRepository(DatabaseConnection& connection) : m_connection(connection)
    {
    }

    std::optional<DatabaseError>
    SessionRepository::replace(const std::string_view ownerAccountId,
                               const javelin::jmap::api::Session& session)
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
                .message = QStringLiteral("Begin session replacement transaction: ") +
                           database.lastError().text(),
            };
        }

        QSqlQuery deleteSession{database};
        deleteSession.prepare(
            QStringLiteral("DELETE FROM sessions WHERE account_id = :account_id"));
        deleteSession.bindValue(QStringLiteral(":account_id"),
                                QString::fromStdString(std::string{ownerAccountId}));
        if (!deleteSession.exec())
        {
            database.rollback();
            return makeQueryError(QStringLiteral("Delete session row"), deleteSession);
        }

        QSqlQuery insertAccount{database};
        insertAccount.prepare(QStringLiteral(
            "INSERT INTO accounts ("
            "account_id, email_address, session_url, is_primary, name, is_personal, "
            "is_read_only, "
            "cap_mail, cap_submission"
            ") VALUES ("
            ":account_id, :email_address, :session_url, :is_primary, :name, :is_personal, "
            ":is_read_only, :cap_mail, :cap_submission"
            ") ON CONFLICT(account_id) DO UPDATE SET "
            "email_address = excluded.email_address, "
            "session_url = excluded.session_url, "
            "is_primary = excluded.is_primary, "
            "name = excluded.name, "
            "is_personal = excluded.is_personal, "
            "is_read_only = excluded.is_read_only, "
            "cap_mail = excluded.cap_mail, "
            "cap_submission = excluded.cap_submission"));
        std::unordered_set<std::string> sessionAccountIds;
        sessionAccountIds.reserve(session.accounts.size());
        for (const auto& [accountId, account] : session.accounts)
        {
            sessionAccountIds.insert(accountId);
            auto storedAccount = account;
            storedAccount.id = accountId;
            bindAccount(insertAccount, storedAccount);
            insertAccount.bindValue(QStringLiteral(":is_primary"),
                                    (session.primaryAccounts.mailAccountId == accountId ||
                                     session.primaryAccounts.submissionAccountId == accountId)
                                        ? 1
                                        : 0);
            if (!insertAccount.exec())
            {
                database.rollback();
                return makeQueryError(QStringLiteral("Insert cached account"), insertAccount);
            }
        }

        QSqlQuery existingAccountsQuery{database};
        if (!existingAccountsQuery.exec(QStringLiteral("SELECT account_id FROM accounts")))
        {
            database.rollback();
            return makeQueryError(QStringLiteral("Read cached accounts"), existingAccountsQuery);
        }

        std::vector<std::string> obsoleteAccountIds;
        while (existingAccountsQuery.next())
        {
            const auto existingAccountId = existingAccountsQuery.value(0).toString().toStdString();
            if (!sessionAccountIds.contains(existingAccountId))
            {
                obsoleteAccountIds.push_back(std::move(existingAccountId));
            }
        }

        if (!obsoleteAccountIds.empty())
        {
            QSqlQuery deleteAccount{database};
            deleteAccount.prepare(
                QStringLiteral("DELETE FROM accounts WHERE account_id = :account_id"));
            for (const auto& obsoleteAccountId : obsoleteAccountIds)
            {
                deleteAccount.bindValue(QStringLiteral(":account_id"),
                                        QString::fromStdString(obsoleteAccountId));
                if (!deleteAccount.exec())
                {
                    database.rollback();
                    return makeQueryError(QStringLiteral("Delete obsolete cached account"),
                                          deleteAccount);
                }
            }
        }

        QSqlQuery insertSession{database};
        insertSession.prepare(QStringLiteral(
            "INSERT INTO sessions ("
            "account_id, api_url, download_url, upload_url, event_source_url, state, username, "
            "has_core_capability, has_mail_capability, has_submission_capability, "
            "core_capabilities_json, primary_mail_account_id, primary_submission_account_id"
            ") VALUES ("
            ":account_id, :api_url, :download_url, :upload_url, :event_source_url, :state, "
            ":username, :has_core_capability, :has_mail_capability, "
            ":has_submission_capability, "
            ":core_capabilities_json, :primary_mail_account_id, "
            ":primary_submission_account_id)"));
        insertSession.bindValue(QStringLiteral(":account_id"),
                                QString::fromStdString(std::string{ownerAccountId}));
        insertSession.bindValue(QStringLiteral(":api_url"), QString::fromStdString(session.apiUrl));
        insertSession.bindValue(QStringLiteral(":download_url"),
                                QString::fromStdString(session.downloadUrl));
        insertSession.bindValue(QStringLiteral(":upload_url"),
                                QString::fromStdString(session.uploadUrl));
        insertSession.bindValue(QStringLiteral(":event_source_url"),
                                session.eventSourceUrl.has_value()
                                    ? QVariant{QString::fromStdString(*session.eventSourceUrl)}
                                    : QVariant{});
        insertSession.bindValue(QStringLiteral(":state"), QString::fromStdString(session.state));
        insertSession.bindValue(QStringLiteral(":username"),
                                QString::fromStdString(session.username));
        insertSession.bindValue(QStringLiteral(":has_core_capability"),
                                session.capabilities.core ? 1 : 0);
        insertSession.bindValue(QStringLiteral(":has_mail_capability"),
                                session.capabilities.mail ? 1 : 0);
        insertSession.bindValue(QStringLiteral(":has_submission_capability"),
                                session.capabilities.submission ? 1 : 0);
        insertSession.bindValue(
            QStringLiteral(":core_capabilities_json"),
            QString::fromStdString(serializeCoreCapability(session.capabilities.coreDetails)));
        insertSession.bindValue(
            QStringLiteral(":primary_mail_account_id"),
            session.primaryAccounts.mailAccountId.has_value()
                ? QVariant{QString::fromStdString(*session.primaryAccounts.mailAccountId)}
                : QVariant{});
        insertSession.bindValue(
            QStringLiteral(":primary_submission_account_id"),
            session.primaryAccounts.submissionAccountId.has_value()
                ? QVariant{QString::fromStdString(*session.primaryAccounts.submissionAccountId)}
                : QVariant{});
        if (!insertSession.exec())
        {
            database.rollback();
            return makeQueryError(QStringLiteral("Insert cached session"), insertSession);
        }

        if (!database.commit())
        {
            database.rollback();
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Commit session replacement transaction: ") +
                           database.lastError().text(),
            };
        }

        return std::nullopt;
    }

    std::variant<std::optional<javelin::jmap::api::Session>, DatabaseError>
    SessionRepository::load(const std::string_view ownerAccountId) const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        QSqlQuery sessionQuery{m_connection.database()};
        sessionQuery.prepare(QStringLiteral(
            "SELECT api_url, download_url, upload_url, event_source_url, state, username, "
            "has_core_capability, has_mail_capability, has_submission_capability, "
            "core_capabilities_json, primary_mail_account_id, primary_submission_account_id "
            "FROM sessions WHERE account_id = :account_id"));
        sessionQuery.bindValue(QStringLiteral(":account_id"),
                               QString::fromStdString(std::string{ownerAccountId}));
        if (!sessionQuery.exec())
        {
            return makeQueryError(QStringLiteral("Read cached session"), sessionQuery);
        }

        if (!sessionQuery.next())
        {
            return std::optional<javelin::jmap::api::Session>{std::nullopt};
        }

        javelin::jmap::api::Session session{
            .username = sessionQuery.value(5).toString().toStdString(),
            .apiUrl = sessionQuery.value(0).toString().toStdString(),
            .downloadUrl = sessionQuery.value(1).toString().toStdString(),
            .uploadUrl = sessionQuery.value(2).toString().toStdString(),
            .eventSourceUrl = sessionQuery.value(3).isNull()
                                  ? std::nullopt
                                  : std::optional{sessionQuery.value(3).toString().toStdString()},
            .state = sessionQuery.value(4).toString().toStdString(),
            .capabilities =
                {
                    .core = sessionQuery.value(6).toInt() != 0,
                    .coreDetails = deserializeCoreCapability(sessionQuery.value(9).toString()),
                    .mail = sessionQuery.value(7).toInt() != 0,
                    .submission = sessionQuery.value(8).toInt() != 0,
                },
            .accounts = {},
            .primaryAccounts =
                {
                    .mailAccountId =
                        sessionQuery.value(10).isNull()
                            ? std::nullopt
                            : std::optional{sessionQuery.value(10).toString().toStdString()},
                    .submissionAccountId =
                        sessionQuery.value(11).isNull()
                            ? std::nullopt
                            : std::optional{sessionQuery.value(11).toString().toStdString()},
                },
        };

        QSqlQuery accountQuery{m_connection.database()};
        if (!accountQuery.exec(QStringLiteral(
                "SELECT account_id, name, is_personal, is_read_only, cap_mail, cap_submission "
                "FROM accounts ORDER BY account_id")))
        {
            return makeQueryError(QStringLiteral("Read cached accounts"), accountQuery);
        }

        while (accountQuery.next())
        {
            javelin::jmap::api::Account account{
                .id = accountQuery.value(0).toString().toStdString(),
                .name = accountQuery.value(1).toString().toStdString(),
                .isPersonal = accountQuery.value(2).toInt() != 0,
                .isReadOnly = accountQuery.value(3).toInt() != 0,
                .accountCapabilities =
                    {
                        .mail = accountQuery.value(4).toInt() != 0,
                        .submission = accountQuery.value(5).toInt() != 0,
                    },
            };
            session.accounts.emplace(account.id, std::move(account));
        }

        return std::optional<javelin::jmap::api::Session>{std::move(session)};
    }

} // namespace javelin::jmap::cache
