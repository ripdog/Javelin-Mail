#include "protocol/SocketWireCodecInternal.h"

#include <QDataStream>

#include <algorithm>
#include <limits>
#include <type_traits>
#include <utility>

namespace javelin::protocol
{
    namespace
    {
        [[nodiscard]] SocketFrameError malformed(QString detail)
        {
            return {.code = SocketFrameErrorCode::MalformedPayload, .detail = std::move(detail)};
        }

        class PayloadWriter final
        {
          public:
            explicit PayloadWriter(const BoundaryLimits limits)
                : m_limits(limits), m_stream(&m_payload, QIODeviceBase::WriteOnly)
            {
                m_stream.setVersion(QDataStream::Qt_6_6);
                m_stream.setByteOrder(QDataStream::BigEndian);
            }

            bool byte(const quint8 value)
            {
                if (m_error.has_value())
                    return false;
                m_stream << value;
                return checkStream();
            }

            bool word(const quint16 value)
            {
                if (m_error.has_value())
                    return false;
                m_stream << value;
                return checkStream();
            }

            bool dword(const quint32 value)
            {
                if (m_error.has_value())
                    return false;
                m_stream << value;
                return checkStream();
            }

            bool qword(const quint64 value)
            {
                if (m_error.has_value())
                    return false;
                m_stream << value;
                return checkStream();
            }

            bool integer(const qint32 value)
            {
                if (m_error.has_value())
                    return false;
                m_stream << value;
                return checkStream();
            }

            bool boolean(const bool value)
            {
                return byte(value ? 1 : 0);
            }

            bool uuid(const QUuid& value)
            {
                if (m_error.has_value())
                    return false;
                m_stream << value;
                return checkStream();
            }

            bool string(const QString& value)
            {
                if (static_cast<std::size_t>(value.toUtf8().size()) > m_limits.maximumStringBytes)
                    return fail(QStringLiteral("string exceeds the protocol limit"));
                if (m_error.has_value())
                    return false;
                m_stream << value;
                return checkStream();
            }

            bool bytes(const QByteArray& value)
            {
                if (static_cast<std::size_t>(value.size()) > m_limits.maximumFrameBytes)
                    return fail(QStringLiteral("byte array exceeds the protocol limit"));
                if (m_error.has_value())
                    return false;
                m_stream << value;
                return checkStream();
            }

            bool count(const std::size_t value, const std::size_t maximum, const QString& field)
            {
                if (value > maximum || value > std::numeric_limits<quint32>::max())
                    return fail(field + QStringLiteral(" exceeds the protocol limit"));
                return dword(static_cast<quint32>(value));
            }

            bool fail(QString detail)
            {
                if (!m_error.has_value())
                    m_error = malformed(std::move(detail));
                return false;
            }

            [[nodiscard]] std::variant<QByteArray, SocketFrameError> finish()
            {
                if (m_error.has_value())
                    return *m_error;
                if (!checkStream())
                    return *m_error;
                return std::move(m_payload);
            }

          private:
            bool checkStream()
            {
                if (m_stream.status() == QDataStream::Ok)
                    return true;
                return fail(QStringLiteral("could not encode the protocol payload"));
            }

            BoundaryLimits m_limits;
            QByteArray m_payload;
            QDataStream m_stream;
            std::optional<SocketFrameError> m_error;
        };

        class PayloadReader final
        {
          public:
            PayloadReader(QByteArray payload, const BoundaryLimits limits)
                : m_payload(std::move(payload)), m_limits(limits),
                  m_stream(&m_payload, QIODeviceBase::ReadOnly)
            {
                m_stream.setVersion(QDataStream::Qt_6_6);
                m_stream.setByteOrder(QDataStream::BigEndian);
            }

            bool byte(quint8& value)
            {
                m_stream >> value;
                return checkStream();
            }

            bool word(quint16& value)
            {
                m_stream >> value;
                return checkStream();
            }

            bool dword(quint32& value)
            {
                m_stream >> value;
                return checkStream();
            }

            bool qword(quint64& value)
            {
                m_stream >> value;
                return checkStream();
            }

            bool qword(std::uint64_t& value)
            {
                quint64 encoded = 0;
                if (!qword(encoded))
                    return false;
                value = static_cast<std::uint64_t>(encoded);
                return true;
            }

            bool integer(qint32& value)
            {
                m_stream >> value;
                return checkStream();
            }

            bool boolean(bool& value)
            {
                quint8 encoded = 0;
                if (!byte(encoded))
                    return false;
                if (encoded > 1)
                    return fail(QStringLiteral("boolean value is invalid"));
                value = encoded != 0;
                return true;
            }

            bool uuid(QUuid& value)
            {
                m_stream >> value;
                return checkStream();
            }

            bool string(QString& value)
            {
                m_stream >> value;
                if (!checkStream())
                    return false;
                if (static_cast<std::size_t>(value.toUtf8().size()) > m_limits.maximumStringBytes)
                    return fail(QStringLiteral("string exceeds the protocol limit"));
                return true;
            }

            bool bytes(QByteArray& value)
            {
                m_stream >> value;
                if (!checkStream())
                    return false;
                if (static_cast<std::size_t>(value.size()) > m_limits.maximumFrameBytes)
                    return fail(QStringLiteral("byte array exceeds the protocol limit"));
                return true;
            }

            bool count(quint32& value, const std::size_t maximum, const QString& field)
            {
                if (!dword(value))
                    return false;
                if (value > maximum)
                    return fail(field + QStringLiteral(" exceeds the protocol limit"));
                return true;
            }

            bool fail(QString detail)
            {
                if (!m_error.has_value())
                    m_error = malformed(std::move(detail));
                return false;
            }

            [[nodiscard]] std::optional<SocketFrameError> finish()
            {
                if (m_error.has_value())
                    return m_error;
                if (!checkStream())
                    return m_error;
                if (!m_stream.atEnd())
                    return malformed(QStringLiteral("protocol payload has trailing bytes"));
                return std::nullopt;
            }

          private:
            bool checkStream()
            {
                if (m_stream.status() == QDataStream::Ok)
                    return true;
                return fail(QStringLiteral("could not decode the protocol payload"));
            }

            QByteArray m_payload;
            BoundaryLimits m_limits;
            QDataStream m_stream;
            std::optional<SocketFrameError> m_error;
        };

        template <typename Enum> bool writeEnum(PayloadWriter& writer, const Enum value)
        {
            return writer.byte(static_cast<quint8>(value));
        }

        template <typename Enum>
        bool readEnum(PayloadReader& reader, Enum& value, const quint8 maximum)
        {
            quint8 encoded = 0;
            if (!reader.byte(encoded))
                return false;
            if (encoded > maximum)
                return reader.fail(QStringLiteral("enum value is invalid"));
            value = static_cast<Enum>(encoded);
            return true;
        }

        template <typename T, typename WriteItem>
        bool writeVector(PayloadWriter& writer, const std::vector<T>& values,
                         const std::size_t maximum, const QString& field, WriteItem writeItem)
        {
            if (!writer.count(values.size(), maximum, field))
                return false;
            for (const auto& value : values)
            {
                if (!writeItem(value))
                    return false;
            }
            return true;
        }

        template <typename T, typename ReadItem>
        bool readVector(PayloadReader& reader, std::vector<T>& values, const std::size_t maximum,
                        const QString& field, ReadItem readItem)
        {
            quint32 count = 0;
            if (!reader.count(count, maximum, field))
                return false;
            values.clear();
            values.reserve(count);
            for (quint32 index = 0; index < count; ++index)
            {
                T value;
                if (!readItem(value))
                    return false;
                values.push_back(std::move(value));
            }
            return true;
        }

        bool writeBoundaryError(PayloadWriter& writer, const BoundaryError& error)
        {
            return writeEnum(writer, error.code) && writer.string(error.field) &&
                   writer.string(error.detail);
        }

        bool readBoundaryError(PayloadReader& reader, BoundaryError& error)
        {
            if (!readEnum(reader, error.code,
                          static_cast<quint8>(BoundaryErrorCode::ProtocolViolation)))
                return false;
            return reader.string(error.field) && reader.string(error.detail);
        }

        bool writeProtocol(PayloadWriter& writer, const ProtocolVersion& protocol)
        {
            return writer.word(protocol.major) && writer.word(protocol.minor);
        }

        bool readProtocol(PayloadReader& reader, ProtocolVersion& protocol)
        {
            return reader.word(protocol.major) && reader.word(protocol.minor);
        }

        bool writeBuild(PayloadWriter& writer, const BuildIdentity& build)
        {
            return writer.string(build.application) && writer.string(build.revision);
        }

        bool readBuild(PayloadReader& reader, BuildIdentity& build)
        {
            return reader.string(build.application) && reader.string(build.revision);
        }

        bool writeCacheIdentity(PayloadWriter& writer, const CacheIdentity& cache)
        {
            return writer.uuid(cache.instance.value) && writer.dword(cache.schema.value) &&
                   writer.qword(cache.dataVersion.value);
        }

        bool readCacheIdentity(PayloadReader& reader, CacheIdentity& cache)
        {
            return reader.uuid(cache.instance.value) && reader.dword(cache.schema.value) &&
                   reader.qword(cache.dataVersion.value);
        }

        bool writeAccountSettings(PayloadWriter& writer, const AccountSettings& account,
                                  const BoundaryLimits& limits)
        {
            return writer.string(account.id) && writer.qword(account.revision) &&
                   writer.string(account.displayName) && writer.string(account.sessionUrl) &&
                   writer.string(account.loginEmail) && writer.string(account.tokenEndpoint) &&
                   writer.string(account.oauthClientId) && writer.string(account.oauthIssuer) &&
                   writer.string(account.oauthResource) && writer.string(account.oauthScope) &&
                   writer.string(account.revocationEndpoint) &&
                   writer.string(account.registrationClientUri) &&
                   writer.boolean(account.hasCredentials) &&
                   writer.string(account.credentialHandle) &&
                   writer.qword(static_cast<quint64>(account.tokenExpiresAtEpochSeconds)) &&
                   writer.boolean(account.reauthenticationRequired) &&
                   writeVector(writer, account.cachedAccountIds, limits.maximumCollectionItems,
                               QStringLiteral("account.cachedAccountIds"),
                               [&writer](const QString& value) { return writer.string(value); });
        }

