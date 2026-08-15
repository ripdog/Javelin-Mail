#include "jmap/cache/SessionRepository.h"

#include "jmap/cache/AccountRepository.h"

#include <glaze/glaze.hpp>

#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include <string>

namespace javelin::jmap::cache
{

    namespace detail
    {

        struct RawCoreCapability
        {
            std::optional<std::uint64_t> maxSizeUpload;
            std::optional<std::uint64_t> maxConcurrentUpload;
            std::optional<std::uint64_t> maxSizeRequest;
            std::optional<std::uint64_t> maxConcurrentRequests;
            std::optional<std::uint64_t> maxCallsInRequest;
            std::optional<std::uint64_t> maxObjectsInGet;
            std::optional<std::uint64_t> maxObjectsInSet;
            std::vector<std::string> collationAlgorithms;
        };

        struct RawContactsCapability
        {
            std::optional<std::uint64_t> maxAddressBooksPerCard;
            bool mayCreateAddressBook = false;
        };

        struct RawCalendarsCapability
        {
            std::optional<std::uint64_t> maxCalendarsPerEvent;
            std::string minDateTime;
            std::string maxDateTime;
            std::string maxExpandedQueryDuration;
            std::optional<std::uint64_t> maxParticipantsPerEvent;
            bool mayCreateCalendar = false;
        };

    } // namespace detail

} // namespace javelin::jmap::cache

template <> struct glz::meta<javelin::jmap::cache::detail::RawCoreCapability>
{
    using T = javelin::jmap::cache::detail::RawCoreCapability;

    static constexpr auto value = glz::object(
        "maxSizeUpload", &T::maxSizeUpload, "maxConcurrentUpload", &T::maxConcurrentUpload,
        "maxSizeRequest", &T::maxSizeRequest, "maxConcurrentRequests", &T::maxConcurrentRequests,
        "maxCallsInRequest", &T::maxCallsInRequest, "maxObjectsInGet", &T::maxObjectsInGet,
        "maxObjectsInSet", &T::maxObjectsInSet, "collationAlgorithms", &T::collationAlgorithms);
};

template <> struct glz::meta<javelin::jmap::cache::detail::RawContactsCapability>
{
    using T = javelin::jmap::cache::detail::RawContactsCapability;
    static constexpr auto value = glz::object("maxAddressBooksPerCard", &T::maxAddressBooksPerCard,
                                              "mayCreateAddressBook", &T::mayCreateAddressBook);
};

