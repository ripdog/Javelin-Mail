#include "jmap/cache/ComposeSessionRepository.h"
#include "jmap/submission/DraftSnapshotSerialization.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>

#include <cstdint>

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

        [[nodiscard]] QJsonObject
        serializeEmailAddress(const javelin::jmap::domain::EmailAddress& address)
        {
            QJsonObject object;
            if (address.name.has_value())
            {
                object.insert(QStringLiteral("name"), QString::fromStdString(*address.name));
            }
            object.insert(QStringLiteral("email"), QString::fromStdString(address.email));
            return object;
        }

        [[nodiscard]] std::optional<javelin::jmap::domain::EmailAddress>
        deserializeEmailAddress(const QJsonValue& value)
        {
            if (!value.isObject())
            {
                return std::nullopt;
            }

            const auto object = value.toObject();
            const auto email = object.value(QStringLiteral("email")).toString();
            if (email.isEmpty())
            {
                return std::nullopt;
            }

            const auto name = object.value(QStringLiteral("name")).toString();
            return javelin::jmap::domain::EmailAddress{
                .name =
                    name.isEmpty() ? std::nullopt : std::optional<std::string>{name.toStdString()},
                .email = email.toStdString(),
            };
        }

        [[nodiscard]] QJsonArray
        serializeAddresses(const std::vector<javelin::jmap::domain::EmailAddress>& addresses)
        {
            QJsonArray array;
            for (const auto& address : addresses)
            {
                array.push_back(serializeEmailAddress(address));
            }
            return array;
        }

        [[nodiscard]] std::vector<javelin::jmap::domain::EmailAddress>
        deserializeAddresses(const QJsonValue& value)
        {
            std::vector<javelin::jmap::domain::EmailAddress> addresses;
            if (!value.isArray())
            {
                return addresses;
            }

            const auto array = value.toArray();
            addresses.reserve(static_cast<std::size_t>(array.size()));
            for (const auto& item : array)
            {
                if (const auto address = deserializeEmailAddress(item))
                {
                    addresses.push_back(*address);
                }
            }

            return addresses;
        }

        [[nodiscard]] QJsonArray serializeStrings(const std::vector<std::string>& values)
        {
            QJsonArray array;
            for (const auto& value : values)
            {
                array.push_back(QString::fromStdString(value));
            }
            return array;
        }

        [[nodiscard]] std::vector<std::string> deserializeStrings(const QJsonValue& value)
        {
            std::vector<std::string> values;
            if (!value.isArray())
            {
                return values;
            }

            const auto array = value.toArray();
            values.reserve(static_cast<std::size_t>(array.size()));
            for (const auto& item : array)
            {
                const auto stringValue = item.toString();
                if (!stringValue.isEmpty())
                {
                    values.push_back(stringValue.toStdString());
                }
            }
            return values;
        }

        [[nodiscard]] QJsonArray serializeAttachments(
            const std::vector<javelin::jmap::submission::DraftAttachment>& attachments)
        {
            QJsonArray array;
            for (const auto& attachment : attachments)
            {
                QJsonObject object;
                object.insert(QStringLiteral("localFilePath"),
                              QString::fromStdString(attachment.localFilePath));
                object.insert(QStringLiteral("displayName"),
                              QString::fromStdString(attachment.displayName));
                object.insert(QStringLiteral("mediaType"),
                              QString::fromStdString(attachment.mediaType));
                object.insert(QStringLiteral("size"), static_cast<qint64>(attachment.size));
                if (attachment.blobId.has_value())
                {
                    object.insert(QStringLiteral("blobId"),
                                  QString::fromStdString(*attachment.blobId));
                }
                object.insert(QStringLiteral("inlineDisposition"), attachment.inlineDisposition);
                if (attachment.contentId.has_value())
                {
                    object.insert(QStringLiteral("contentId"),
                                  QString::fromStdString(*attachment.contentId));
                }
                if (attachment.contentHash.has_value())
                {
                    object.insert(QStringLiteral("contentHash"),
                                  QString::fromStdString(*attachment.contentHash));
                }
                array.push_back(object);
            }
            return array;
        }

        [[nodiscard]] std::vector<javelin::jmap::submission::DraftAttachment>
        deserializeAttachments(const QJsonValue& value)
        {
            std::vector<javelin::jmap::submission::DraftAttachment> attachments;
            if (!value.isArray())
            {
                return attachments;
            }

            const auto array = value.toArray();
            attachments.reserve(static_cast<std::size_t>(array.size()));
            for (const auto& item : array)
            {
                if (!item.isObject())
                {
                    continue;
                }

                const auto object = item.toObject();
                attachments.push_back(javelin::jmap::submission::DraftAttachment{
                    .localFilePath =
                        object.value(QStringLiteral("localFilePath")).toString().toStdString(),
                    .displayName =
                        object.value(QStringLiteral("displayName")).toString().toStdString(),
                    .mediaType = object.value(QStringLiteral("mediaType")).toString().toStdString(),
                    .size = static_cast<std::uint64_t>(
                        object.value(QStringLiteral("size")).toInteger(0)),
                    .blobId =
                        object.value(QStringLiteral("blobId")).isUndefined()
                            ? std::nullopt
                            : std::optional<std::string>{object.value(QStringLiteral("blobId"))
                                                             .toString()
                                                             .toStdString()},
                    .inlineDisposition =
                        object.value(QStringLiteral("inlineDisposition")).toBool(false),
                    .contentId =
                        object.value(QStringLiteral("contentId")).isUndefined()
                            ? std::nullopt
                            : std::optional<std::string>{object.value(QStringLiteral("contentId"))
                                                             .toString()
                                                             .toStdString()},
                    .contentHash =
                        object.value(QStringLiteral("contentHash")).isUndefined()
                            ? std::nullopt
                            : std::optional<std::string>{object.value(QStringLiteral("contentHash"))
                                                             .toString()
                                                             .toStdString()},
                });
            }

            return attachments;
        }

        [[nodiscard]] QString
        serializeSnapshot(const javelin::jmap::submission::DraftSnapshot& snapshot)
        {
            QJsonObject object;
            object.insert(QStringLiteral("composeSessionId"),
                          QString::fromStdString(snapshot.composeSessionId));
            object.insert(QStringLiteral("accountId"), QString::fromStdString(snapshot.accountId));
            object.insert(QStringLiteral("revision"), static_cast<qint64>(snapshot.revision));
            if (snapshot.draftEmailId.has_value())
            {
                object.insert(QStringLiteral("draftEmailId"),
                              QString::fromStdString(*snapshot.draftEmailId));
            }
            object.insert(QStringLiteral("mode"),
                          QString::fromStdString(
                              std::string{javelin::jmap::submission::toString(snapshot.mode)}));
            object.insert(QStringLiteral("editorMode"),
                          QString::fromStdString(std::string{
                              javelin::jmap::submission::toString(snapshot.editorMode)}));
            object.insert(QStringLiteral("identityId"),
                          QString::fromStdString(snapshot.identityId));
            object.insert(QStringLiteral("to"), serializeAddresses(snapshot.to));
            object.insert(QStringLiteral("cc"), serializeAddresses(snapshot.cc));
            object.insert(QStringLiteral("bcc"), serializeAddresses(snapshot.bcc));
            if (snapshot.subject.has_value())
            {
                object.insert(QStringLiteral("subject"), QString::fromStdString(*snapshot.subject));
            }
            object.insert(QStringLiteral("plainTextBody"),
                          QString::fromStdString(snapshot.plainTextBody));
            object.insert(QStringLiteral("htmlBody"), QString::fromStdString(snapshot.htmlBody));
            QJsonObject threading;
            threading.insert(QStringLiteral("messageId"),
                             serializeStrings(snapshot.threading.messageId));
            threading.insert(QStringLiteral("inReplyTo"),
                             serializeStrings(snapshot.threading.inReplyTo));
            threading.insert(QStringLiteral("references"),
                             serializeStrings(snapshot.threading.references));
            object.insert(QStringLiteral("threading"), threading);
            object.insert(QStringLiteral("attachments"),
                          serializeAttachments(snapshot.attachments));
            return QString::fromUtf8(QJsonDocument{object}.toJson(QJsonDocument::Compact));
        }

        [[nodiscard]] std::optional<javelin::jmap::submission::DraftSnapshot>
        deserializeSnapshot(const QString& json)
        {
            const auto document = QJsonDocument::fromJson(json.toUtf8());
            if (!document.isObject())
            {
                return std::nullopt;
            }

            const auto object = document.object();
            const auto composeSessionId =
                object.value(QStringLiteral("composeSessionId")).toString();
            const auto accountId = object.value(QStringLiteral("accountId")).toString();
            const auto identityId = object.value(QStringLiteral("identityId")).toString();
            const auto modeValue = object.value(QStringLiteral("mode")).toString();
            const auto editorModeValue = object.value(QStringLiteral("editorMode")).toString();
            const auto mode =
                javelin::jmap::submission::composeModeFromString(modeValue.toStdString());
            const auto editorMode =
                javelin::jmap::submission::bodyEditorModeFromString(editorModeValue.toStdString());
            if (composeSessionId.isEmpty() || accountId.isEmpty() || identityId.isEmpty() ||
                !mode.has_value() || !editorMode.has_value())
            {
                return std::nullopt;
            }

            const auto threadingObject = object.value(QStringLiteral("threading")).toObject();
            return javelin::jmap::submission::DraftSnapshot{
                .composeSessionId = composeSessionId.toStdString(),
                .accountId = accountId.toStdString(),
                .revision = static_cast<std::uint64_t>(
                    object.value(QStringLiteral("revision")).toInteger(1)),
                .draftEmailId =
                    object.value(QStringLiteral("draftEmailId")).isUndefined()
                        ? std::nullopt
                        : std::optional<std::string>{object.value(QStringLiteral("draftEmailId"))
                                                         .toString()
                                                         .toStdString()},
                .mode = *mode,
                .editorMode = *editorMode,
                .identityId = identityId.toStdString(),
                .to = deserializeAddresses(object.value(QStringLiteral("to"))),
                .cc = deserializeAddresses(object.value(QStringLiteral("cc"))),
                .bcc = deserializeAddresses(object.value(QStringLiteral("bcc"))),
                .subject = object.value(QStringLiteral("subject")).isUndefined()
                               ? std::nullopt
                               : std::optional<std::string>{object.value(QStringLiteral("subject"))
                                                                .toString()
                                                                .toStdString()},
                .plainTextBody =
                    object.value(QStringLiteral("plainTextBody")).toString().toStdString(),
                .htmlBody = object.value(QStringLiteral("htmlBody")).toString().toStdString(),
                .threading =
                    {
                        .messageId =
                            deserializeStrings(threadingObject.value(QStringLiteral("messageId"))),
                        .inReplyTo =
                            deserializeStrings(threadingObject.value(QStringLiteral("inReplyTo"))),
                        .references =
                            deserializeStrings(threadingObject.value(QStringLiteral("references"))),
                    },
                .attachments = deserializeAttachments(object.value(QStringLiteral("attachments"))),
            };
        }

    } // namespace

} // namespace javelin::jmap::cache