        bool readAccountSettings(PayloadReader& reader, AccountSettings& account,
                                 const BoundaryLimits& limits)
        {
            quint64 expiresAt = 0;
            if (!reader.string(account.id) || !reader.qword(account.revision) ||
                !reader.string(account.displayName) || !reader.string(account.sessionUrl) ||
                !reader.string(account.loginEmail) || !reader.string(account.tokenEndpoint) ||
                !reader.string(account.oauthClientId) || !reader.string(account.oauthIssuer) ||
                !reader.string(account.oauthResource) || !reader.string(account.oauthScope) ||
                !reader.string(account.revocationEndpoint) ||
                !reader.string(account.registrationClientUri) ||
                !reader.boolean(account.hasCredentials) ||
                !reader.string(account.credentialHandle) || !reader.qword(expiresAt) ||
                !reader.boolean(account.reauthenticationRequired) ||
                expiresAt > static_cast<quint64>(std::numeric_limits<qint64>::max()))
                return false;
            account.tokenExpiresAtEpochSeconds = static_cast<qint64>(expiresAt);
            return readVector(reader, account.cachedAccountIds, limits.maximumCollectionItems,
                              QStringLiteral("account.cachedAccountIds"),
                              [&reader](QString& value) { return reader.string(value); });
        }

        bool writeMailboxSelection(PayloadWriter& writer, const MailboxSelectionSettings& selection,
                                   const BoundaryLimits& limits)
        {
            return writer.string(selection.accountId) &&
                   writeVector(writer, selection.mailboxIds, limits.maximumCollectionItems,
                               QStringLiteral("mailboxSelection.mailboxIds"),
                               [&writer](const QString& value) { return writer.string(value); });
        }

        bool readMailboxSelection(PayloadReader& reader, MailboxSelectionSettings& selection,
                                  const BoundaryLimits& limits)
        {
            return reader.string(selection.accountId) &&
                   readVector(reader, selection.mailboxIds, limits.maximumCollectionItems,
                              QStringLiteral("mailboxSelection.mailboxIds"),
                              [&reader](QString& value) { return reader.string(value); });
        }

        bool writeWorkspaceSettings(PayloadWriter& writer, const WorkspaceSettings& workspace,
                                    const BoundaryLimits& limits)
        {
            return writer.dword(workspace.formatVersion) &&
                   writer.bytes(workspace.mainWindowState) &&
                   writer.boolean(workspace.composeRichTextDefault) &&
                   writer.string(workspace.defaultCalendarDestination.ownerAccountId) &&
                   writer.string(workspace.defaultCalendarDestination.accountId) &&
                   writer.string(workspace.defaultCalendarDestination.calendarId) &&
                   writeVector(writer, workspace.emailContextMenuLayout,
                               limits.maximumCollectionItems,
                               QStringLiteral("workspace.emailContextMenuLayout"),
                               [&writer](const QString& value) { return writer.string(value); }) &&
                   writeVector(writer, workspace.calendarEventContextMenuLayout,
                               limits.maximumCollectionItems,
                               QStringLiteral("workspace.calendarEventContextMenuLayout"),
                               [&writer](const QString& value) { return writer.string(value); });
        }

        bool readWorkspaceSettings(PayloadReader& reader, WorkspaceSettings& workspace,
                                   const BoundaryLimits& limits)
        {
            if (!reader.dword(workspace.formatVersion) ||
                !reader.bytes(workspace.mainWindowState) ||
                !reader.boolean(workspace.composeRichTextDefault) ||
                !reader.string(workspace.defaultCalendarDestination.ownerAccountId) ||
                !reader.string(workspace.defaultCalendarDestination.accountId) ||
                !reader.string(workspace.defaultCalendarDestination.calendarId) ||
                static_cast<std::size_t>(workspace.mainWindowState.size()) >
                    limits.maximumWorkspaceBytes)
                return false;
            return readVector(reader, workspace.emailContextMenuLayout,
                              limits.maximumCollectionItems,
                              QStringLiteral("workspace.emailContextMenuLayout"),
                              [&reader](QString& value) { return reader.string(value); }) &&
                   readVector(reader, workspace.calendarEventContextMenuLayout,
                              limits.maximumCollectionItems,
                              QStringLiteral("workspace.calendarEventContextMenuLayout"),
                              [&reader](QString& value) { return reader.string(value); });
        }

        bool writeSettingsUpdate(PayloadWriter& writer, const SettingsUpdate& update,
                                 const BoundaryLimits& limits)
        {
            if (!writer.boolean(update.accounts.has_value()))
                return false;
            if (update.accounts.has_value() &&
                !writeVector(writer, *update.accounts, limits.maximumCollectionItems,
                             QStringLiteral("update.accounts"),
                             [&writer, &limits](const AccountSettings& value)
                             { return writeAccountSettings(writer, value, limits); }))
                return false;

            if (!writer.boolean(update.syncedMailboxSelections.has_value()))
                return false;
            if (update.syncedMailboxSelections.has_value() &&
                !writeVector(writer, *update.syncedMailboxSelections, limits.maximumCollectionItems,
                             QStringLiteral("update.syncedMailboxSelections"),
                             [&writer, &limits](const MailboxSelectionSettings& value)
                             { return writeMailboxSelection(writer, value, limits); }))
                return false;

            if (!writer.boolean(update.notificationMailboxSelections.has_value()))
                return false;
            if (update.notificationMailboxSelections.has_value() &&
                !writeVector(writer, *update.notificationMailboxSelections,
                             limits.maximumCollectionItems,
                             QStringLiteral("update.notificationMailboxSelections"),
                             [&writer, &limits](const MailboxSelectionSettings& value)
                             { return writeMailboxSelection(writer, value, limits); }))
                return false;

            const auto writeStringList =
                [&writer, &limits](const std::optional<std::vector<QString>>& values,
                                   const QString& field)
            {
                if (!writer.boolean(values.has_value()))
                    return false;
                return !values.has_value() ||
                       writeVector(writer, *values, limits.maximumCollectionItems, field,
                                   [&writer](const QString& value)
                                   { return writer.string(value); });
            };
            if (!writeStringList(update.remoteContentSenders,
                                 QStringLiteral("update.remoteContentSenders")) ||
                !writeStringList(update.remoteContentDomains,
                                 QStringLiteral("update.remoteContentDomains")))
                return false;

            if (!writer.boolean(update.appearance.has_value()))
                return false;
            if (update.appearance.has_value() &&
                !writer.integer(update.appearance->messageColorMode))
                return false;

            if (!writer.boolean(update.attachments.has_value()))
                return false;
            if (update.attachments.has_value() && !writer.boolean(update.attachments->alwaysAsk))
                return false;
            if (update.attachments.has_value() && !writer.string(update.attachments->directory))
                return false;

            if (!writer.boolean(update.undoSendDelaySeconds.has_value()))
                return false;
            if (update.undoSendDelaySeconds.has_value() &&
                !writer.integer(*update.undoSendDelaySeconds))
                return false;

            if (!writer.boolean(update.undoSendUsesDialog.has_value()))
                return false;
            if (update.undoSendUsesDialog.has_value() &&
                !writer.boolean(*update.undoSendUsesDialog))
                return false;

            if (!writer.boolean(update.workspace.has_value()))
                return false;
            return !update.workspace.has_value() ||
                   writeWorkspaceSettings(writer, *update.workspace, limits);
        }

        bool readSettingsUpdate(PayloadReader& reader, SettingsUpdate& update,
                                const BoundaryLimits& limits)
        {
            bool present = false;
            if (!reader.boolean(present))
                return false;
            if (present)
            {
                update.accounts.emplace();
                if (!readVector(reader, *update.accounts, limits.maximumCollectionItems,
                                QStringLiteral("update.accounts"),
                                [&reader, &limits](AccountSettings& value)
                                { return readAccountSettings(reader, value, limits); }))
                    return false;
            }

            if (!reader.boolean(present))
                return false;
            if (present)
            {
                update.syncedMailboxSelections.emplace();
                if (!readVector(reader, *update.syncedMailboxSelections,
                                limits.maximumCollectionItems,
                                QStringLiteral("update.syncedMailboxSelections"),
                                [&reader, &limits](MailboxSelectionSettings& value)
                                { return readMailboxSelection(reader, value, limits); }))
                    return false;
            }

            if (!reader.boolean(present))
                return false;
            if (present)
            {
                update.notificationMailboxSelections.emplace();
                if (!readVector(reader, *update.notificationMailboxSelections,
                                limits.maximumCollectionItems,
                                QStringLiteral("update.notificationMailboxSelections"),
                                [&reader, &limits](MailboxSelectionSettings& value)
                                { return readMailboxSelection(reader, value, limits); }))
                    return false;
            }

            const auto readStringList =
                [&reader, &limits](std::optional<std::vector<QString>>& values,
                                   const QString& field)
            {
                bool isPresent = false;
                if (!reader.boolean(isPresent))
                    return false;
                if (!isPresent)
                    return true;
                values.emplace();
                return readVector(reader, *values, limits.maximumCollectionItems, field,
                                  [&reader](QString& value) { return reader.string(value); });
            };
            if (!readStringList(update.remoteContentSenders,
                                QStringLiteral("update.remoteContentSenders")) ||
                !readStringList(update.remoteContentDomains,
                                QStringLiteral("update.remoteContentDomains")))
                return false;

            if (!reader.boolean(present))
                return false;
            if (present)
            {
                update.appearance.emplace();
                if (!reader.integer(update.appearance->messageColorMode))
                    return false;
            }

            if (!reader.boolean(present))
                return false;
            if (present)
            {
                update.attachments.emplace();
                if (!reader.boolean(update.attachments->alwaysAsk) ||
                    !reader.string(update.attachments->directory))
                    return false;
            }

            if (!reader.boolean(present))
                return false;
            if (present)
            {
                update.undoSendDelaySeconds.emplace();
                if (!reader.integer(*update.undoSendDelaySeconds))
                    return false;
            }

            if (!reader.boolean(present))
                return false;
            if (present)
            {
                update.undoSendUsesDialog.emplace();
                if (!reader.boolean(*update.undoSendUsesDialog))
                    return false;
            }

            if (!reader.boolean(present))
                return false;
            if (present)
            {
                update.workspace.emplace();
                if (!readWorkspaceSettings(reader, *update.workspace, limits))
                    return false;
            }
            return true;
        }