template <> struct glz::meta<javelin::jmap::cache::detail::RawCalendarsCapability>
{
    using T = javelin::jmap::cache::detail::RawCalendarsCapability;
    static constexpr auto value =
        glz::object("maxCalendarsPerEvent", &T::maxCalendarsPerEvent, "minDateTime",
                    &T::minDateTime, "maxDateTime", &T::maxDateTime, "maxExpandedQueryDuration",
                    &T::maxExpandedQueryDuration, "maxParticipantsPerEvent",
                    &T::maxParticipantsPerEvent, "mayCreateCalendar", &T::mayCreateCalendar);
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

        [[nodiscard]] DatabaseError invalidSessionIdentity(const QString& detail)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Store connection-qualified session: ") + detail,
            };
        }

        [[nodiscard]] std::variant<std::string, DatabaseError>
        localAccountId(QSqlDatabase& database, const std::string_view connectionId,
                       const std::string_view remoteAccountId)
        {
            QSqlQuery existingLocator{database};
            existingLocator.prepare(QStringLiteral(
                "SELECT account_id FROM accounts WHERE connection_id=:connection_id AND "
                "remote_account_id=:remote_account_id"));
            existingLocator.bindValue(QStringLiteral(":connection_id"),
                                      QString::fromStdString(std::string{connectionId}));
            existingLocator.bindValue(QStringLiteral(":remote_account_id"),
                                      QString::fromStdString(std::string{remoteAccountId}));
            if (!existingLocator.exec())
                return makeQueryError(QStringLiteral("Resolve cached account locator"),
                                      existingLocator);
            if (existingLocator.next())
                return existingLocator.value(0).toString().toStdString();

            QSqlQuery existingKey{database};
            existingKey.prepare(
                QStringLiteral("SELECT 1 FROM accounts WHERE account_id=:account_id"));
            existingKey.bindValue(QStringLiteral(":account_id"),
                                  QString::fromStdString(std::string{remoteAccountId}));
            if (!existingKey.exec())
                return makeQueryError(QStringLiteral("Inspect cached account key"), existingKey);
            if (!existingKey.next())
                return std::string{remoteAccountId};

            for (;;)
            {
                const std::string candidate =
                    QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
                existingKey.bindValue(QStringLiteral(":account_id"),
                                      QString::fromStdString(candidate));
                if (!existingKey.exec())
                    return makeQueryError(QStringLiteral("Inspect generated cached account key"),
                                          existingKey);
                if (!existingKey.next())
                    return candidate;
            }
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
                .maxSizeRequest = capability->maxSizeRequest,
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
                .maxSizeRequest = raw.maxSizeRequest,
                .maxConcurrentRequests = raw.maxConcurrentRequests,
                .maxCallsInRequest = raw.maxCallsInRequest,
                .maxObjectsInGet = raw.maxObjectsInGet,
                .maxObjectsInSet = raw.maxObjectsInSet,
                .collationAlgorithms = std::move(raw.collationAlgorithms),
            };
        }

        void bindAccount(QSqlQuery& query, const std::string_view localAccountId,
                         const javelin::jmap::api::Account& account)
        {
            query.bindValue(QStringLiteral(":account_id"),
                            QString::fromStdString(std::string{localAccountId}));
            query.bindValue(QStringLiteral(":email_address"), QStringLiteral(""));
            query.bindValue(QStringLiteral(":session_url"), QStringLiteral(""));
            query.bindValue(QStringLiteral(":is_primary"), 0);
            query.bindValue(QStringLiteral(":name"), QString::fromStdString(account.name));
            query.bindValue(QStringLiteral(":is_personal"), account.isPersonal ? 1 : 0);
            query.bindValue(QStringLiteral(":is_read_only"), account.isReadOnly ? 1 : 0);
            query.bindValue(QStringLiteral(":cap_mail"), account.accountCapabilities.mail ? 1 : 0);
            query.bindValue(
                QStringLiteral(":mail_may_create_top_level_mailbox"),
                account.accountCapabilities.mailDetails.has_value() &&
                        account.accountCapabilities.mailDetails->mayCreateTopLevelMailbox
                    ? 1
                    : 0);
            query.bindValue(QStringLiteral(":cap_submission"),
                            account.accountCapabilities.submission.has_value() ? 1 : 0);
            query.bindValue(QStringLiteral(":submission_max_delayed_send"),
                            account.accountCapabilities.submission.has_value()
                                ? static_cast<qulonglong>(
                                      account.accountCapabilities.submission->maxDelayedSend)
                                : qulonglong{0});
            query.bindValue(QStringLiteral(":cap_contacts"),
                            account.accountCapabilities.contacts.has_value() ? 1 : 0);
            std::string contactsJson = "null";
            if (account.accountCapabilities.contacts.has_value())
            {
                const detail::RawContactsCapability raw{
                    .maxAddressBooksPerCard =
                        account.accountCapabilities.contacts->maxAddressBooksPerCard,
                    .mayCreateAddressBook =
                        account.accountCapabilities.contacts->mayCreateAddressBook,
                };
                if (glz::write_json(raw, contactsJson))
                {
                    contactsJson = "null";
                }
            }
            query.bindValue(QStringLiteral(":contacts_capabilities_json"),
                            QString::fromStdString(contactsJson));
            query.bindValue(QStringLiteral(":cap_calendars"),
                            account.accountCapabilities.calendars.has_value() ? 1 : 0);
            std::string calendarsJson = "null";
            if (const auto& capability = account.accountCapabilities.calendars)
            {
                const detail::RawCalendarsCapability raw{
                    .maxCalendarsPerEvent = capability->maxCalendarsPerEvent,
                    .minDateTime = capability->minDateTime,
                    .maxDateTime = capability->maxDateTime,
                    .maxExpandedQueryDuration = capability->maxExpandedQueryDuration,
                    .maxParticipantsPerEvent = capability->maxParticipantsPerEvent,
                    .mayCreateCalendar = capability->mayCreateCalendar,
                };
                if (glz::write_json(raw, calendarsJson))
                {
                    calendarsJson = "null";
                }
            }
            query.bindValue(QStringLiteral(":calendars_capabilities_json"),
                            QString::fromStdString(calendarsJson));
        }

    } // namespace

    SessionRepository::SessionRepository(DatabaseConnection& connection) : m_connection(connection)
    {
    }

    std::optional<DatabaseError>
    SessionRepository::replace(const std::string_view ownerAccountId,
                               const javelin::jmap::api::Session& session)
    {
        AccountRepository accounts{m_connection};
        if (const auto error = accounts.claimLegacyConnection(
                ownerAccountId, {QString::fromStdString(std::string{ownerAccountId})}))
        {
            return error;
        }
        const auto result = replaceForConnection(ownerAccountId, ownerAccountId, session);
        if (const auto* error = std::get_if<DatabaseError>(&result))
            return *error;
        return std::nullopt;
    }

    SessionReplaceResult
    SessionRepository::replaceForConnection(const std::string_view connectionId,
                                            const std::string_view ownerRemoteAccountId,
                                            const javelin::jmap::api::Session& session)
    {
        if (const auto error = m_connection.validate())
            return *error;
        if (connectionId.empty() || ownerRemoteAccountId.empty())
            return invalidSessionIdentity(QStringLiteral("connection and owner ids are required"));
        if (!session.accounts.contains(std::string{ownerRemoteAccountId}))
        {
            return invalidSessionIdentity(
                QStringLiteral("owner account is not present in the discovered session"));
        }

        const DatabaseWriteScope writeScope{m_connection};
        QSqlDatabase& database = m_connection.database();
        if (!database.transaction())
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Begin session replacement transaction: ") +
                           database.lastError().text(),
            };
        }

        StoredSessionAccounts storedAccounts;
        storedAccounts.accountIdsByRemoteId.reserve(session.accounts.size());
        for (const auto& [remoteAccountId, account] : session.accounts)
        {
            static_cast<void>(account);
            const auto localIdResult = localAccountId(database, connectionId, remoteAccountId);
            if (const auto* error = std::get_if<DatabaseError>(&localIdResult))
            {
                database.rollback();
                return *error;
            }
            storedAccounts.accountIdsByRemoteId.emplace(remoteAccountId,
                                                        std::get<std::string>(localIdResult));
        }
        storedAccounts.ownerAccountId =
            storedAccounts.accountIdsByRemoteId.at(std::string{ownerRemoteAccountId});

        QSqlQuery deleteSession{database};
        deleteSession.prepare(
            QStringLiteral("DELETE FROM sessions WHERE account_id = :account_id"));
        deleteSession.bindValue(QStringLiteral(":account_id"),
                                QString::fromStdString(storedAccounts.ownerAccountId));
        if (!deleteSession.exec())
        {
            database.rollback();
            return makeQueryError(QStringLiteral("Delete session row"), deleteSession);
        }

        QSqlQuery insertAccount{database};
        insertAccount.prepare(QStringLiteral(
            "INSERT INTO accounts ("
            "account_id, connection_id, remote_account_id, email_address, session_url, is_primary, "
            "name, is_personal, is_read_only, owner_account_id, "
            "cap_mail, mail_may_create_top_level_mailbox, cap_submission, "
            "submission_max_delayed_send, cap_contacts, "
            "contacts_capabilities_json, cap_calendars, calendars_capabilities_json"
            ") VALUES ("
            ":account_id, :connection_id, :remote_account_id, :email_address, :session_url, "
            ":is_primary, :name, :is_personal, :is_read_only, :owner_account_id, :cap_mail, "
            ":mail_may_create_top_level_mailbox, "
            ":cap_submission, :submission_max_delayed_send, :cap_contacts, "
            ":contacts_capabilities_json, "
            ":cap_calendars, :calendars_capabilities_json"
            ") ON CONFLICT(account_id) DO UPDATE SET "
            "connection_id = excluded.connection_id, "
            "remote_account_id = excluded.remote_account_id, "
            "email_address = excluded.email_address, "
            "session_url = excluded.session_url, "
            "is_primary = excluded.is_primary, "
            "name = excluded.name, "
            "is_personal = excluded.is_personal, "
            "is_read_only = excluded.is_read_only, "
            "owner_account_id = excluded.owner_account_id, "
            "cap_mail = excluded.cap_mail, "
            "mail_may_create_top_level_mailbox = excluded.mail_may_create_top_level_mailbox, "
            "cap_submission = excluded.cap_submission, "
            "submission_max_delayed_send = excluded.submission_max_delayed_send, "
            "cap_contacts = excluded.cap_contacts, "
            "contacts_capabilities_json = excluded.contacts_capabilities_json, "
            "cap_calendars = excluded.cap_calendars, "
            "calendars_capabilities_json = excluded.calendars_capabilities_json"));
        for (const auto& [accountId, account] : session.accounts)
        {
            bindAccount(insertAccount, storedAccounts.accountIdsByRemoteId.at(accountId), account);
            insertAccount.bindValue(QStringLiteral(":connection_id"),
                                    QString::fromStdString(std::string{connectionId}));
            insertAccount.bindValue(QStringLiteral(":remote_account_id"),
                                    QString::fromStdString(accountId));
            insertAccount.bindValue(QStringLiteral(":is_primary"),
                                    (session.primaryAccounts.mailAccountId == accountId ||
                                     session.primaryAccounts.submissionAccountId == accountId) ||
                                            session.primaryAccounts.contactsAccountId ==
                                                accountId ||
                                            session.primaryAccounts.calendarsAccountId == accountId
                                        ? 1
                                        : 0);
            insertAccount.bindValue(QStringLiteral(":owner_account_id"),
                                    QString::fromStdString(storedAccounts.ownerAccountId));
            if (!insertAccount.exec())
            {
                database.rollback();
                return makeQueryError(QStringLiteral("Insert cached account"), insertAccount);
            }
        }

        QSqlQuery insertSession{database};
        insertSession.prepare(QStringLiteral(
            "INSERT INTO sessions ("
            "account_id, api_url, download_url, upload_url, event_source_url, state, username, "
            "has_core_capability, has_mail_capability, has_submission_capability, "
            "has_contacts_capability, has_calendars_capability, core_capabilities_json, "
            "primary_mail_account_id, "
            "primary_submission_account_id, primary_contacts_account_id, "
            "primary_calendars_account_id, websocket_url, "
            "websocket_supports_push"
            ") VALUES ("
            ":account_id, :api_url, :download_url, :upload_url, :event_source_url, :state, "
            ":username, :has_core_capability, :has_mail_capability, "
            ":has_submission_capability, :has_contacts_capability, :has_calendars_capability, "
            ":core_capabilities_json, :primary_mail_account_id, "
            ":primary_submission_account_id, :primary_contacts_account_id, "
            ":primary_calendars_account_id, :websocket_url, "
            ":websocket_supports_push)"));
        insertSession.bindValue(QStringLiteral(":account_id"),
                                QString::fromStdString(storedAccounts.ownerAccountId));
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
        insertSession.bindValue(QStringLiteral(":has_contacts_capability"),
                                session.capabilities.contacts ? 1 : 0);
        insertSession.bindValue(QStringLiteral(":has_calendars_capability"),
                                session.capabilities.calendars ? 1 : 0);
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
        insertSession.bindValue(
            QStringLiteral(":primary_contacts_account_id"),
            session.primaryAccounts.contactsAccountId.has_value()
                ? QVariant{QString::fromStdString(*session.primaryAccounts.contactsAccountId)}
                : QVariant{});
        insertSession.bindValue(
            QStringLiteral(":primary_calendars_account_id"),
            session.primaryAccounts.calendarsAccountId.has_value()
                ? QVariant{QString::fromStdString(*session.primaryAccounts.calendarsAccountId)}
                : QVariant{});
        insertSession.bindValue(
            QStringLiteral(":websocket_url"),
            session.capabilities.websocket.has_value()
                ? QVariant{QString::fromStdString(session.capabilities.websocket->url)}
                : QVariant{});
        insertSession.bindValue(QStringLiteral(":websocket_supports_push"),
                                session.capabilities.websocket.has_value() &&
                                        session.capabilities.websocket->supportsPush
                                    ? 1
                                    : 0);
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

        return storedAccounts;
    }

    std::variant<std::optional<javelin::jmap::api::Session>, DatabaseError>
    SessionRepository::load(const std::string_view ownerAccountId) const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        std::string ownerLocalAccountId{ownerAccountId};
        QSqlQuery ownerQuery{m_connection.database()};
        ownerQuery.prepare(
            QStringLiteral("SELECT COALESCE(owner_account_id,account_id) FROM accounts WHERE "
                           "account_id=:account_id"));
        ownerQuery.bindValue(QStringLiteral(":account_id"),
                             QString::fromStdString(std::string{ownerAccountId}));
        if (!ownerQuery.exec())
            return makeQueryError(QStringLiteral("Resolve cached session owner"), ownerQuery);
        if (ownerQuery.next())
            ownerLocalAccountId = ownerQuery.value(0).toString().toStdString();

        QSqlQuery sessionQuery{m_connection.database()};
        sessionQuery.prepare(QStringLiteral(
            "SELECT api_url, download_url, upload_url, event_source_url, state, username, "
            "has_core_capability, has_mail_capability, has_submission_capability, "
            "has_contacts_capability, has_calendars_capability, core_capabilities_json, "
            "primary_mail_account_id, primary_submission_account_id, "
            "primary_contacts_account_id, primary_calendars_account_id, websocket_url, "
            "websocket_supports_push "
            "FROM sessions WHERE account_id = :account_id"));
        sessionQuery.bindValue(QStringLiteral(":account_id"),
                               QString::fromStdString(ownerLocalAccountId));
        if (!sessionQuery.exec())
        {
            return makeQueryError(QStringLiteral("Read cached session"), sessionQuery);
        }

        if (!sessionQuery.next())
        {
            return std::optional<javelin::jmap::api::Session>{std::nullopt};
        }

        javelin::jmap::api::Session session{};
        session.username = sessionQuery.value(5).toString().toStdString();
        session.apiUrl = sessionQuery.value(0).toString().toStdString();
        session.downloadUrl = sessionQuery.value(1).toString().toStdString();
        session.uploadUrl = sessionQuery.value(2).toString().toStdString();
        if (!sessionQuery.value(3).isNull())
        {
            session.eventSourceUrl = sessionQuery.value(3).toString().toStdString();
        }
        session.state = sessionQuery.value(4).toString().toStdString();
        session.capabilities.core = sessionQuery.value(6).toInt() != 0;
        session.capabilities.coreDetails =
            deserializeCoreCapability(sessionQuery.value(11).toString());
        session.capabilities.mail = sessionQuery.value(7).toInt() != 0;
        session.capabilities.submission = sessionQuery.value(8).toInt() != 0;
        session.capabilities.contacts = sessionQuery.value(9).toInt() != 0;
        session.capabilities.calendars = sessionQuery.value(10).toInt() != 0;
        if (!sessionQuery.value(16).isNull())
        {
            session.capabilities.websocket = javelin::jmap::api::WebSocketCapability{
                .url = sessionQuery.value(16).toString().toStdString(),
                .supportsPush = sessionQuery.value(17).toInt() != 0,
            };
        }
        if (!sessionQuery.value(12).isNull())
        {
            session.primaryAccounts.mailAccountId = sessionQuery.value(12).toString().toStdString();
        }
        if (!sessionQuery.value(13).isNull())
        {
            session.primaryAccounts.submissionAccountId =
                sessionQuery.value(13).toString().toStdString();
        }
        if (!sessionQuery.value(14).isNull())
        {
            session.primaryAccounts.contactsAccountId =
                sessionQuery.value(14).toString().toStdString();
        }
        if (!sessionQuery.value(15).isNull())
        {
            session.primaryAccounts.calendarsAccountId =
                sessionQuery.value(15).toString().toStdString();
        }

        QSqlQuery accountQuery{m_connection.database()};
        accountQuery.prepare(QStringLiteral(
            "SELECT COALESCE(remote_account_id,account_id), name, is_personal, is_read_only, "
            "cap_mail, mail_may_create_top_level_mailbox, cap_submission, "
            "submission_max_delayed_send, cap_contacts, contacts_capabilities_json, cap_calendars, "
            "calendars_capabilities_json FROM accounts WHERE owner_account_id = "
            ":owner_account_id ORDER BY account_id"));
        accountQuery.bindValue(QStringLiteral(":owner_account_id"),
                               QString::fromStdString(ownerLocalAccountId));
        if (!accountQuery.exec())
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
                        .mailDetails =
                            accountQuery.value(4).toInt() != 0
                                ? std::optional{javelin::jmap::api::MailAccountCapability{
                                      .mayCreateTopLevelMailbox =
                                          accountQuery.value(5).toInt() != 0,
                                  }}
                                : std::nullopt,
                        .submission = std::nullopt,
                        .contacts = std::nullopt,
                        .calendars = std::nullopt,
                    },
            };
            if (accountQuery.value(6).toInt() != 0)
            {
                account.accountCapabilities.submission = javelin::jmap::api::SubmissionCapability{
                    .maxDelayedSend = accountQuery.value(7).toULongLong(),
                };
            }
            if (accountQuery.value(8).toInt() != 0)
            {
                detail::RawContactsCapability raw;
                auto json = accountQuery.value(9).toString().toStdString();
                if (!glz::read<glz::opts{.error_on_unknown_keys = false}>(raw, json))
                {
                    account.accountCapabilities.contacts = javelin::jmap::api::ContactsCapability{
                        .maxAddressBooksPerCard = raw.maxAddressBooksPerCard,
                        .mayCreateAddressBook = raw.mayCreateAddressBook,
                    };
                }
            }
            if (accountQuery.value(10).toInt() != 0)
            {
                detail::RawCalendarsCapability raw;
                auto json = accountQuery.value(11).toString().toStdString();
                if (!glz::read<glz::opts{.error_on_unknown_keys = false}>(raw, json))
                {
                    account.accountCapabilities.calendars = javelin::jmap::api::CalendarsCapability{
                        .maxCalendarsPerEvent = raw.maxCalendarsPerEvent,
                        .minDateTime = std::move(raw.minDateTime),
                        .maxDateTime = std::move(raw.maxDateTime),
                        .maxExpandedQueryDuration = std::move(raw.maxExpandedQueryDuration),
                        .maxParticipantsPerEvent = raw.maxParticipantsPerEvent,
                        .mayCreateCalendar = raw.mayCreateCalendar,
                    };
                }
            }
            session.accounts.emplace(account.id, std::move(account));
        }

        return std::optional<javelin::jmap::api::Session>{std::move(session)};
    }

} // namespace javelin::jmap::cache