namespace javelin::jmap::submission
{
    QString serializeDraftSnapshot(const DraftSnapshot& snapshot)
    {
        return javelin::jmap::cache::serializeSnapshot(snapshot);
    }

    std::optional<DraftSnapshot> deserializeDraftSnapshot(const QString& json)
    {
        return javelin::jmap::cache::deserializeSnapshot(json);
    }
} // namespace javelin::jmap::submission

namespace javelin::jmap::cache
{

    ComposeSessionRepository::ComposeSessionRepository(DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    std::optional<DatabaseError>
    ComposeSessionRepository::upsert(const javelin::jmap::submission::DraftSnapshot& snapshot)
    {
        if (const auto error = m_connection.validate())
        {
            return error;
        }
        auto transactionResult =
            DatabaseTransaction::begin(m_connection, QStringLiteral("Store compose session"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));
        if (const auto error = upsert(transaction, snapshot))
            return error;
        return transaction.commit();
    }

    std::optional<DatabaseError>
    ComposeSessionRepository::upsert(DatabaseTransaction& transaction,
                                     const javelin::jmap::submission::DraftSnapshot& snapshot)
    {
        if (const auto error = m_connection.validate())
            return error;
        if (!transaction.isActive() || &transaction.connection() != &m_connection)
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Compose update requires a matching transaction"),
            };
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "INSERT INTO compose_sessions ("
            "compose_session_id, account_id, draft_email_id, mode, editor_mode, snapshot_json, "
            "last_saved_at, updated_at"
            ") VALUES ("
            ":compose_session_id, :account_id, :draft_email_id, :mode, :editor_mode, "
            ":snapshot_json, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP"
            ") ON CONFLICT(compose_session_id) DO UPDATE SET "
            "account_id = excluded.account_id, "
            "draft_email_id = excluded.draft_email_id, "
            "mode = excluded.mode, "
            "editor_mode = excluded.editor_mode, "
            "snapshot_json = excluded.snapshot_json, "
            "last_saved_at = excluded.last_saved_at, "
            "updated_at = CURRENT_TIMESTAMP"));
        query.bindValue(QStringLiteral(":compose_session_id"),
                        QString::fromStdString(snapshot.composeSessionId));
        query.bindValue(QStringLiteral(":account_id"), QString::fromStdString(snapshot.accountId));
        query.bindValue(QStringLiteral(":draft_email_id"),
                        snapshot.draftEmailId.has_value()
                            ? QVariant{QString::fromStdString(*snapshot.draftEmailId)}
                            : QVariant{});
        query.bindValue(QStringLiteral(":mode"),
                        QString::fromStdString(
                            std::string{javelin::jmap::submission::toString(snapshot.mode)}));
        query.bindValue(QStringLiteral(":editor_mode"),
                        QString::fromStdString(
                            std::string{javelin::jmap::submission::toString(snapshot.editorMode)}));
        query.bindValue(QStringLiteral(":snapshot_json"),
                        javelin::jmap::submission::serializeDraftSnapshot(snapshot));
        if (!query.exec())
        {
            return makeQueryError(QStringLiteral("Upsert compose session"), query);
        }

        return std::nullopt;
    }

    std::variant<std::optional<javelin::jmap::submission::DraftSnapshot>, DatabaseError>
    ComposeSessionRepository::find(const std::string_view composeSessionId) const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(
            QStringLiteral("SELECT snapshot_json FROM compose_sessions WHERE compose_session_id = "
                           ":compose_session_id"));
        query.bindValue(QStringLiteral(":compose_session_id"),
                        QString::fromStdString(std::string{composeSessionId}));
        if (!query.exec())
        {
            return makeQueryError(QStringLiteral("Read compose session"), query);
        }

        if (!query.next())
        {
            return std::optional<javelin::jmap::submission::DraftSnapshot>{std::nullopt};
        }

        return javelin::jmap::submission::deserializeDraftSnapshot(query.value(0).toString());
    }

    std::variant<std::vector<javelin::jmap::submission::DraftSnapshot>, DatabaseError>
    ComposeSessionRepository::listByAccount(const std::string_view accountId) const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT snapshot_json FROM compose_sessions WHERE account_id = :account_id "
            "ORDER BY updated_at DESC"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        if (!query.exec())
        {
            return makeQueryError(QStringLiteral("Read compose sessions"), query);
        }

        std::vector<javelin::jmap::submission::DraftSnapshot> snapshots;
        while (query.next())
        {
            if (const auto snapshot =
                    javelin::jmap::submission::deserializeDraftSnapshot(query.value(0).toString()))
            {
                snapshots.push_back(*snapshot);
            }
        }
        return snapshots;
    }

    std::optional<DatabaseError>
    ComposeSessionRepository::remove(const std::string_view composeSessionId)
    {
        if (const auto error = m_connection.validate())
        {
            return error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "DELETE FROM compose_sessions WHERE compose_session_id = :compose_session_id"));
        query.bindValue(QStringLiteral(":compose_session_id"),
                        QString::fromStdString(std::string{composeSessionId}));
        if (!query.exec())
        {
            return makeQueryError(QStringLiteral("Delete compose session"), query);
        }

        return std::nullopt;
    }

} // namespace javelin::jmap::cache