        bool writeStringVector(PayloadWriter& writer, const std::vector<QString>& values,
                               const std::size_t maximum, const QString& field)
        {
            return writeVector(writer, values, maximum, field,
                               [&writer](const QString& value) { return writer.string(value); });
        }

        bool readStringVector(PayloadReader& reader, std::vector<QString>& values,
                              const std::size_t maximum, const QString& field)
        {
            return readVector(reader, values, maximum, field,
                              [&reader](QString& value) { return reader.string(value); });
        }

        bool writeChangedDomains(PayloadWriter& writer, const std::vector<ChangedDomain>& domains,
                                 const BoundaryLimits& limits)
        {
            return writeVector(
                writer, domains, limits.maximumCollectionItems, QStringLiteral("changedDomains"),
                [&writer](const ChangedDomain domain) { return writeEnum(writer, domain); });
        }

        bool readChangedDomains(PayloadReader& reader, std::vector<ChangedDomain>& domains,
                                const BoundaryLimits& limits)
        {
            return readVector(
                reader, domains, limits.maximumCollectionItems, QStringLiteral("changedDomains"),
                [&reader](ChangedDomain& domain)
                {
                    return readEnum(reader, domain,
                                    static_cast<quint8>(ChangedDomain::UserVisibleFailures));
                });
        }

        bool writeAffectedKeys(PayloadWriter& writer, const std::vector<QString>& keys,
                               const BoundaryLimits& limits)
        {
            return writeStringVector(writer, keys, limits.maximumAffectedKeys,
                                     QStringLiteral("affectedKeys"));
        }

        bool readAffectedKeys(PayloadReader& reader, std::vector<QString>& keys,
                              const BoundaryLimits& limits)
        {
            return readStringVector(reader, keys, limits.maximumAffectedKeys,
                                    QStringLiteral("affectedKeys"));
        }

        bool writeOptionalSize(PayloadWriter& writer, const std::optional<std::uint64_t> value)
        {
            return writer.boolean(value.has_value()) &&
                   (!value.has_value() || writer.qword(*value));
        }

        bool readOptionalSize(PayloadReader& reader, std::optional<std::uint64_t>& value)
        {
            bool present = false;
            if (!reader.boolean(present))
                return false;
            if (!present)
            {
                value.reset();
                return true;
            }
            value.emplace();
            return reader.qword(*value);
        }

        bool writeMailboxWindowInvalidation(PayloadWriter& writer,
                                            const MailboxWindowInvalidation& window)
        {
            return writer.string(window.mailboxId) && writer.qword(window.offset) &&
                   writer.qword(window.limit) && writeOptionalSize(writer, window.total);
        }

        bool readMailboxWindowInvalidation(PayloadReader& reader, MailboxWindowInvalidation& window)
        {
            return reader.string(window.mailboxId) && reader.qword(window.offset) &&
                   reader.qword(window.limit) && readOptionalSize(reader, window.total);
        }

        bool writeSearchWindowInvalidation(PayloadWriter& writer,
                                           const SearchWindowInvalidation& window)
        {
            return writer.string(window.queryKey) && writer.qword(window.offset) &&
                   writer.qword(window.limit) && writeOptionalSize(writer, window.total);
        }

        bool readSearchWindowInvalidation(PayloadReader& reader, SearchWindowInvalidation& window)
        {
            return reader.string(window.queryKey) && reader.qword(window.offset) &&
                   reader.qword(window.limit) && readOptionalSize(reader, window.total);
        }

        bool writeCacheInvalidation(PayloadWriter& writer, const CacheInvalidation& invalidation,
                                    const BoundaryLimits& limits)
        {
            return writer.qword(invalidation.epoch.value) &&
                   writeChangedDomains(writer, invalidation.changedDomains, limits) &&
                   writeAffectedKeys(writer, invalidation.affectedKeys, limits) &&
                   writer.string(invalidation.accountId) &&
                   writeStringVector(writer, invalidation.mailboxIds, limits.maximumAffectedKeys,
                                     QStringLiteral("mailboxIds")) &&
                   writeStringVector(writer, invalidation.messageContentEmailIds,
                                     limits.maximumAffectedKeys,
                                     QStringLiteral("messageContentEmailIds")) &&
                   writeVector(writer, invalidation.mailboxWindows, limits.maximumCollectionItems,
                               QStringLiteral("mailboxWindows"),
                               [&writer](const MailboxWindowInvalidation& window)
                               { return writeMailboxWindowInvalidation(writer, window); }) &&
                   writeVector(writer, invalidation.searchWindows, limits.maximumCollectionItems,
                               QStringLiteral("searchWindows"),
                               [&writer](const SearchWindowInvalidation& window)
                               { return writeSearchWindowInvalidation(writer, window); });
        }

        bool readCacheInvalidation(PayloadReader& reader, CacheInvalidation& invalidation,
                                   const BoundaryLimits& limits)
        {
            return reader.qword(invalidation.epoch.value) &&
                   readChangedDomains(reader, invalidation.changedDomains, limits) &&
                   readAffectedKeys(reader, invalidation.affectedKeys, limits) &&
                   reader.string(invalidation.accountId) &&
                   readStringVector(reader, invalidation.mailboxIds, limits.maximumAffectedKeys,
                                    QStringLiteral("mailboxIds")) &&
                   readStringVector(reader, invalidation.messageContentEmailIds,
                                    limits.maximumAffectedKeys,
                                    QStringLiteral("messageContentEmailIds")) &&
                   readVector(reader, invalidation.mailboxWindows, limits.maximumCollectionItems,
                              QStringLiteral("mailboxWindows"),
                              [&reader](MailboxWindowInvalidation& window)
                              { return readMailboxWindowInvalidation(reader, window); }) &&
                   readVector(reader, invalidation.searchWindows, limits.maximumCollectionItems,
                              QStringLiteral("searchWindows"),
                              [&reader](SearchWindowInvalidation& window)
                              { return readSearchWindowInvalidation(reader, window); });
        }

        bool writeSettingsSnapshot(PayloadWriter& writer, const SettingsSnapshot& snapshot,
                                   const BoundaryLimits& limits)
        {
            return writer.qword(snapshot.revision.value) && writer.dword(snapshot.schemaVersion) &&
                   writeVector(writer, snapshot.accounts, limits.maximumCollectionItems,
                               QStringLiteral("snapshot.accounts"),
                               [&writer, &limits](const AccountSettings& value)
                               { return writeAccountSettings(writer, value, limits); }) &&
                   writeVector(writer, snapshot.syncedMailboxSelections,
                               limits.maximumCollectionItems,
                               QStringLiteral("snapshot.syncedMailboxSelections"),
                               [&writer, &limits](const MailboxSelectionSettings& value)
                               { return writeMailboxSelection(writer, value, limits); }) &&
                   writeVector(writer, snapshot.notificationMailboxSelections,
                               limits.maximumCollectionItems,
                               QStringLiteral("snapshot.notificationMailboxSelections"),
                               [&writer, &limits](const MailboxSelectionSettings& value)
                               { return writeMailboxSelection(writer, value, limits); }) &&
                   writeStringVector(writer, snapshot.remoteContentSenders,
                                     limits.maximumCollectionItems,
                                     QStringLiteral("snapshot.remoteContentSenders")) &&
                   writeStringVector(writer, snapshot.remoteContentDomains,
                                     limits.maximumCollectionItems,
                                     QStringLiteral("snapshot.remoteContentDomains")) &&
                   writer.integer(snapshot.appearance.messageColorMode) &&
                   writer.boolean(snapshot.attachments.alwaysAsk) &&
                   writer.string(snapshot.attachments.directory) &&
                   writer.integer(snapshot.undoSendDelaySeconds) &&
                   writer.boolean(snapshot.undoSendUsesDialog) &&
                   writeWorkspaceSettings(writer, snapshot.workspace, limits);
        }

        bool readSettingsSnapshot(PayloadReader& reader, SettingsSnapshot& snapshot,
                                  const BoundaryLimits& limits)
        {
            return reader.qword(snapshot.revision.value) && reader.dword(snapshot.schemaVersion) &&
                   readVector(reader, snapshot.accounts, limits.maximumCollectionItems,
                              QStringLiteral("snapshot.accounts"),
                              [&reader, &limits](AccountSettings& value)
                              { return readAccountSettings(reader, value, limits); }) &&
                   readVector(reader, snapshot.syncedMailboxSelections,
                              limits.maximumCollectionItems,
                              QStringLiteral("snapshot.syncedMailboxSelections"),
                              [&reader, &limits](MailboxSelectionSettings& value)
                              { return readMailboxSelection(reader, value, limits); }) &&
                   readVector(reader, snapshot.notificationMailboxSelections,
                              limits.maximumCollectionItems,
                              QStringLiteral("snapshot.notificationMailboxSelections"),
                              [&reader, &limits](MailboxSelectionSettings& value)
                              { return readMailboxSelection(reader, value, limits); }) &&
                   readStringVector(reader, snapshot.remoteContentSenders,
                                    limits.maximumCollectionItems,
                                    QStringLiteral("snapshot.remoteContentSenders")) &&
                   readStringVector(reader, snapshot.remoteContentDomains,
                                    limits.maximumCollectionItems,
                                    QStringLiteral("snapshot.remoteContentDomains")) &&
                   reader.integer(snapshot.appearance.messageColorMode) &&
                   reader.boolean(snapshot.attachments.alwaysAsk) &&
                   reader.string(snapshot.attachments.directory) &&
                   reader.integer(snapshot.undoSendDelaySeconds) &&
                   reader.boolean(snapshot.undoSendUsesDialog) &&
                   readWorkspaceSettings(reader, snapshot.workspace, limits);
        }

        struct EncodedPayload
        {
            SocketFrameKind kind;
            QByteArray payload;
        };

        using EncodedPayloadResult = std::variant<EncodedPayload, SocketFrameError>;

        template <typename Write>
        EncodedPayloadResult makePayload(const SocketFrameKind kind, const BoundaryLimits& limits,
                                         Write write)
        {
            PayloadWriter writer{limits};
            if (!write(writer))
                return malformed(QStringLiteral("could not encode the protocol payload"));
            auto result = writer.finish();
            if (auto* error = std::get_if<SocketFrameError>(&result))
                return *error;
            return EncodedPayload{.kind = kind, .payload = std::move(std::get<QByteArray>(result))};
        }

        EncodedPayloadResult encodeClientRequest(const ClientRequest& request,
                                                 const BoundaryLimits& limits)
        {
            return std::visit(
                [&limits](const auto& value) -> EncodedPayloadResult
                {
                    using Request = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<Request, HelloRequest>)
                    {
                        return makePayload(SocketFrameKind::HelloRequest, limits,
                                           [&value](PayloadWriter& writer)
                                           {
                                               return writeProtocol(writer, value.protocol) &&
                                                      writeBuild(writer, value.build);
                                           });
                    }
                    else if constexpr (std::is_same_v<Request, CommandRequest>)
                    {
                        return makePayload(
                            SocketFrameKind::CommandRequest, limits,
                            [&value](PayloadWriter& writer)
                            {
                                if (!writer.uuid(value.id.value) ||
                                    !writer.byte(static_cast<quint8>(value.command.index())))
                                    return false;
                                return std::visit(
                                    [&writer](const auto& command)
                                    {
                                        using Command = std::decay_t<decltype(command)>;
                                        if constexpr (std::is_same_v<Command,
                                                                     RefreshAccountCommand>)
                                            return writer.string(command.accountId) &&
                                                   writer.boolean(command.force);
                                        else
                                            return writer.word(command.action.value) &&
                                                   writer.bytes(command.payload);
                                    },
                                    value.command);
                            });
                    }
                    else if constexpr (std::is_same_v<Request, MaterializationRequest>)
                    {
                        return makePayload(
                            SocketFrameKind::MaterializationRequest, limits,
                            [&value](PayloadWriter& writer)
                            {
                                if (!writer.uuid(value.id.value) ||
                                    !writer.uuid(value.scope.value) ||
                                    !writer.byte(static_cast<quint8>(value.request.index())))
                                    return false;
                                return std::visit(
                                    [&writer](const auto& requestValue)
                                    {
                                        using MaterializationValue =
                                            std::decay_t<decltype(requestValue)>;
                                        if constexpr (std::is_same_v<MaterializationValue,
                                                                     MailboxWindowMaterialization>)
                                            return writer.string(requestValue.accountId) &&
                                                   writer.string(requestValue.mailboxId) &&
                                                   writer.qword(requestValue.offset) &&
                                                   writer.dword(requestValue.limit);
                                    },
                                    value.request);
                            });
                    }
                    else if constexpr (std::is_same_v<Request, CancelMaterializationScopeRequest>)
                    {
                        return makePayload(SocketFrameKind::CancelMaterializationScopeRequest,
                                           limits, [&value](PayloadWriter& writer)
                                           { return writer.uuid(value.scope.value); });
                    }
                    else if constexpr (std::is_same_v<Request, GetSettingsRequest>)
                    {
                        return makePayload(SocketFrameKind::GetSettingsRequest, limits,
                                           [](PayloadWriter&) { return true; });
                    }
                    else if constexpr (std::is_same_v<Request, UpdateSettingsRequest>)
                    {
                        return makePayload(SocketFrameKind::UpdateSettingsRequest, limits,
                                           [&value, &limits](PayloadWriter& writer)
                                           {
                                               return writer.qword(value.baseRevision.value) &&
                                                      writeSettingsUpdate(writer, value.update,
                                                                          limits);
                                           });
                    }
                    else if constexpr (std::is_same_v<Request, CacheAccessSuspendedAcknowledgement>)
                    {
                        return makePayload(SocketFrameKind::CacheAccessSuspendedAcknowledgement,
                                           limits, [&value](PayloadWriter& writer)
                                           { return writer.uuid(value.instance.value); });
                    }
                    else
                    {
                        return makePayload(SocketFrameKind::PingRequest, limits,
                                           [](PayloadWriter&) { return true; });
                    }
                },
                request);
        }

        using DecodedRequestResult = std::variant<ClientRequest, SocketFrameError>;

        DecodedRequestResult finishRequest(PayloadReader& reader, ClientRequest request)
        {
            if (const auto error = reader.finish())
                return *error;
            return request;
        }

        DecodedRequestResult decodeClientRequest(const SocketFrameKind kind,
                                                 const QByteArray& payload,
                                                 const BoundaryLimits& limits)
        {
            PayloadReader reader{payload, limits};
            switch (kind)
            {
            case SocketFrameKind::HelloRequest:
            {
                HelloRequest request;
                if (!readProtocol(reader, request.protocol) || !readBuild(reader, request.build))
                    return malformed(QStringLiteral("invalid HELLO payload"));
                return finishRequest(reader, ClientRequest{std::move(request)});
            }
            case SocketFrameKind::CommandRequest:
            {
                CommandRequest request;
                quint8 commandIndex = 0;
                if (!reader.uuid(request.id.value) || !reader.byte(commandIndex))
                    return malformed(QStringLiteral("invalid command variant"));
                if (commandIndex == 0)
                {
                    RefreshAccountCommand command;
                    if (!reader.string(command.accountId) || !reader.boolean(command.force))
                        return malformed(QStringLiteral("invalid command payload"));
                    request.command = std::move(command);
                }
                else if (commandIndex == 1)
                {
                    RemoteActionCommand command;
                    quint16 actionKind = 0;
                    if (!reader.word(actionKind) || !reader.bytes(command.payload))
                        return malformed(QStringLiteral("invalid remote action payload"));
                    command.action = ActionId{.value = actionKind};
                    request.command = std::move(command);
                }
                else
                    return malformed(QStringLiteral("invalid command variant"));
                return finishRequest(reader, ClientRequest{std::move(request)});
            }
            case SocketFrameKind::MaterializationRequest:
            {
                MaterializationRequest request;
                quint8 materializationIndex = 0;
                if (!reader.uuid(request.id.value) || !reader.uuid(request.scope.value) ||
                    !reader.byte(materializationIndex) || materializationIndex != 0)
                    return malformed(QStringLiteral("invalid materialization variant"));
                MailboxWindowMaterialization materialization;
                if (!reader.string(materialization.accountId) ||
                    !reader.string(materialization.mailboxId) ||
                    !reader.qword(materialization.offset) || !reader.dword(materialization.limit))
                    return malformed(QStringLiteral("invalid materialization payload"));
                request.request = std::move(materialization);
                return finishRequest(reader, ClientRequest{std::move(request)});
            }
            case SocketFrameKind::CancelMaterializationScopeRequest:
            {
                CancelMaterializationScopeRequest request;
                if (!reader.uuid(request.scope.value))
                    return malformed(QStringLiteral("invalid cancellation payload"));
                return finishRequest(reader, ClientRequest{std::move(request)});
            }
            case SocketFrameKind::GetSettingsRequest:
                return finishRequest(reader, ClientRequest{GetSettingsRequest{}});
            case SocketFrameKind::UpdateSettingsRequest:
            {
                UpdateSettingsRequest request;
                if (!reader.qword(request.baseRevision.value) ||
                    !readSettingsUpdate(reader, request.update, limits))
                    return malformed(QStringLiteral("invalid settings update payload"));
                return finishRequest(reader, ClientRequest{std::move(request)});
            }
            case SocketFrameKind::CacheAccessSuspendedAcknowledgement:
            {
                CacheAccessSuspendedAcknowledgement request;
                if (!reader.uuid(request.instance.value))
                    return malformed(QStringLiteral("invalid cache acknowledgement payload"));
                return finishRequest(reader, ClientRequest{std::move(request)});
            }
            case SocketFrameKind::PingRequest:
                return finishRequest(reader, ClientRequest{PingRequest{}});
            default:
                return malformed(QStringLiteral("frame is not a client request"));
            }
        }

        EncodedPayloadResult encodeHandshakeReply(const HandshakeReply& reply,
                                                  const BoundaryLimits& limits)
        {
            return makePayload(SocketFrameKind::HelloReply, limits,
                               [&reply, &limits](PayloadWriter& writer)
                               {
                                   if (const auto* ready = std::get_if<ReadyReply>(&reply))
                                   {
                                       return writer.byte(0) &&
                                              writeProtocol(writer, ready->protocol) &&
                                              writer.uuid(ready->daemon.value) &&
                                              writeCacheIdentity(writer, ready->cache) &&
                                              writer.string(ready->cacheDatabasePath) &&
                                              writer.qword(ready->epoch.value) &&
                                              writer.qword(ready->settingsRevision.value);
                                   }
                                   const auto& rejected = std::get<HandshakeRejected>(reply);
                                   return writer.byte(1) &&
                                          writeBoundaryError(writer, rejected.error);
                               });
        }

        EncodedPayloadResult encodeCommandReply(const CommandReply& reply,
                                                const BoundaryLimits& limits)
        {
            return makePayload(
                SocketFrameKind::CommandReplyFrame, limits,
                [&reply, &limits](PayloadWriter& writer)
                {
                    if (const auto* accepted = std::get_if<CommandAccepted>(&reply))
                    {
                        if (!writer.byte(0) || !writer.uuid(accepted->id.value) ||
                            !writer.boolean(accepted->operation.has_value()))
                            return false;
                        if (accepted->operation.has_value() &&
                            !writer.uuid(accepted->operation->value))
                            return false;
                        return writer.qword(accepted->epoch.value) &&
                               writeChangedDomains(writer, accepted->changedDomains, limits) &&
                               writeAffectedKeys(writer, accepted->affectedKeys, limits) &&
                               writer.boolean(accepted->immediateResult.has_value()) &&
                               (!accepted->immediateResult.has_value() ||
                                writer.bytes(*accepted->immediateResult));
                    }
                    const auto& rejected = std::get<CommandRejected>(reply);
                    return writer.byte(1) && writer.uuid(rejected.id.value) &&
                           writeBoundaryError(writer, rejected.error);
                });
        }

        EncodedPayloadResult encodeMaterializationReply(const MaterializationReply& reply,
                                                        const BoundaryLimits& limits)
        {
            return makePayload(SocketFrameKind::MaterializationReplyFrame, limits,
                               [&reply](PayloadWriter& writer)
                               {
                                   if (const auto* accepted =
                                           std::get_if<MaterializationAccepted>(&reply))
                                       return writer.byte(0) && writer.uuid(accepted->id.value);
                                   const auto& rejected = std::get<MaterializationRejected>(reply);
                                   return writer.byte(1) && writer.uuid(rejected.id.value) &&
                                          writeBoundaryError(writer, rejected.error);
                               });
        }

        EncodedPayloadResult encodeSettingsReadReply(const SettingsReadReply& reply,
                                                     const BoundaryLimits& limits)
        {
            return makePayload(
                SocketFrameKind::SettingsReadReplyFrame, limits,
                [&reply, &limits](PayloadWriter& writer)
                {
                    if (const auto* snapshot = std::get_if<SettingsSnapshotReply>(&reply))
                        return writer.byte(0) &&
                               writeSettingsSnapshot(writer, snapshot->snapshot, limits);
                    return writer.byte(1) &&
                           writeBoundaryError(writer, std::get<SettingsReadRejected>(reply).error);
                });
        }

        EncodedPayloadResult encodeSettingsUpdateReply(const SettingsUpdateReply& reply,
                                                       const BoundaryLimits& limits)
        {
            return makePayload(SocketFrameKind::SettingsUpdateReplyFrame, limits,
                               [&reply](PayloadWriter& writer)
                               {
                                   if (const auto* updated = std::get_if<SettingsUpdated>(&reply))
                                       return writer.byte(0) &&
                                              writer.qword(updated->revision.value);
                                   const auto& rejected = std::get<SettingsUpdateRejected>(reply);
                                   return writer.byte(1) &&
                                          writer.qword(rejected.currentRevision.value) &&
                                          writeBoundaryError(writer, rejected.error);
                               });
        }

        EncodedPayloadResult encodeOptionalError(const SocketFrameKind kind,
                                                 const std::optional<BoundaryError>& error,
                                                 const BoundaryLimits& limits)
        {
            return makePayload(kind, limits,
                               [&error](PayloadWriter& writer)
                               {
                                   return writer.boolean(error.has_value()) &&
                                          (!error.has_value() ||
                                           writeBoundaryError(writer, *error));
                               });
        }

        using DecodedReply =
            std::variant<HandshakeReply, CommandReply, MaterializationReply, SettingsReadReply,
                         SettingsUpdateReply, std::optional<BoundaryError>>;

        template <typename Reply>
        std::variant<Reply, SocketFrameError> finishReply(PayloadReader& reader, Reply reply)
        {
            if (const auto error = reader.finish())
                return *error;
            return reply;
        }

        std::variant<HandshakeReply, SocketFrameError>
        decodeHandshakeReply(const QByteArray& payload, const BoundaryLimits& limits)
        {
            PayloadReader reader{payload, limits};
            quint8 kind = 0;
            if (!reader.byte(kind))
                return malformed(QStringLiteral("invalid handshake reply"));
            if (kind == 0)
            {
                ReadyReply ready;
                if (!readProtocol(reader, ready.protocol) || !reader.uuid(ready.daemon.value) ||
                    !readCacheIdentity(reader, ready.cache) ||
                    !reader.string(ready.cacheDatabasePath) || !reader.qword(ready.epoch.value) ||
                    !reader.qword(ready.settingsRevision.value))
                    return malformed(QStringLiteral("invalid READY reply"));
                return finishReply(reader, HandshakeReply{std::move(ready)});
            }
            if (kind == 1)
            {
                HandshakeRejected rejected;
                if (!readBoundaryError(reader, rejected.error))
                    return malformed(QStringLiteral("invalid handshake rejection"));
                return finishReply(reader, HandshakeReply{std::move(rejected)});
            }
            return malformed(QStringLiteral("unknown handshake reply variant"));
        }

        std::variant<CommandReply, SocketFrameError>
        decodeCommandReply(const QByteArray& payload, const BoundaryLimits& limits)
        {
            PayloadReader reader{payload, limits};
            quint8 kind = 0;
            if (!reader.byte(kind))
                return malformed(QStringLiteral("invalid command reply"));
            if (kind == 0)
            {
                CommandAccepted accepted;
                bool hasOperation = false;
                if (!reader.uuid(accepted.id.value) || !reader.boolean(hasOperation))
                    return malformed(QStringLiteral("invalid accepted command reply"));
                if (hasOperation)
                {
                    accepted.operation.emplace();
                    if (!reader.uuid(accepted.operation->value))
                        return malformed(QStringLiteral("invalid command operation"));
                }
                bool hasImmediateResult = false;
                if (!reader.qword(accepted.epoch.value) ||
                    !readChangedDomains(reader, accepted.changedDomains, limits) ||
                    !readAffectedKeys(reader, accepted.affectedKeys, limits) ||
                    !reader.boolean(hasImmediateResult))
                    return malformed(QStringLiteral("invalid accepted command payload"));
                if (hasImmediateResult)
                {
                    accepted.immediateResult.emplace();
                    if (!reader.bytes(*accepted.immediateResult))
                        return malformed(QStringLiteral("invalid immediate command result"));
                }
                return finishReply(reader, CommandReply{std::move(accepted)});
            }
            if (kind == 1)
            {
                CommandRejected rejected;
                if (!reader.uuid(rejected.id.value) || !readBoundaryError(reader, rejected.error))
                    return malformed(QStringLiteral("invalid rejected command reply"));
                return finishReply(reader, CommandReply{std::move(rejected)});
            }
            return malformed(QStringLiteral("unknown command reply variant"));
        }

        std::variant<MaterializationReply, SocketFrameError>
        decodeMaterializationReply(const QByteArray& payload, const BoundaryLimits& limits)
        {
            PayloadReader reader{payload, limits};
            quint8 kind = 0;
            if (!reader.byte(kind))
                return malformed(QStringLiteral("invalid materialization reply"));
            if (kind == 0)
            {
                MaterializationAccepted accepted;
                if (!reader.uuid(accepted.id.value))
                    return malformed(QStringLiteral("invalid accepted materialization reply"));
                return finishReply(reader, MaterializationReply{std::move(accepted)});
            }
            if (kind == 1)
            {
                MaterializationRejected rejected;
                if (!reader.uuid(rejected.id.value) || !readBoundaryError(reader, rejected.error))
                    return malformed(QStringLiteral("invalid rejected materialization reply"));
                return finishReply(reader, MaterializationReply{std::move(rejected)});
            }
            return malformed(QStringLiteral("unknown materialization reply variant"));
        }

        std::variant<SettingsReadReply, SocketFrameError>
        decodeSettingsReadReply(const QByteArray& payload, const BoundaryLimits& limits)
        {
            PayloadReader reader{payload, limits};
            quint8 kind = 0;
            if (!reader.byte(kind))
                return malformed(QStringLiteral("invalid settings reply"));
            if (kind == 0)
            {
                SettingsSnapshot snapshot;
                if (!readSettingsSnapshot(reader, snapshot, limits))
                    return malformed(QStringLiteral("invalid settings snapshot"));
                return finishReply(reader, SettingsReadReply{SettingsSnapshotReply{
                                               .snapshot = std::move(snapshot)}});
            }
            if (kind == 1)
            {
                SettingsReadRejected rejected;
                if (!readBoundaryError(reader, rejected.error))
                    return malformed(QStringLiteral("invalid settings rejection"));
                return finishReply(reader, SettingsReadReply{std::move(rejected)});
            }
            return malformed(QStringLiteral("unknown settings reply variant"));
        }

        std::variant<SettingsUpdateReply, SocketFrameError>
        decodeSettingsUpdateReply(const QByteArray& payload, const BoundaryLimits& limits)
        {
            PayloadReader reader{payload, limits};
            quint8 kind = 0;
            if (!reader.byte(kind))
                return malformed(QStringLiteral("invalid settings update reply"));
            if (kind == 0)
            {
                SettingsUpdated updated;
                if (!reader.qword(updated.revision.value))
                    return malformed(QStringLiteral("invalid settings updated reply"));
                return finishReply(reader, SettingsUpdateReply{std::move(updated)});
            }
            if (kind == 1)
            {
                SettingsUpdateRejected rejected;
                if (!reader.qword(rejected.currentRevision.value) ||
                    !readBoundaryError(reader, rejected.error))
                    return malformed(QStringLiteral("invalid settings update rejection"));
                return finishReply(reader, SettingsUpdateReply{std::move(rejected)});
            }
            return malformed(QStringLiteral("unknown settings update reply variant"));
        }

        std::variant<std::optional<BoundaryError>, SocketFrameError>
        decodeOptionalError(const QByteArray& payload, const BoundaryLimits& limits)
        {
            PayloadReader reader{payload, limits};
            bool present = false;
            if (!reader.boolean(present))
                return malformed(QStringLiteral("invalid empty reply"));
            if (!present)
                return finishReply(reader, std::optional<BoundaryError>{});
            BoundaryError error;
            if (!readBoundaryError(reader, error))
                return malformed(QStringLiteral("invalid error reply"));
            return finishReply(reader, std::optional<BoundaryError>{std::move(error)});
        }

        [[nodiscard]] quint8 activationRouteKind(const ActivationRoute& route)
        {
            return std::visit(
                [](const auto& value) -> quint8
                {
                    using Route = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<Route, OpenMailboxRoute>)
                        return 0;
                    else if constexpr (std::is_same_v<Route, OpenMessageRoute>)
                        return 1;
                    else if constexpr (std::is_same_v<Route, OpenComposeRoute>)
                        return 2;
                    else if constexpr (std::is_same_v<Route, RaiseGuiRoute>)
                        return 3;
                    else if constexpr (std::is_same_v<Route, OpenSettingsRoute>)
                        return 4;
                    else if constexpr (std::is_same_v<Route, RestoreDraftRoute>)
                        return 5;
                    else if constexpr (std::is_same_v<Route, OpenTaskCenterRoute>)
                        return 6;
                    else if constexpr (std::is_same_v<Route, OpenMailtoRoute>)
                        return 7;
                    else if constexpr (std::is_same_v<Route, ShowUndoSendDialogRoute>)
                        return 8;
                    else if constexpr (std::is_same_v<Route, CloseUndoSendDialogRoute>)
                        return 9;
                    else if constexpr (std::is_same_v<Route, OpenCalendarEventRoute>)
                        return 10;
                    else
                        static_assert(sizeof(Route) == 0, "Unhandled activation route");
                },
                route);
        }

        bool writeActivationRoute(PayloadWriter& writer, const ActivationRoute& route)
        {
            if (!writer.byte(activationRouteKind(route)))
                return false;
            return std::visit(
                [&writer](const auto& value)
                {
                    using Route = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<Route, OpenMailboxRoute>)
                        return writer.string(value.accountId) && writer.string(value.mailboxId) &&
                               writer.string(value.activationToken);
                    else if constexpr (std::is_same_v<Route, OpenMessageRoute>)
                        return writer.string(value.accountId) && writer.string(value.mailboxId) &&
                               writer.string(value.mailboxName) && writer.string(value.threadId) &&
                               writer.string(value.emailId) && writer.string(value.activationToken);
                    else if constexpr (std::is_same_v<Route, OpenComposeRoute>)
                        return writer.string(value.composeSessionId) &&
                               writer.string(value.activationToken);
                    else if constexpr (std::is_same_v<Route, RaiseGuiRoute>)
                        return writer.string(value.activationToken);
                    else if constexpr (std::is_same_v<Route, OpenSettingsRoute>)
                        return writer.string(value.connectionId) &&
                               writer.string(value.activationToken);
                    else if constexpr (std::is_same_v<Route, OpenCalendarEventRoute>)
                        return writer.string(value.calendarAccountId) &&
                               writer.string(value.eventId) &&
                               writer.boolean(value.recurrenceId.has_value()) &&
                               (!value.recurrenceId || writer.string(*value.recurrenceId)) &&
                               writer.string(value.navigationDate) &&
                               writer.string(value.activationToken);
                    else if constexpr (std::is_same_v<Route, RestoreDraftRoute>)
                        return writer.string(value.accountId) &&
                               writer.string(value.draftEmailId) &&
                               writer.string(value.composeSessionId) &&
                               writer.string(value.activationToken);
                    else if constexpr (std::is_same_v<Route, OpenTaskCenterRoute>)
                        return writer.string(value.activationToken);
                    else if constexpr (std::is_same_v<Route, OpenMailtoRoute>)
                        return writer.string(value.uri) && writer.string(value.activationToken);
                    else if constexpr (std::is_same_v<Route, ShowUndoSendDialogRoute>)
                        return value.deadlineEpochMilliseconds >= 0 &&
                               writer.string(value.sendId) && writer.string(value.title) &&
                               writer.string(value.message) &&
                               writer.qword(static_cast<quint64>(value.deadlineEpochMilliseconds));
                    else if constexpr (std::is_same_v<Route, CloseUndoSendDialogRoute>)
                        return writer.string(value.sendId);
                    else
                        return false;
                },
                route);
        }

        bool readActivationRoute(PayloadReader& reader, ActivationRoute& route)
        {
            quint8 kind = 0;
            if (!reader.byte(kind))
                return false;
            if (kind == 0)
            {
                OpenMailboxRoute value;
                if (!reader.string(value.accountId) || !reader.string(value.mailboxId) ||
                    !reader.string(value.activationToken))
                    return false;
                route = std::move(value);
                return true;
            }
            if (kind == 1)
            {
                OpenMessageRoute value;
                if (!reader.string(value.accountId) || !reader.string(value.mailboxId) ||
                    !reader.string(value.mailboxName) || !reader.string(value.threadId) ||
                    !reader.string(value.emailId) || !reader.string(value.activationToken))
                    return false;
                route = std::move(value);
                return true;
            }
            if (kind == 2)
            {
                OpenComposeRoute value;
                if (!reader.string(value.composeSessionId) || !reader.string(value.activationToken))
                    return false;
                route = std::move(value);
                return true;
            }
            if (kind == 3)
            {
                RaiseGuiRoute value;
                if (!reader.string(value.activationToken))
                    return false;
                route = std::move(value);
                return true;
            }
            if (kind == 4)
            {
                OpenSettingsRoute value;
                if (!reader.string(value.connectionId) || !reader.string(value.activationToken))
                    return false;
                route = std::move(value);
                return true;
            }
            if (kind == 10)
            {
                OpenCalendarEventRoute value;
                bool hasRecurrenceId = false;
                if (!reader.string(value.calendarAccountId) || !reader.string(value.eventId) ||
                    !reader.boolean(hasRecurrenceId))
                    return false;
                if (hasRecurrenceId)
                {
                    QString recurrenceId;
                    if (!reader.string(recurrenceId))
                        return false;
                    value.recurrenceId = std::move(recurrenceId);
                }
                if (!reader.string(value.navigationDate) || !reader.string(value.activationToken))
                    return false;
                route = std::move(value);
                return true;
            }
            if (kind == 5)
            {
                RestoreDraftRoute value;
                if (!reader.string(value.accountId) || !reader.string(value.draftEmailId) ||
                    !reader.string(value.composeSessionId) || !reader.string(value.activationToken))
                    return false;
                route = std::move(value);
                return true;
            }
            if (kind == 6)
            {
                OpenTaskCenterRoute value;
                if (!reader.string(value.activationToken))
                    return false;
                route = std::move(value);
                return true;
            }
            if (kind == 7)
            {
                OpenMailtoRoute value;
                if (!reader.string(value.uri) || !reader.string(value.activationToken))
                    return false;
                route = std::move(value);
                return true;
            }
            if (kind == 8)
            {
                ShowUndoSendDialogRoute value;
                quint64 deadline = 0;
                if (!reader.string(value.sendId) || !reader.string(value.title) ||
                    !reader.string(value.message) || !reader.qword(deadline))
                    return false;
                if (deadline > static_cast<quint64>(std::numeric_limits<qint64>::max()))
                    return false;
                value.deadlineEpochMilliseconds = static_cast<qint64>(deadline);
                route = std::move(value);
                return true;
            }
            if (kind == 9)
            {
                CloseUndoSendDialogRoute value;
                if (!reader.string(value.sendId))
                    return false;
                route = std::move(value);
                return true;
            }
            return reader.fail(QStringLiteral("activation route variant is invalid"));
        }

        struct DecodedActivationRequest
        {
            ProtocolVersion protocol;
            BuildIdentity build;
            ActivationRoute route;
        };

        EncodedPayloadResult encodeActivationRequest(const SocketEndpointOptions& options,
                                                     const ActivationRoute& route)
        {
            const auto build = options.expectedBuild.value_or(BuildIdentity{});
            return makePayload(SocketFrameKind::ActivationRequest, options.limits,
                               [&options, &build, &route](PayloadWriter& writer)
                               {
                                   return writeProtocol(writer, options.protocol) &&
                                          writeBuild(writer, build) &&
                                          writeActivationRoute(writer, route);
                               });
        }

        std::variant<DecodedActivationRequest, SocketFrameError>
        decodeActivationRequest(const QByteArray& payload, const BoundaryLimits& limits)
        {
            PayloadReader reader{payload, limits};
            DecodedActivationRequest request;
            if (!readProtocol(reader, request.protocol) || !readBuild(reader, request.build) ||
                !readActivationRoute(reader, request.route))
                return malformed(QStringLiteral("invalid activation request"));
            if (const auto error = reader.finish())
                return *error;
            return request;
        }

        EncodedPayloadResult encodeBoundaryEvent(const BoundaryEvent& event,
                                                 const BoundaryLimits& limits)
        {
            return makePayload(
                SocketFrameKind::BoundaryEventFrame, limits,
                [&event, &limits](PayloadWriter& writer)
                {
                    if (!writer.byte(static_cast<quint8>(event.index())))
                        return false;
                    return std::visit(
                        [&writer, &limits](const auto& value)
                        {
                            using Event = std::decay_t<decltype(value)>;
                            if constexpr (std::is_same_v<Event, CacheInvalidation>)
                                return writeCacheInvalidation(writer, value, limits);
                            else if constexpr (std::is_same_v<Event, OperationFailed>)
                                return writer.uuid(value.operation.value) &&
                                       writeBoundaryError(writer, value.error);
                            else if constexpr (std::is_same_v<Event, OperationCompleted>)
                                return writer.uuid(value.operation.value) &&
                                       writer.bytes(value.result);
                            else if constexpr (std::is_same_v<Event, SettingsUpdated>)
                                return writer.qword(value.revision.value);
                            else if constexpr (std::is_same_v<Event, ActivationRequested>)
                                return writeActivationRoute(writer, value.route);
                            else if constexpr (std::is_same_v<Event, DaemonStatusChanged>)
                            {
                                return writeEnum(writer, value.status.lifecycle) &&
                                       writeVector(writer, value.status.accounts,
                                                   limits.maximumCollectionItems,
                                                   QStringLiteral("status.accounts"),
                                                   [&writer](const AccountStatus& account)
                                                   {
                                                       return writer.string(account.accountId) &&
                                                              writeEnum(writer, account.state) &&
                                                              writer.string(account.detail);
                                                   });
                            }
                            else if constexpr (std::is_same_v<Event, CacheAccessSuspendRequested>)
                            {
                                return writer.uuid(value.instance.value) &&
                                       writeEnum(writer, value.reason) &&
                                       writer.boolean(value.targetSchema.has_value()) &&
                                       (!value.targetSchema.has_value() ||
                                        writer.dword(value.targetSchema->value));
                            }
                            else if constexpr (std::is_same_v<Event, DaemonShutdownRequested>)
                                return true;
                            else if constexpr (std::is_same_v<Event, DaemonLogEntries>)
                            {
                                return writeVector(
                                    writer, value.entries, limits.maximumCollectionItems,
                                    QStringLiteral("daemon_log.entries"),
                                    [&writer](const DiagnosticLogEntry& entry)
                                    {
                                        return writer.qword(entry.timestampMilliseconds) &&
                                               writer.byte(entry.level) &&
                                               writer.string(entry.subsystem) &&
                                               writer.string(entry.message);
                                    });
                            }
                            else if constexpr (std::is_same_v<Event, ThreadMaterializationProgress>)
                            {
                                return writer.string(value.accountId) &&
                                       writeVector(writer, value.threadIds,
                                                   limits.maximumMaterializationItems,
                                                   QStringLiteral("thread_progress.thread_ids"),
                                                   [&writer](const QString& threadId)
                                                   { return writer.string(threadId); }) &&
                                       writer.boolean(value.inFlight) &&
                                       writer.boolean(value.success) && writer.string(value.error);
                            }
                            else
                                return writeCacheIdentity(writer, value.cache) &&
                                       writer.string(value.cacheDatabasePath) &&
                                       writer.qword(value.epoch.value);
                        },
                        event);
                });
        }

        std::variant<BoundaryEvent, SocketFrameError>
        decodeBoundaryEvent(const QByteArray& payload, const BoundaryLimits& limits)
        {
            PayloadReader reader{payload, limits};
            quint8 kind = 0;
            if (!reader.byte(kind))
                return malformed(QStringLiteral("invalid boundary event"));
            if (kind == 0)
            {
                CacheInvalidation event;
                if (!readCacheInvalidation(reader, event, limits))
                    return malformed(QStringLiteral("invalid cache invalidation"));
                return finishReply(reader, BoundaryEvent{std::move(event)});
            }
            if (kind == 1)
            {
                OperationFailed event;
                if (!reader.uuid(event.operation.value) || !readBoundaryError(reader, event.error))
                    return malformed(QStringLiteral("invalid operation failure"));
                return finishReply(reader, BoundaryEvent{std::move(event)});
            }
            if (kind == 2)
            {
                OperationCompleted event;
                if (!reader.uuid(event.operation.value) || !reader.bytes(event.result))
                    return malformed(QStringLiteral("invalid operation completion"));
                return finishReply(reader, BoundaryEvent{std::move(event)});
            }
            if (kind == 3)
            {
                SettingsUpdated event;
                if (!reader.qword(event.revision.value))
                    return malformed(QStringLiteral("invalid settings event"));
                return finishReply(reader, BoundaryEvent{std::move(event)});
            }
            if (kind == 4)
            {
                ActivationRequested event;
                if (!readActivationRoute(reader, event.route))
                    return malformed(QStringLiteral("invalid activation event"));
                return finishReply(reader, BoundaryEvent{std::move(event)});
            }
            if (kind == 5)
            {
                DaemonStatusChanged event;
                if (!readEnum(reader, event.status.lifecycle,
                              static_cast<quint8>(DaemonLifecycle::ShuttingDown)) ||
                    !readVector(reader, event.status.accounts, limits.maximumCollectionItems,
                                QStringLiteral("status.accounts"),
                                [&reader](AccountStatus& account)
                                {
                                    return reader.string(account.accountId) &&
                                           readEnum(reader, account.state,
                                                    static_cast<quint8>(AccountState::Paused)) &&
                                           reader.string(account.detail);
                                }))
                    return malformed(QStringLiteral("invalid daemon status event"));
                return finishReply(reader, BoundaryEvent{std::move(event)});
            }
            if (kind == 6)
            {
                CacheAccessSuspendRequested event;
                bool hasSchema = false;
                if (!reader.uuid(event.instance.value) ||
                    !readEnum(reader, event.reason,
                              static_cast<quint8>(CacheSuspendReason::Recovery)) ||
                    !reader.boolean(hasSchema))
                    return malformed(QStringLiteral("invalid cache suspension event"));
                if (hasSchema)
                {
                    event.targetSchema.emplace();
                    if (!reader.dword(event.targetSchema->value))
                        return malformed(QStringLiteral("invalid cache target schema"));
                }
                return finishReply(reader, BoundaryEvent{std::move(event)});
            }
            if (kind == 7)
            {
                CacheAccessResumed event;
                if (!readCacheIdentity(reader, event.cache) ||
                    !reader.string(event.cacheDatabasePath) || !reader.qword(event.epoch.value))
                    return malformed(QStringLiteral("invalid cache resume event"));
                return finishReply(reader, BoundaryEvent{std::move(event)});
            }
            if (kind == 8)
                return finishReply(reader, BoundaryEvent{DaemonShutdownRequested{}});
            if (kind == 9)
            {
                DaemonLogEntries event;
                if (!readVector(reader, event.entries, limits.maximumCollectionItems,
                                QStringLiteral("daemon_log.entries"),
                                [&reader](DiagnosticLogEntry& entry)
                                {
                                    quint8 level = 0;
                                    if (!reader.qword(entry.timestampMilliseconds) ||
                                        !reader.byte(level) || level > 4 ||
                                        !reader.string(entry.subsystem) ||
                                        !reader.string(entry.message))
                                        return false;
                                    entry.level = level;
                                    return true;
                                }))
                    return malformed(QStringLiteral("invalid daemon log event"));
                return finishReply(reader, BoundaryEvent{std::move(event)});
            }
            if (kind == 10)
            {
                ThreadMaterializationProgress event;
                if (!reader.string(event.accountId) ||
                    !readVector(reader, event.threadIds, limits.maximumMaterializationItems,
                                QStringLiteral("thread_progress.thread_ids"),
                                [&reader](QString& threadId) { return reader.string(threadId); }) ||
                    !reader.boolean(event.inFlight) || !reader.boolean(event.success) ||
                    !reader.string(event.error))
                    return malformed(QStringLiteral("invalid Thread materialization progress"));
                return finishReply(reader, BoundaryEvent{std::move(event)});
            }
            return malformed(QStringLiteral("unknown boundary event variant"));
        }

        std::variant<SocketFrameError, std::optional<BoundaryError>>
        decodeProtocolError(const QByteArray& payload, const BoundaryLimits& limits)
        {
            PayloadReader reader{payload, limits};
            quint8 code = 0;
            QString detail;
            if (!reader.byte(code) || !reader.string(detail))
                return malformed(QStringLiteral("invalid protocol error"));
            if (code > static_cast<quint8>(SocketFrameErrorCode::MalformedPayload))
                return malformed(QStringLiteral("protocol error code is invalid"));
            if (const auto error = reader.finish())
                return *error;
            return std::optional<BoundaryError>{BoundaryError{
                .code = BoundaryErrorCode::ProtocolViolation,
                .field = QStringLiteral("socket"),
                .detail = detail,
            }};
        }

        EncodedPayloadResult encodeProtocolError(const SocketFrameError& error,
                                                 const BoundaryLimits& limits)
        {
            return makePayload(SocketFrameKind::ProtocolError, limits,
                               [&error](PayloadWriter& writer)
                               {
                                   return writer.byte(static_cast<quint8>(error.code)) &&
                                          writer.string(error.detail);
                               });
        }

        [[nodiscard]] BoundaryError makeBoundaryError(const SocketTransportError& error)
        {
            auto code = BoundaryErrorCode::TransportUnavailable;
            if (error.reason == SocketDisconnectReason::ProtocolViolation)
                code = BoundaryErrorCode::ProtocolViolation;
            else if (error.reason == SocketDisconnectReason::QueueOverflow)
                code = BoundaryErrorCode::Busy;
            return {.code = code, .field = QStringLiteral("socket"), .detail = error.detail};
        }

        [[nodiscard]] bool isReadyReply(const HandshakeReply& reply)
        {
            return std::holds_alternative<ReadyReply>(reply);
        }

        [[nodiscard]] bool sameEventKind(const std::optional<BoundaryEvent>& left,
                                         const std::optional<BoundaryEvent>& right)
        {
            return left.has_value() && right.has_value() && left->index() == right->index();
        }

        void mergeInvalidation(CacheInvalidation& target, const CacheInvalidation& source,
                               const BoundaryLimits& limits)
        {
            const auto appendUnique = [](auto& values, const auto& value, const std::size_t maximum)
            {
                if (values.size() < maximum && !std::ranges::contains(values, value))
                    values.push_back(value);
            };
            target.epoch = source.epoch;
            if (target.accountId.isEmpty())
                target.accountId = source.accountId;
            for (const auto domain : source.changedDomains)
                appendUnique(target.changedDomains, domain, limits.maximumCollectionItems);
            for (const auto& key : source.affectedKeys)
                appendUnique(target.affectedKeys, key, limits.maximumAffectedKeys);
            for (const auto& mailboxId : source.mailboxIds)
                appendUnique(target.mailboxIds, mailboxId, limits.maximumAffectedKeys);
            for (const auto& emailId : source.messageContentEmailIds)
                appendUnique(target.messageContentEmailIds, emailId, limits.maximumAffectedKeys);
            for (const auto& window : source.mailboxWindows)
            {
                const auto found =
                    std::ranges::find_if(target.mailboxWindows,
                                         [&window](const MailboxWindowInvalidation& existing)
                                         {
                                             return existing.mailboxId == window.mailboxId &&
                                                    existing.offset == window.offset &&
                                                    existing.limit == window.limit;
                                         });
                if (found == target.mailboxWindows.end())
                {
                    if (target.mailboxWindows.size() < limits.maximumCollectionItems)
                        target.mailboxWindows.push_back(window);
                }
                else if (window.total.has_value())
                {
                    found->total = window.total;
                }
            }
            for (const auto& window : source.searchWindows)
            {
                const auto found =
                    std::ranges::find_if(target.searchWindows,
                                         [&window](const SearchWindowInvalidation& existing)
                                         {
                                             return existing.queryKey == window.queryKey &&
                                                    existing.offset == window.offset &&
                                                    existing.limit == window.limit;
                                         });
                if (found == target.searchWindows.end())
                {
                    if (target.searchWindows.size() < limits.maximumCollectionItems)
                        target.searchWindows.push_back(window);
                }
                else if (window.total.has_value())
                {
                    found->total = window.total;
                }
            }
        }

    } // namespace

    namespace detail
    {
        namespace
        {
            [[nodiscard]] EncodedPayloadResult convertEncoded(auto encoded)
            {
                if (const auto* error = std::get_if<SocketFrameError>(&encoded))
                    return *error;
                auto payload = std::get<0>(std::move(encoded));
                return EncodedPayload{.kind = payload.kind, .payload = std::move(payload.payload)};
            }
        } // namespace

        EncodedPayloadResult encodeClientRequest(const ClientRequest& request,
                                                 const BoundaryLimits& limits)
        {
            return convertEncoded(javelin::protocol::encodeClientRequest(request, limits));
        }

        std::variant<ClientRequest, SocketFrameError>
        decodeClientRequest(const SocketFrameKind kind, const QByteArray& payload,
                            const BoundaryLimits& limits)
        {
            return javelin::protocol::decodeClientRequest(kind, payload, limits);
        }

        EncodedPayloadResult encodeHandshakeReply(const HandshakeReply& reply,
                                                  const BoundaryLimits& limits)
        {
            return convertEncoded(javelin::protocol::encodeHandshakeReply(reply, limits));
        }

        EncodedPayloadResult encodeCommandReply(const CommandReply& reply,
                                                const BoundaryLimits& limits)
        {
            return convertEncoded(javelin::protocol::encodeCommandReply(reply, limits));
        }

        EncodedPayloadResult encodeMaterializationReply(const MaterializationReply& reply,
                                                        const BoundaryLimits& limits)
        {
            return convertEncoded(javelin::protocol::encodeMaterializationReply(reply, limits));
        }

        EncodedPayloadResult encodeSettingsReadReply(const SettingsReadReply& reply,
                                                     const BoundaryLimits& limits)
        {
            return convertEncoded(javelin::protocol::encodeSettingsReadReply(reply, limits));
        }

        EncodedPayloadResult encodeSettingsUpdateReply(const SettingsUpdateReply& reply,
                                                       const BoundaryLimits& limits)
        {
            return convertEncoded(javelin::protocol::encodeSettingsUpdateReply(reply, limits));
        }

        EncodedPayloadResult encodeOptionalError(const SocketFrameKind kind,
                                                 const std::optional<BoundaryError>& error,
                                                 const BoundaryLimits& limits)
        {
            return convertEncoded(javelin::protocol::encodeOptionalError(kind, error, limits));
        }

        std::variant<HandshakeReply, SocketFrameError>
        decodeHandshakeReply(const QByteArray& payload, const BoundaryLimits& limits)
        {
            return javelin::protocol::decodeHandshakeReply(payload, limits);
        }

        std::variant<CommandReply, SocketFrameError>
        decodeCommandReply(const QByteArray& payload, const BoundaryLimits& limits)
        {
            return javelin::protocol::decodeCommandReply(payload, limits);
        }

        std::variant<MaterializationReply, SocketFrameError>
        decodeMaterializationReply(const QByteArray& payload, const BoundaryLimits& limits)
        {
            return javelin::protocol::decodeMaterializationReply(payload, limits);
        }

        std::variant<SettingsReadReply, SocketFrameError>
        decodeSettingsReadReply(const QByteArray& payload, const BoundaryLimits& limits)
        {
            return javelin::protocol::decodeSettingsReadReply(payload, limits);
        }

        std::variant<SettingsUpdateReply, SocketFrameError>
        decodeSettingsUpdateReply(const QByteArray& payload, const BoundaryLimits& limits)
        {
            return javelin::protocol::decodeSettingsUpdateReply(payload, limits);
        }

        std::variant<std::optional<BoundaryError>, SocketFrameError>
        decodeOptionalError(const QByteArray& payload, const BoundaryLimits& limits)
        {
            return javelin::protocol::decodeOptionalError(payload, limits);
        }

        EncodedPayloadResult encodeActivationRequest(const SocketEndpointOptions& options,
                                                     const ActivationRoute& route)
        {
            return convertEncoded(javelin::protocol::encodeActivationRequest(options, route));
        }

        std::variant<DecodedActivationRequest, SocketFrameError>
        decodeActivationRequest(const QByteArray& payload, const BoundaryLimits& limits)
        {
            auto decoded = javelin::protocol::decodeActivationRequest(payload, limits);
            if (const auto* error = std::get_if<SocketFrameError>(&decoded))
                return *error;
            auto request = std::get<0>(std::move(decoded));
            return DecodedActivationRequest{.protocol = request.protocol,
                                            .build = std::move(request.build),
                                            .route = std::move(request.route)};
        }

        EncodedPayloadResult encodeBoundaryEvent(const BoundaryEvent& event,
                                                 const BoundaryLimits& limits)
        {
            return convertEncoded(javelin::protocol::encodeBoundaryEvent(event, limits));
        }

        std::variant<BoundaryEvent, SocketFrameError>
        decodeBoundaryEvent(const QByteArray& payload, const BoundaryLimits& limits)
        {
            return javelin::protocol::decodeBoundaryEvent(payload, limits);
        }

        std::variant<SocketFrameError, std::optional<BoundaryError>>
        decodeProtocolError(const QByteArray& payload, const BoundaryLimits& limits)
        {
            return javelin::protocol::decodeProtocolError(payload, limits);
        }

        EncodedPayloadResult encodeProtocolError(const SocketFrameError& error,
                                                 const BoundaryLimits& limits)
        {
            return convertEncoded(javelin::protocol::encodeProtocolError(error, limits));
        }

        BoundaryError makeBoundaryError(const SocketTransportError& error)
        {
            return javelin::protocol::makeBoundaryError(error);
        }

        bool isReadyReply(const HandshakeReply& reply)
        {
            return javelin::protocol::isReadyReply(reply);
        }

        bool sameEventKind(const std::optional<BoundaryEvent>& left,
                           const std::optional<BoundaryEvent>& right)
        {
            return javelin::protocol::sameEventKind(left, right);
        }

        void mergeInvalidation(CacheInvalidation& target, const CacheInvalidation& source,
                               const BoundaryLimits& limits)
        {
            javelin::protocol::mergeInvalidation(target, source, limits);
        }
    } // namespace detail

    std::variant<QByteArray, SocketFrameError> encodeActivationRoute(const ActivationRoute& route,
                                                                     const BoundaryLimits& limits)
    {
        const auto encoded =
            makePayload(SocketFrameKind::ActivationRequest, limits, [&route](PayloadWriter& writer)
                        { return writeActivationRoute(writer, route); });
        if (const auto* error = std::get_if<SocketFrameError>(&encoded))
            return *error;
        return std::get<EncodedPayload>(encoded).payload;
    }

    std::variant<ActivationRoute, SocketFrameError>
    decodeActivationRoute(const QByteArray& payload, const BoundaryLimits& limits)
    {
        PayloadReader reader{payload, limits};
        ActivationRoute route;
        if (!readActivationRoute(reader, route))
            return malformed(QStringLiteral("invalid activation route"));
        if (const auto error = reader.finish())
            return *error;
        return route;
    }

    std::variant<QByteArray, SocketFrameError>
    encodeActivationReply(const std::optional<BoundaryError>& error, const BoundaryLimits& limits)
    {
        const auto encoded =
            makePayload(SocketFrameKind::ActivationReply, limits,
                        [&error](PayloadWriter& writer)
                        {
                            return writer.boolean(error.has_value()) &&
                                   (!error.has_value() || writeBoundaryError(writer, *error));
                        });
        if (const auto* frameError = std::get_if<SocketFrameError>(&encoded))
            return *frameError;
        return std::get<EncodedPayload>(encoded).payload;
    }

    std::variant<std::optional<BoundaryError>, SocketFrameError>
    decodeActivationReply(const QByteArray& payload, const BoundaryLimits& limits)
    {
        PayloadReader reader{payload, limits};
        bool hasError = false;
        if (!reader.boolean(hasError))
            return malformed(QStringLiteral("invalid activation reply"));
        if (!hasError)
        {
            if (const auto error = reader.finish())
                return *error;
            return std::optional<BoundaryError>{};
        }
        BoundaryError error;
        if (!readBoundaryError(reader, error))
            return malformed(QStringLiteral("invalid activation error"));
        if (const auto streamError = reader.finish())
            return *streamError;
        return std::optional<BoundaryError>{std::move(error)};
    }

} // namespace javelin::protocol
