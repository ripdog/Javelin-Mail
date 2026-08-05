#include "protocol/SocketTransport.h"

#include <QAbstractSocket>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDevice>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QLocalServer>
#include <QLocalSocket>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QTimer>

#include <algorithm>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>

#ifdef Q_OS_LINUX
#include <sys/socket.h>
#include <unistd.h>
#elif defined(Q_OS_MACOS)
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace javelin::protocol
{
    namespace
    {
        constexpr std::size_t frameHeaderBytes = 24;
        constexpr quint16 wireVersion = 2;
        constexpr char frameMagic[] = {'J', 'V', 'I', 'P'};

        [[nodiscard]] quint16 readU16(const QByteArray& bytes, const int offset)
        {
            const auto high =
                static_cast<unsigned int>(static_cast<unsigned char>(bytes.at(offset)));
            const auto low =
                static_cast<unsigned int>(static_cast<unsigned char>(bytes.at(offset + 1)));
            return static_cast<quint16>((high << 8U) | low);
        }

        [[nodiscard]] quint32 readU32(const QByteArray& bytes, const int offset)
        {
            quint32 value = 0;
            for (int index = 0; index < 4; ++index)
                value = (value << 8) |
                        static_cast<quint32>(static_cast<unsigned char>(bytes.at(offset + index)));
            return value;
        }

        [[nodiscard]] quint64 readU64(const QByteArray& bytes, const int offset)
        {
            quint64 value = 0;
            for (int index = 0; index < 8; ++index)
                value = (value << 8) |
                        static_cast<quint64>(static_cast<unsigned char>(bytes.at(offset + index)));
            return value;
        }

        void writeU16(char* target, const quint16 value)
        {
            target[0] = static_cast<char>((value >> 8) & 0xff);
            target[1] = static_cast<char>(value & 0xff);
        }

        void writeU32(char* target, const quint32 value)
        {
            for (int index = 0; index < 4; ++index)
                target[index] = static_cast<char>((value >> (24 - index * 8)) & 0xff);
        }

        void writeU64(char* target, const quint64 value)
        {
            for (int index = 0; index < 8; ++index)
                target[index] = static_cast<char>((value >> (56 - index * 8)) & 0xff);
        }

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
                   writer.string(account.loginEmail) && writer.string(account.apiKey) &&
                   writer.string(account.refreshToken) && writer.string(account.tokenEndpoint) &&
                   writer.string(account.oauthClientId) && writer.string(account.oauthIssuer) &&
                   writer.string(account.oauthResource) && writer.string(account.oauthScope) &&
                   writer.string(account.revocationEndpoint) &&
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
                !reader.string(account.loginEmail) || !reader.string(account.apiKey) ||
                !reader.string(account.refreshToken) || !reader.string(account.tokenEndpoint) ||
                !reader.string(account.oauthClientId) || !reader.string(account.oauthIssuer) ||
                !reader.string(account.oauthResource) || !reader.string(account.oauthScope) ||
                !reader.string(account.revocationEndpoint) || !reader.qword(expiresAt) ||
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
                   writeVector(
                       writer, workspace.calendarColorOverrides, limits.maximumCollectionItems,
                       QStringLiteral("workspace.calendarColorOverrides"),
                       [&writer](const CalendarColorOverride& value)
                       { return writer.string(value.calendarId) && writer.string(value.color); });
        }

        bool readWorkspaceSettings(PayloadReader& reader, WorkspaceSettings& workspace,
                                   const BoundaryLimits& limits)
        {
            if (!reader.dword(workspace.formatVersion) ||
                !reader.bytes(workspace.mainWindowState) ||
                static_cast<std::size_t>(workspace.mainWindowState.size()) >
                    limits.maximumWorkspaceBytes)
                return false;
            return readVector(
                reader, workspace.calendarColorOverrides, limits.maximumCollectionItems,
                QStringLiteral("workspace.calendarColorOverrides"),
                [&reader](CalendarColorOverride& value)
                { return reader.string(value.calendarId) && reader.string(value.color); });
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
                                            return writer.word(
                                                       static_cast<quint16>(command.kind)) &&
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
                    if (!reader.word(actionKind) ||
                        actionKind > static_cast<quint16>(RemoteActionKind::Last) ||
                        !reader.bytes(command.payload))
                        return malformed(QStringLiteral("invalid remote action payload"));
                    command.kind = static_cast<RemoteActionKind>(actionKind);
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

        bool writeActivationRoute(PayloadWriter& writer, const ActivationRoute& route)
        {
            if (!writer.byte(static_cast<quint8>(route.index())))
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
                    else if constexpr (std::is_same_v<Route, RestoreDraftRoute>)
                        return writer.string(value.accountId) &&
                               writer.string(value.draftEmailId) &&
                               writer.string(value.composeSessionId) &&
                               writer.string(value.activationToken);
                    else if constexpr (std::is_same_v<Route, OpenTaskCenterRoute>)
                        return writer.string(value.activationToken);
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

        [[nodiscard]] std::optional<SocketTransportError>
        validateRuntimeDirectory(const SocketEndpointOptions& options)
        {
            if (options.runtimeDirectory.isEmpty() || options.socketPath.isEmpty())
            {
                return SocketTransportError{
                    .reason = SocketDisconnectReason::TransportFailure,
                    .detail = QStringLiteral("runtime directory and socket path are required")};
            }
            const QFileInfo directoryInfo{options.runtimeDirectory};
            if (!directoryInfo.exists() || !directoryInfo.isDir())
            {
                return SocketTransportError{.reason = SocketDisconnectReason::TransportFailure,
                                            .detail =
                                                QStringLiteral("runtime directory does not exist")};
            }
            const auto permissions = directoryInfo.permissions();
            if (directoryInfo.ownerId() != static_cast<uint>(::geteuid()) ||
                !(permissions & QFileDevice::ReadOwner) ||
                !(permissions & QFileDevice::WriteOwner) ||
                !(permissions & QFileDevice::ExeOwner) ||
                (permissions &
                 (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
                  QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther)))
            {
                return SocketTransportError{
                    .reason = SocketDisconnectReason::CredentialFailure,
                    .detail = QStringLiteral("runtime directory must be a private directory")};
            }
            const QString runtime = QDir::cleanPath(options.runtimeDirectory);
            const QString socket = QDir::cleanPath(options.socketPath);
            const QString relative = QDir{runtime}.relativeFilePath(socket);
            if (relative.isEmpty() || relative == QStringLiteral("..") ||
                relative.startsWith(QStringLiteral("../")))
            {
                return SocketTransportError{
                    .reason = SocketDisconnectReason::CredentialFailure,
                    .detail = QStringLiteral("socket path must be inside the runtime directory")};
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<SocketTransportError>
        listenOnLocalServer(QLocalServer& server, const SocketEndpointOptions& options,
                            QString addressInUseDetail)
        {
            if (const auto error = validateRuntimeDirectory(options))
                return error;
            if (server.isListening())
                return std::nullopt;
            if (server.listen(options.socketPath))
                return std::nullopt;
            if (server.serverError() != QAbstractSocket::AddressInUseError)
            {
                return SocketTransportError{
                    .reason = SocketDisconnectReason::TransportFailure,
                    .detail = server.errorString(),
                };
            }

            QLocalSocket probe;
            probe.connectToServer(options.socketPath);
            if (probe.waitForConnected(100))
            {
                return SocketTransportError{
                    .reason = SocketDisconnectReason::TransportFailure,
                    .detail = std::move(addressInUseDetail),
                };
            }

            QLocalServer::removeServer(options.socketPath);
            if (server.listen(options.socketPath))
                return std::nullopt;
            return SocketTransportError{
                .reason = SocketDisconnectReason::TransportFailure,
                .detail = server.errorString(),
            };
        }

        void closeLocalServer(QLocalServer* server, const QString& socketPath)
        {
            if (server == nullptr)
                return;
            const bool wasListening = server->isListening();
            server->close();
            if (wasListening && !socketPath.isEmpty())
                QLocalServer::removeServer(socketPath);
        }

        [[nodiscard]] std::optional<SocketTransportError>
        validatePeerCredentials(QLocalSocket& socket, const bool enforce)
        {
            if (!enforce)
                return std::nullopt;
#ifdef Q_OS_LINUX
            struct ucred peer;
            socklen_t length = sizeof(peer);
            const int descriptor = static_cast<int>(socket.socketDescriptor());
            if (descriptor < 0 ||
                getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &peer, &length) != 0)
            {
                return SocketTransportError{
                    .reason = SocketDisconnectReason::CredentialFailure,
                    .detail = QStringLiteral("could not inspect local socket credentials")};
            }
            if (peer.uid != static_cast<uid_t>(::geteuid()))
            {
                return SocketTransportError{
                    .reason = SocketDisconnectReason::CredentialFailure,
                    .detail = QStringLiteral("local socket peer has a different user id")};
            }
#elif defined(Q_OS_MACOS)
            uid_t uid = 0;
            gid_t gid = 0;
            if (getpeereid(socket.socketDescriptor(), &uid, &gid) != 0 ||
                uid != static_cast<uid_t>(::geteuid()))
            {
                return SocketTransportError{
                    .reason = SocketDisconnectReason::CredentialFailure,
                    .detail = QStringLiteral("local socket peer credentials are invalid")};
            }
#else
            Q_UNUSED(socket);
#endif
            return std::nullopt;
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

    SocketFrameDecoder::SocketFrameDecoder(BoundaryLimits limits) : m_limits(limits)
    {
    }

    std::optional<SocketFrameError> SocketFrameDecoder::append(const QByteArray& bytes)
    {
        const auto maximumBuffer =
            m_limits.maximumFrameBytes > std::numeric_limits<std::size_t>::max() / 2
                ? std::numeric_limits<std::size_t>::max()
                : m_limits.maximumFrameBytes * 2;
        if (static_cast<std::size_t>(m_buffer.size()) + static_cast<std::size_t>(bytes.size()) >
            maximumBuffer)
        {
            return SocketFrameError{.code = SocketFrameErrorCode::FrameTooLarge,
                                    .detail = QStringLiteral("socket input buffer is too large")};
        }
        m_buffer.append(bytes);
        return std::nullopt;
    }

    std::variant<std::optional<SocketFrame>, SocketFrameError> SocketFrameDecoder::takeFrame()
    {
        if (m_buffer.size() < static_cast<qsizetype>(frameHeaderBytes))
            return std::optional<SocketFrame>{};
        if (std::memcmp(m_buffer.constData(), frameMagic, sizeof(frameMagic)) != 0)
            return SocketFrameError{.code = SocketFrameErrorCode::InvalidHeader,
                                    .detail = QStringLiteral("socket frame magic is invalid")};
        if (readU16(m_buffer, 4) != wireVersion)
            return SocketFrameError{.code = SocketFrameErrorCode::UnsupportedVersion,
                                    .detail = QStringLiteral("socket wire version is unsupported")};
        if (readU16(m_buffer, 8) != 0 || readU16(m_buffer, 10) != 0)
            return SocketFrameError{.code = SocketFrameErrorCode::InvalidHeader,
                                    .detail = QStringLiteral("socket frame flags are invalid")};
        const auto rawKind = readU16(m_buffer, 6);
        if (!isKnownSocketFrameKind(rawKind))
            return SocketFrameError{.code = SocketFrameErrorCode::UnknownMessageKind,
                                    .detail = QStringLiteral("socket message kind is unknown")};
        const auto payloadLength = static_cast<std::size_t>(readU32(m_buffer, 12));
        if (m_limits.maximumFrameBytes < frameHeaderBytes ||
            payloadLength > m_limits.maximumFrameBytes - frameHeaderBytes)
            return SocketFrameError{.code = SocketFrameErrorCode::FrameTooLarge,
                                    .detail =
                                        QStringLiteral("socket frame exceeds the size limit")};
        const auto frameLength = frameHeaderBytes + payloadLength;
        if (static_cast<std::size_t>(m_buffer.size()) < frameLength)
            return std::optional<SocketFrame>{};
        SocketFrame frame{.kind = static_cast<SocketFrameKind>(rawKind),
                          .correlation = readU64(m_buffer, 16),
                          .payload = m_buffer.mid(static_cast<qsizetype>(frameHeaderBytes),
                                                  static_cast<qsizetype>(payloadLength))};
        m_buffer.remove(0, static_cast<qsizetype>(frameLength));
        return frame;
    }

    void SocketFrameDecoder::clear()
    {
        m_buffer.clear();
    }

    bool isKnownSocketFrameKind(const std::uint16_t kind)
    {
        switch (static_cast<SocketFrameKind>(kind))
        {
        case SocketFrameKind::HelloRequest:
        case SocketFrameKind::CommandRequest:
        case SocketFrameKind::MaterializationRequest:
        case SocketFrameKind::CancelMaterializationScopeRequest:
        case SocketFrameKind::GetSettingsRequest:
        case SocketFrameKind::UpdateSettingsRequest:
        case SocketFrameKind::CacheAccessSuspendedAcknowledgement:
        case SocketFrameKind::PingRequest:
        case SocketFrameKind::ReadyForActivationRequest:
        case SocketFrameKind::ActivationRequest:
        case SocketFrameKind::HelloReply:
        case SocketFrameKind::CommandReplyFrame:
        case SocketFrameKind::MaterializationReplyFrame:
        case SocketFrameKind::CancelMaterializationScopeReply:
        case SocketFrameKind::SettingsReadReplyFrame:
        case SocketFrameKind::SettingsUpdateReplyFrame:
        case SocketFrameKind::CacheAccessSuspendedReply:
        case SocketFrameKind::PingReply:
        case SocketFrameKind::ReadyForActivationReply:
        case SocketFrameKind::ActivationReply:
        case SocketFrameKind::BoundaryEventFrame:
        case SocketFrameKind::ProtocolError:
            return true;
        }
        return false;
    }

    std::variant<QByteArray, SocketFrameError>
    encodeSocketFrame(const SocketFrameKind kind, const std::uint64_t correlation,
                      const QByteArray& payload, const std::size_t maximumFrameBytes)
    {
        if (!isKnownSocketFrameKind(static_cast<std::uint16_t>(kind)))
            return SocketFrameError{.code = SocketFrameErrorCode::UnknownMessageKind,
                                    .detail = QStringLiteral("socket message kind is unknown")};
        if (maximumFrameBytes < frameHeaderBytes ||
            static_cast<std::size_t>(payload.size()) > maximumFrameBytes - frameHeaderBytes)
            return SocketFrameError{.code = SocketFrameErrorCode::FrameTooLarge,
                                    .detail =
                                        QStringLiteral("socket frame exceeds the size limit")};
        QByteArray frame;
        frame.resize(static_cast<qsizetype>(frameHeaderBytes));
        std::memcpy(frame.data(), frameMagic, sizeof(frameMagic));
        writeU16(frame.data() + 4, wireVersion);
        writeU16(frame.data() + 6, static_cast<quint16>(kind));
        writeU16(frame.data() + 8, 0);
        writeU16(frame.data() + 10, 0);
        writeU32(frame.data() + 12, static_cast<quint32>(payload.size()));
        writeU64(frame.data() + 16, correlation);
        frame.append(payload);
        return frame;
    }

    std::variant<QByteArray, SocketFrameError> encodeActivationRoute(const ActivationRoute& route,
                                                                     const BoundaryLimits& limits)
    {
        const auto encoded = makePayload(
            SocketFrameKind::ActivationRequest, limits,
            [&route](PayloadWriter& writer)
            {
                return std::visit(
                    [&writer](const auto& value)
                    {
                        using Route = std::decay_t<decltype(value)>;
                        if constexpr (std::is_same_v<Route, OpenMailboxRoute>)
                        {
                            return writer.byte(0) && writer.string(value.accountId) &&
                                   writer.string(value.mailboxId) &&
                                   writer.string(value.activationToken);
                        }
                        else if constexpr (std::is_same_v<Route, OpenMessageRoute>)
                        {
                            return writer.byte(1) && writer.string(value.accountId) &&
                                   writer.string(value.mailboxId) &&
                                   writer.string(value.mailboxName) &&
                                   writer.string(value.threadId) && writer.string(value.emailId) &&
                                   writer.string(value.activationToken);
                        }
                        else if constexpr (std::is_same_v<Route, OpenComposeRoute>)
                            return writer.byte(2) && writer.string(value.composeSessionId) &&
                                   writer.string(value.activationToken);
                        else if constexpr (std::is_same_v<Route, RaiseGuiRoute>)
                        {
                            return writer.byte(3) && writer.string(value.activationToken);
                        }
                        else if constexpr (std::is_same_v<Route, OpenSettingsRoute>)
                        {
                            return writer.byte(4) && writer.string(value.connectionId) &&
                                   writer.string(value.activationToken);
                        }
                        else if constexpr (std::is_same_v<Route, RestoreDraftRoute>)
                        {
                            return writer.byte(5) && writer.string(value.accountId) &&
                                   writer.string(value.draftEmailId) &&
                                   writer.string(value.composeSessionId) &&
                                   writer.string(value.activationToken);
                        }
                        else if constexpr (std::is_same_v<Route, OpenTaskCenterRoute>)
                            return writer.byte(6) && writer.string(value.activationToken);
                        else
                        {
                            return false;
                        }
                    },
                    route);
            });
        if (const auto* error = std::get_if<SocketFrameError>(&encoded))
            return *error;
        return std::get<EncodedPayload>(encoded).payload;
    }

    std::variant<ActivationRoute, SocketFrameError>
    decodeActivationRoute(const QByteArray& payload, const BoundaryLimits& limits)
    {
        PayloadReader reader{payload, limits};
        quint8 routeIndex = 0;
        if (!reader.byte(routeIndex))
            return malformed(QStringLiteral("invalid activation route"));
        if (routeIndex == 0)
        {
            OpenMailboxRoute route;
            if (!reader.string(route.accountId) || !reader.string(route.mailboxId) ||
                !reader.string(route.activationToken))
                return malformed(QStringLiteral("invalid mailbox activation route"));
            if (const auto error = reader.finish())
                return *error;
            return ActivationRoute{std::move(route)};
        }
        if (routeIndex == 1)
        {
            OpenMessageRoute route;
            if (!reader.string(route.accountId) || !reader.string(route.mailboxId) ||
                !reader.string(route.mailboxName) || !reader.string(route.threadId) ||
                !reader.string(route.emailId) || !reader.string(route.activationToken))
                return malformed(QStringLiteral("invalid message activation route"));
            if (const auto error = reader.finish())
                return *error;
            return ActivationRoute{std::move(route)};
        }
        if (routeIndex == 2)
        {
            OpenComposeRoute route;
            if (!reader.string(route.composeSessionId) || !reader.string(route.activationToken))
                return malformed(QStringLiteral("invalid compose activation route"));
            if (const auto error = reader.finish())
                return *error;
            return ActivationRoute{std::move(route)};
        }
        if (routeIndex == 3)
        {
            RaiseGuiRoute route;
            if (!reader.string(route.activationToken))
                return malformed(QStringLiteral("invalid raise activation route"));
            if (const auto error = reader.finish())
                return *error;
            return ActivationRoute{std::move(route)};
        }
        if (routeIndex == 4)
        {
            OpenSettingsRoute route;
            if (!reader.string(route.connectionId) || !reader.string(route.activationToken))
                return malformed(QStringLiteral("invalid settings activation route"));
            if (const auto error = reader.finish())
                return *error;
            return ActivationRoute{std::move(route)};
        }
        if (routeIndex == 5)
        {
            RestoreDraftRoute route;
            if (!reader.string(route.accountId) || !reader.string(route.draftEmailId) ||
                !reader.string(route.composeSessionId) || !reader.string(route.activationToken))
                return malformed(QStringLiteral("invalid draft activation route"));
            if (const auto error = reader.finish())
                return *error;
            return ActivationRoute{std::move(route)};
        }
        if (routeIndex == 6)
        {
            OpenTaskCenterRoute route;
            if (!reader.string(route.activationToken))
                return malformed(QStringLiteral("invalid task center activation route"));
            if (const auto error = reader.finish())
                return *error;
            return ActivationRoute{std::move(route)};
        }
        return malformed(QStringLiteral("unknown activation route variant"));
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

    SocketDaemonEndpoint::SocketDaemonEndpoint(DaemonRequestHandler& handler,
                                               SocketEndpointOptions options, QObject* parent)
        : QObject(parent), m_handler(handler), m_options(std::move(options)),
          m_decoder(m_options.limits)
    {
    }

    SocketDaemonEndpoint::~SocketDaemonEndpoint()
    {
        close();
    }

    std::optional<SocketTransportError> SocketDaemonEndpoint::listen()
    {
        if (m_server == nullptr)
        {
            m_server = std::make_unique<QLocalServer>();
            m_server->setSocketOptions(QLocalServer::UserAccessOption);
            connect(m_server.get(), &QLocalServer::newConnection, this,
                    &SocketDaemonEndpoint::acceptConnection);
        }
        m_lastError = listenOnLocalServer(*m_server, m_options,
                                          QStringLiteral("socket path is already in use"));
        return m_lastError;
    }

    void SocketDaemonEndpoint::close()
    {
        if (m_socket != nullptr)
            disconnect(SocketDisconnectReason::ServerShutdown,
                       QStringLiteral("socket endpoint is shutting down"));
        closeLocalServer(m_server.get(), m_options.socketPath);
    }

    std::optional<SocketTransportError> SocketDaemonEndpoint::lastError() const
    {
        return m_lastError;
    }

    void SocketDaemonEndpoint::publishEvent(const BoundaryEvent& event)
    {
        if (m_socket == nullptr || !m_handshakeComplete)
            return;
        if (!enqueueEvent(event))
            disconnect(SocketDisconnectReason::QueueOverflow,
                       QStringLiteral("socket output queue cannot carry the boundary event"));
    }

    void SocketDaemonEndpoint::acceptConnection()
    {
        while (m_server != nullptr && m_server->hasPendingConnections())
        {
            auto* candidate = m_server->nextPendingConnection();
            if (candidate == nullptr)
                continue;
            if (m_socket != nullptr)
            {
                candidate->disconnectFromServer();
                candidate->deleteLater();
                continue;
            }
            if (const auto error =
                    validatePeerCredentials(*candidate, m_options.enforcePeerCredentials))
            {
                candidate->disconnectFromServer();
                candidate->deleteLater();
                m_lastError = error;
                continue;
            }
            m_socket.reset(candidate);
            m_socket->setReadBufferSize(static_cast<qint64>(m_options.limits.maximumFrameBytes));
            connect(m_socket.get(), &QLocalSocket::readyRead, this,
                    &SocketDaemonEndpoint::readSocket);
            connect(m_socket.get(), &QLocalSocket::bytesWritten, this,
                    &SocketDaemonEndpoint::writeSocket);
            connect(m_socket.get(), &QLocalSocket::disconnected, this,
                    &SocketDaemonEndpoint::socketDisconnected);
            connect(m_socket.get(), &QLocalSocket::errorOccurred, this,
                    &SocketDaemonEndpoint::socketError);
            m_decoder.clear();
            m_handshakeComplete = false;
            m_lastError.reset();
            Q_EMIT connectionOpened();
        }
    }

    void SocketDaemonEndpoint::readSocket()
    {
        const QScopedValueRollback callbackGuard{m_inSocketCallback, true};
        if (m_socket == nullptr)
            return;
        if (const auto error = m_decoder.append(m_socket->readAll()))
        {
            sendProtocolError(0, *error);
            return;
        }
        while (m_socket != nullptr && !m_processingFrame)
        {
            auto decoded = m_decoder.takeFrame();
            if (auto* error = std::get_if<SocketFrameError>(&decoded))
            {
                sendProtocolError(0, *error);
                return;
            }
            auto& frame = std::get<std::optional<SocketFrame>>(decoded);
            if (!frame.has_value())
                return;
            m_processingFrame = true;
            handleFrame(*frame);
            m_processingFrame = false;
        }
    }

    void SocketDaemonEndpoint::writeSocket(const qint64)
    {
        const QScopedValueRollback callbackGuard{m_inSocketCallback, true};
        pumpWrites();
    }

    void SocketDaemonEndpoint::socketDisconnected()
    {
        const QScopedValueRollback callbackGuard{m_inSocketCallback, true};
        if (m_socket != nullptr)
            disconnect(SocketDisconnectReason::PeerClosed,
                       QStringLiteral("socket peer disconnected"));
    }

    void SocketDaemonEndpoint::socketError()
    {
        const QScopedValueRollback callbackGuard{m_inSocketCallback, true};
        if (m_socket != nullptr)
            disconnect(SocketDisconnectReason::TransportFailure, m_socket->errorString());
    }

    void SocketDaemonEndpoint::handleFrame(const SocketFrame& frame)
    {
        const auto rawKind = static_cast<std::uint16_t>(frame.kind);
        if (frame.correlation == 0)
        {
            sendProtocolError(0, malformed(QStringLiteral("request correlation is zero")));
            return;
        }
        if (frame.kind == SocketFrameKind::ProtocolError ||
            rawKind >= static_cast<std::uint16_t>(SocketFrameKind::HelloReply))
        {
            sendProtocolError(frame.correlation,
                              malformed(QStringLiteral("unexpected socket message kind")));
            return;
        }
        if (frame.kind == SocketFrameKind::ReadyForActivationRequest)
        {
            if (!m_handshakeComplete || !frame.payload.isEmpty())
            {
                sendProtocolError(frame.correlation,
                                  malformed(QStringLiteral("invalid activation request frame")));
                return;
            }
            const auto reply =
                encodeOptionalError(SocketFrameKind::ReadyForActivationReply,
                                    m_handler.handleGuiReadyForActivation(), m_options.limits);
            if (auto* error = std::get_if<SocketFrameError>(&reply))
            {
                disconnect(SocketDisconnectReason::ProtocolViolation, error->detail);
                return;
            }
            const auto frameData = encodeSocketFrame(
                SocketFrameKind::ReadyForActivationReply, frame.correlation,
                std::get<EncodedPayload>(reply).payload, m_options.limits.maximumFrameBytes);
            if (auto* error = std::get_if<SocketFrameError>(&frameData))
            {
                disconnect(SocketDisconnectReason::ProtocolViolation, error->detail);
                return;
            }
            if (!enqueue(PendingFrame{.data = std::get<QByteArray>(frameData),
                                      .kind = SocketFrameKind::ReadyForActivationReply,
                                      .correlation = frame.correlation,
                                      .event = std::nullopt}))
                disconnect(SocketDisconnectReason::QueueOverflow,
                           QStringLiteral("socket output queue is full"));
            return;
        }

        auto decoded = decodeClientRequest(frame.kind, frame.payload, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&decoded))
        {
            sendProtocolError(frame.correlation, *error);
            return;
        }
        auto request = std::move(std::get<ClientRequest>(decoded));
        if (const auto error = validate(request, m_options.limits))
        {
            if (const auto* command = std::get_if<CommandRequest>(&request))
            {
                const auto reply = encodeCommandReply(
                    CommandRejected{.id = command->id, .error = *error}, m_options.limits);
                if (auto* encodedError = std::get_if<SocketFrameError>(&reply))
                    return disconnect(SocketDisconnectReason::ProtocolViolation,
                                      encodedError->detail);
                const auto frameData = encodeSocketFrame(
                    SocketFrameKind::CommandReplyFrame, frame.correlation,
                    std::get<EncodedPayload>(reply).payload, m_options.limits.maximumFrameBytes);
                if (auto* frameError = std::get_if<SocketFrameError>(&frameData))
                    return disconnect(SocketDisconnectReason::ProtocolViolation,
                                      frameError->detail);
                if (!enqueue(PendingFrame{.data = std::get<QByteArray>(frameData),
                                          .kind = SocketFrameKind::CommandReplyFrame,
                                          .correlation = frame.correlation,
                                          .event = std::nullopt}))
                    disconnect(SocketDisconnectReason::QueueOverflow,
                               QStringLiteral("socket output queue is full"));
                return;
            }
            if (const auto* materialization = std::get_if<MaterializationRequest>(&request))
            {
                const auto reply = encodeMaterializationReply(
                    MaterializationRejected{.id = materialization->id, .error = *error},
                    m_options.limits);
                if (auto* encodedError = std::get_if<SocketFrameError>(&reply))
                    return disconnect(SocketDisconnectReason::ProtocolViolation,
                                      encodedError->detail);
                const auto frameData = encodeSocketFrame(
                    SocketFrameKind::MaterializationReplyFrame, frame.correlation,
                    std::get<EncodedPayload>(reply).payload, m_options.limits.maximumFrameBytes);
                if (auto* frameError = std::get_if<SocketFrameError>(&frameData))
                    return disconnect(SocketDisconnectReason::ProtocolViolation,
                                      frameError->detail);
                if (!enqueue(PendingFrame{.data = std::get<QByteArray>(frameData),
                                          .kind = SocketFrameKind::MaterializationReplyFrame,
                                          .correlation = frame.correlation,
                                          .event = std::nullopt}))
                    disconnect(SocketDisconnectReason::QueueOverflow,
                               QStringLiteral("socket output queue is full"));
                return;
            }
            sendProtocolError(frame.correlation,
                              malformed(QStringLiteral("request validation failed")));
            return;
        }

        if (const auto* hello = std::get_if<HelloRequest>(&request))
        {
            if (m_handshakeComplete)
            {
                sendProtocolError(frame.correlation,
                                  malformed(QStringLiteral("HELLO was sent twice")));
                return;
            }
            if (hello->protocol.major != m_options.protocol.major ||
                hello->protocol.minor > m_options.protocol.minor ||
                (m_options.expectedBuild.has_value() && hello->build != *m_options.expectedBuild))
            {
                HandshakeRejected rejected{
                    .error = {.code = hello->protocol.major != m_options.protocol.major
                                          ? BoundaryErrorCode::InvalidProtocol
                                          : BoundaryErrorCode::IncompatibleBuild,
                              .field = QStringLiteral("hello"),
                              .detail = QStringLiteral(
                                  "socket peer is not compatible with this daemon")}};
                const auto reply = encodeHandshakeReply(rejected, m_options.limits);
                if (auto* error = std::get_if<SocketFrameError>(&reply))
                    return disconnect(SocketDisconnectReason::ProtocolViolation, error->detail);
                const auto frameData = encodeSocketFrame(
                    SocketFrameKind::HelloReply, frame.correlation,
                    std::get<EncodedPayload>(reply).payload, m_options.limits.maximumFrameBytes);
                if (auto* error = std::get_if<SocketFrameError>(&frameData))
                    return disconnect(SocketDisconnectReason::ProtocolViolation, error->detail);
                if (!enqueue(PendingFrame{.data = std::get<QByteArray>(frameData),
                                          .kind = SocketFrameKind::HelloReply,
                                          .correlation = frame.correlation,
                                          .event = std::nullopt}))
                    disconnect(SocketDisconnectReason::QueueOverflow,
                               QStringLiteral("socket output queue is full"));
                return;
            }
            const auto reply = m_handler.handleHello(*hello);
            const auto encoded = encodeHandshakeReply(reply, m_options.limits);
            if (auto* error = std::get_if<SocketFrameError>(&encoded))
                return disconnect(SocketDisconnectReason::ProtocolViolation, error->detail);
            const auto frameData = encodeSocketFrame(SocketFrameKind::HelloReply, frame.correlation,
                                                     std::get<EncodedPayload>(encoded).payload,
                                                     m_options.limits.maximumFrameBytes);
            if (auto* error = std::get_if<SocketFrameError>(&frameData))
                return disconnect(SocketDisconnectReason::ProtocolViolation, error->detail);
            if (isReadyReply(reply))
                m_handshakeComplete = true;
            else
            {
                // A rejected handshake is returned before the connection is retired.
                m_closeAfterWrites = true;
                m_closeReason = SocketDisconnectReason::IncompatiblePeer;
                m_closeDetail = QStringLiteral("daemon rejected the socket handshake");
            }
            if (!enqueue(PendingFrame{.data = std::get<QByteArray>(frameData),
                                      .kind = SocketFrameKind::HelloReply,
                                      .correlation = frame.correlation,
                                      .event = std::nullopt}))
                disconnect(SocketDisconnectReason::QueueOverflow,
                           QStringLiteral("socket output queue is full"));
            return;
        }

        if (!m_handshakeComplete)
        {
            sendProtocolError(frame.correlation,
                              malformed(QStringLiteral("HELLO is required before requests")));
            return;
        }

        const auto enqueueReply =
            [this, &frame](const SocketFrameKind kind, const EncodedPayloadResult& encoded)
        {
            if (auto* error = std::get_if<SocketFrameError>(&encoded))
            {
                disconnect(SocketDisconnectReason::ProtocolViolation, error->detail);
                return;
            }
            const auto frameData = encodeSocketFrame(kind, frame.correlation,
                                                     std::get<EncodedPayload>(encoded).payload,
                                                     m_options.limits.maximumFrameBytes);
            if (auto* error = std::get_if<SocketFrameError>(&frameData))
            {
                disconnect(SocketDisconnectReason::ProtocolViolation, error->detail);
                return;
            }
            if (!enqueue(PendingFrame{.data = std::get<QByteArray>(frameData),
                                      .kind = kind,
                                      .correlation = frame.correlation,
                                      .event = std::nullopt}))
                disconnect(SocketDisconnectReason::QueueOverflow,
                           QStringLiteral("socket output queue is full"));
        };

        std::visit(
            [this, &enqueueReply](auto&& value)
            {
                using Request = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Request, CommandRequest>)
                    enqueueReply(SocketFrameKind::CommandReplyFrame,
                                 encodeCommandReply(m_handler.handleCommand(std::move(value)),
                                                    m_options.limits));
                else if constexpr (std::is_same_v<Request, MaterializationRequest>)
                {
                    m_activeScopes.push_back(value.scope);
                    enqueueReply(
                        SocketFrameKind::MaterializationReplyFrame,
                        encodeMaterializationReply(
                            m_handler.handleMaterialization(std::move(value)), m_options.limits));
                }
                else if constexpr (std::is_same_v<Request, CancelMaterializationScopeRequest>)
                {
                    m_handler.handleCancelMaterializationScope(value);
                    std::erase(m_activeScopes, value.scope);
                    enqueueReply(
                        SocketFrameKind::CancelMaterializationScopeReply,
                        encodeOptionalError(SocketFrameKind::CancelMaterializationScopeReply,
                                            std::nullopt, m_options.limits));
                }
                else if constexpr (std::is_same_v<Request, GetSettingsRequest>)
                    enqueueReply(SocketFrameKind::SettingsReadReplyFrame,
                                 encodeSettingsReadReply(m_handler.handleGetSettings(value),
                                                         m_options.limits));
                else if constexpr (std::is_same_v<Request, UpdateSettingsRequest>)
                    enqueueReply(
                        SocketFrameKind::SettingsUpdateReplyFrame,
                        encodeSettingsUpdateReply(m_handler.handleUpdateSettings(std::move(value)),
                                                  m_options.limits));
                else if constexpr (std::is_same_v<Request, CacheAccessSuspendedAcknowledgement>)
                    enqueueReply(SocketFrameKind::CacheAccessSuspendedReply,
                                 encodeOptionalError(SocketFrameKind::CacheAccessSuspendedReply,
                                                     m_handler.handleCacheAccessSuspended(value),
                                                     m_options.limits));
                else if constexpr (std::is_same_v<Request, PingRequest>)
                    enqueueReply(SocketFrameKind::PingReply,
                                 encodeOptionalError(SocketFrameKind::PingReply,
                                                     m_handler.handlePing(value),
                                                     m_options.limits));
            },
            std::move(request));
    }

    void SocketDaemonEndpoint::sendProtocolError(const std::uint64_t correlation,
                                                 const SocketFrameError& error)
    {
        const auto payload = encodeProtocolError(error, m_options.limits);
        if (std::holds_alternative<SocketFrameError>(payload))
            return;
        const auto frameData = encodeSocketFrame(SocketFrameKind::ProtocolError, correlation,
                                                 std::get<EncodedPayload>(payload).payload,
                                                 m_options.limits.maximumFrameBytes);
        if (std::holds_alternative<SocketFrameError>(frameData))
            return;
        m_closeAfterWrites = true;
        m_closeReason = SocketDisconnectReason::ProtocolViolation;
        m_closeDetail = error.detail;
        if (!enqueue(PendingFrame{.data = std::get<QByteArray>(frameData),
                                  .kind = SocketFrameKind::ProtocolError,
                                  .correlation = correlation,
                                  .event = std::nullopt}))
            disconnect(SocketDisconnectReason::QueueOverflow,
                       QStringLiteral("socket output queue is full"));
    }

    bool SocketDaemonEndpoint::enqueue(PendingFrame frame)
    {
        const auto frameBytes = static_cast<std::size_t>(frame.data.size());
        if (m_pendingWrites.size() + (m_currentWrite != nullptr ? 1U : 0U) >=
                m_options.maximumQueuedFrames ||
            m_queuedBytes + frameBytes > m_options.maximumQueuedBytes)
            return false;
        m_queuedBytes += frameBytes;
        m_pendingWrites.push_back(std::move(frame));
        pumpWrites();
        return true;
    }

    bool SocketDaemonEndpoint::enqueueEvent(const BoundaryEvent& event)
    {
        const auto encoded = encodeBoundaryEvent(event, m_options.limits);
        if (std::holds_alternative<SocketFrameError>(encoded))
            return false;
        const auto frameData = encodeSocketFrame(SocketFrameKind::BoundaryEventFrame, 0,
                                                 std::get<EncodedPayload>(encoded).payload,
                                                 m_options.limits.maximumFrameBytes);
        if (std::holds_alternative<SocketFrameError>(frameData))
            return false;
        const auto coalescible = std::holds_alternative<CacheInvalidation>(event) ||
                                 std::holds_alternative<DaemonStatusChanged>(event) ||
                                 std::holds_alternative<SettingsUpdated>(event);
        if (coalescible)
        {
            for (auto iterator = m_pendingWrites.rbegin(); iterator != m_pendingWrites.rend();
                 ++iterator)
            {
                if (!sameEventKind(iterator->event, std::optional<BoundaryEvent>{event}))
                    continue;
                if (auto* target = std::get_if<CacheInvalidation>(&*iterator->event))
                {
                    const auto& source = std::get<CacheInvalidation>(event);
                    if (target->accountId != source.accountId)
                        continue;
                    mergeInvalidation(*target, source, m_options.limits);
                    const auto merged = encodeBoundaryEvent(*iterator->event, m_options.limits);
                    if (std::holds_alternative<SocketFrameError>(merged))
                        return false;
                    const auto mergedFrame =
                        encodeSocketFrame(SocketFrameKind::BoundaryEventFrame, 0,
                                          std::get<EncodedPayload>(merged).payload,
                                          m_options.limits.maximumFrameBytes);
                    if (std::holds_alternative<SocketFrameError>(mergedFrame))
                        return false;
                    m_queuedBytes -= static_cast<std::size_t>(iterator->data.size());
                    iterator->data = std::get<QByteArray>(mergedFrame);
                    m_queuedBytes += static_cast<std::size_t>(iterator->data.size());
                }
                else
                {
                    m_queuedBytes -= static_cast<std::size_t>(iterator->data.size());
                    iterator->data = std::get<QByteArray>(frameData);
                    iterator->event = event;
                    m_queuedBytes += static_cast<std::size_t>(iterator->data.size());
                }
                return true;
            }
        }
        PendingFrame frame{.data = std::get<QByteArray>(frameData),
                           .kind = SocketFrameKind::BoundaryEventFrame,
                           .correlation = 0,
                           .coalescible = coalescible,
                           .event = event};
        if (coalescible && (m_pendingWrites.size() + (m_currentWrite != nullptr ? 1U : 0U) >=
                                m_options.maximumQueuedFrames ||
                            m_queuedBytes + static_cast<std::size_t>(frame.data.size()) >
                                m_options.maximumQueuedBytes))
        {
            if (std::holds_alternative<DaemonStatusChanged>(event) ||
                std::holds_alternative<SettingsUpdated>(event))
                return true;
        }
        return enqueue(std::move(frame));
    }

    void SocketDaemonEndpoint::pumpWrites()
    {
        if (m_socket == nullptr)
            return;
        if (m_currentWrite != nullptr)
        {
            auto& completed = *m_currentWrite;
            if (completed.offset != static_cast<std::size_t>(completed.data.size()) ||
                m_socket->bytesToWrite() != 0)
                return;
            m_queuedBytes -= static_cast<std::size_t>(completed.data.size());
            m_currentWrite.reset();
        }
        if (m_pendingWrites.empty())
        {
            if (m_closeAfterWrites)
                disconnect(m_closeReason, m_closeDetail);
            return;
        }
        m_currentWrite = std::make_unique<PendingFrame>(std::move(m_pendingWrites.front()));
        m_pendingWrites.pop_front();
        auto& frame = *m_currentWrite;
        while (frame.offset < static_cast<std::size_t>(frame.data.size()))
        {
            const auto remaining = frame.data.size() - static_cast<qsizetype>(frame.offset);
            const auto written = m_socket->write(frame.data.constData() + frame.offset, remaining);
            if (written < 0)
            {
                disconnect(SocketDisconnectReason::TransportFailure, m_socket->errorString());
                return;
            }
            if (written == 0)
                break;
            frame.offset += static_cast<std::size_t>(written);
        }
        if (frame.offset == static_cast<std::size_t>(frame.data.size()) &&
            m_socket->bytesToWrite() == 0)
        {
            m_queuedBytes -= static_cast<std::size_t>(frame.data.size());
            m_currentWrite.reset();
            pumpWrites();
        }
    }

    void SocketDaemonEndpoint::disconnect(const SocketDisconnectReason reason, QString detail)
    {
        if (m_socket == nullptr)
            return;
        if (m_inSocketCallback)
        {
            if (m_disconnectScheduled)
                return;
            m_disconnectScheduled = true;
            QMetaObject::invokeMethod(
                this,
                [this, reason, detail = std::move(detail)]() mutable
                {
                    m_disconnectScheduled = false;
                    disconnect(reason, std::move(detail));
                },
                Qt::QueuedConnection);
            return;
        }
        for (const auto& scope : std::exchange(m_activeScopes, {}))
            m_handler.handleCancelMaterializationScope({.scope = scope});
        if (m_socket != nullptr)
        {
            QSignalBlocker blocker{m_socket.get()};
            m_socket->abort();
        }
        clearSocket();
        m_lastError = SocketTransportError{.reason = reason, .detail = std::move(detail)};
        Q_EMIT connectionClosed(reason, m_lastError->detail);
    }

    void SocketDaemonEndpoint::clearSocket()
    {
        m_socket.reset();
        m_decoder.clear();
        m_pendingWrites.clear();
        m_currentWrite.reset();
        m_queuedBytes = 0;
        m_handshakeComplete = false;
        m_disconnectScheduled = false;
        m_closeAfterWrites = false;
        m_closeReason = SocketDisconnectReason::None;
        m_closeDetail.clear();
    }

    SocketActivationEndpoint::SocketActivationEndpoint(DaemonRequestHandler& handler,
                                                       SocketEndpointOptions options,
                                                       QObject* parent)
        : QObject(parent), m_handler(handler), m_options(std::move(options)),
          m_decoder(m_options.limits)
    {
    }

    SocketActivationEndpoint::~SocketActivationEndpoint()
    {
        close();
    }

    std::optional<SocketTransportError> SocketActivationEndpoint::listen()
    {
        if (m_server == nullptr)
        {
            m_server = std::make_unique<QLocalServer>();
            m_server->setSocketOptions(QLocalServer::UserAccessOption);
            connect(m_server.get(), &QLocalServer::newConnection, this,
                    &SocketActivationEndpoint::acceptConnection);
        }
        m_lastError = listenOnLocalServer(
            *m_server, m_options, QStringLiteral("activation socket path is already in use"));
        return m_lastError;
    }

    void SocketActivationEndpoint::close()
    {
        clearSocket();
        closeLocalServer(m_server.get(), m_options.socketPath);
    }

    void SocketActivationEndpoint::acceptConnection()
    {
        while (m_server != nullptr && m_server->hasPendingConnections())
        {
            auto* candidate = m_server->nextPendingConnection();
            if (candidate == nullptr)
                continue;
            if (m_socket != nullptr)
            {
                candidate->disconnectFromServer();
                candidate->deleteLater();
                continue;
            }
            if (const auto error =
                    validatePeerCredentials(*candidate, m_options.enforcePeerCredentials))
            {
                m_lastError = error;
                candidate->disconnectFromServer();
                candidate->deleteLater();
                continue;
            }
            candidate->setReadBufferSize(static_cast<qint64>(m_options.limits.maximumFrameBytes));
            m_socket.reset(candidate);
            connect(m_socket.get(), &QLocalSocket::readyRead, this,
                    &SocketActivationEndpoint::readSocket);
            connect(m_socket.get(), &QLocalSocket::disconnected, this,
                    &SocketActivationEndpoint::socketDisconnected);
            connect(m_socket.get(), &QLocalSocket::errorOccurred, this,
                    &SocketActivationEndpoint::socketError);
        }
    }

    void SocketActivationEndpoint::readSocket()
    {
        if (m_socket == nullptr)
            return;
        if (const auto error = m_decoder.append(m_socket->readAll()))
        {
            m_lastError = SocketTransportError{.reason = SocketDisconnectReason::ProtocolViolation,
                                               .detail = error->detail};
            clearSocket();
            return;
        }
        auto decoded = m_decoder.takeFrame();
        if (auto* error = std::get_if<SocketFrameError>(&decoded))
        {
            m_lastError = SocketTransportError{.reason = SocketDisconnectReason::ProtocolViolation,
                                               .detail = error->detail};
            clearSocket();
            return;
        }
        const auto& frame = std::get<std::optional<SocketFrame>>(decoded);
        if (!frame.has_value())
            return;
        if (frame->kind != SocketFrameKind::ActivationRequest || frame->correlation == 0)
        {
            m_lastError = SocketTransportError{
                .reason = SocketDisconnectReason::ProtocolViolation,
                .detail = QStringLiteral("invalid activation request frame"),
            };
            clearSocket();
            return;
        }
        const auto request = decodeActivationRequest(frame->payload, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&request))
        {
            m_lastError = SocketTransportError{.reason = SocketDisconnectReason::ProtocolViolation,
                                               .detail = error->detail};
            clearSocket();
            return;
        }
        const auto& requestValue = std::get<DecodedActivationRequest>(request);
        std::optional<BoundaryError> activationError;
        if (requestValue.protocol.major != m_options.protocol.major ||
            requestValue.protocol.minor > m_options.protocol.minor)
        {
            activationError = BoundaryError{
                .code = BoundaryErrorCode::InvalidProtocol,
                .field = QStringLiteral("activation.protocol"),
                .detail = QStringLiteral("daemon protocol is incompatible"),
            };
        }
        else if (m_options.expectedBuild.has_value() &&
                 requestValue.build != *m_options.expectedBuild)
        {
            activationError = BoundaryError{
                .code = BoundaryErrorCode::IncompatibleBuild,
                .field = QStringLiteral("activation.build"),
                .detail = QStringLiteral("daemon build identity is incompatible"),
            };
        }
        else
        {
            activationError = m_handler.handleGuiActivation(requestValue.route);
        }
        const auto payload = encodeActivationReply(activationError, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&payload))
        {
            m_lastError = SocketTransportError{.reason = SocketDisconnectReason::ProtocolViolation,
                                               .detail = error->detail};
            clearSocket();
            return;
        }
        const auto encoded =
            encodeSocketFrame(SocketFrameKind::ActivationReply, frame->correlation,
                              std::get<QByteArray>(payload), m_options.limits.maximumFrameBytes);
        if (auto* error = std::get_if<SocketFrameError>(&encoded))
        {
            m_lastError = SocketTransportError{.reason = SocketDisconnectReason::ProtocolViolation,
                                               .detail = error->detail};
            clearSocket();
            return;
        }
        m_socket->write(std::get<QByteArray>(encoded));
        m_socket->flush();
    }

    void SocketActivationEndpoint::socketDisconnected()
    {
        clearSocket();
    }

    void SocketActivationEndpoint::socketError()
    {
        if (m_socket != nullptr)
        {
            m_lastError = SocketTransportError{.reason = SocketDisconnectReason::TransportFailure,
                                               .detail = m_socket->errorString()};
            clearSocket();
        }
    }

    void SocketActivationEndpoint::clearSocket()
    {
        auto* socket = m_socket.release();
        m_decoder.clear();
        if (socket == nullptr)
            return;
        QSignalBlocker blocker{socket};
        socket->abort();
        socket->deleteLater();
    }

    SocketActivationResult SocketActivationClient::request(const SocketClientOptions& options,
                                                           ActivationRoute route)
    {
        if (const auto error = validateRuntimeDirectory(options))
            return *error;
        QLocalSocket socket;
        socket.connectToServer(options.socketPath);
        if (!socket.waitForConnected(options.responseTimeoutMilliseconds))
            return SocketTransportError{.reason = SocketDisconnectReason::TransportFailure,
                                        .detail = socket.errorString()};
        if (const auto error = validatePeerCredentials(socket, options.enforcePeerCredentials))
            return *error;
        const auto payload = encodeActivationRequest(options, route);
        if (auto* error = std::get_if<SocketFrameError>(&payload))
            return SocketTransportError{.reason = SocketDisconnectReason::ProtocolViolation,
                                        .detail = error->detail};
        const auto frame = encodeSocketFrame(SocketFrameKind::ActivationRequest, 1,
                                             std::get<EncodedPayload>(payload).payload,
                                             options.limits.maximumFrameBytes);
        if (auto* error = std::get_if<SocketFrameError>(&frame))
            return SocketTransportError{.reason = SocketDisconnectReason::ProtocolViolation,
                                        .detail = error->detail};
        const auto bytes = std::get<QByteArray>(frame);
        if (socket.write(bytes) != bytes.size() ||
            !socket.waitForBytesWritten(options.responseTimeoutMilliseconds) ||
            !socket.waitForReadyRead(options.responseTimeoutMilliseconds))
            return SocketTransportError{
                .reason = SocketDisconnectReason::TransportFailure,
                .detail = socket.errorString().isEmpty()
                              ? QStringLiteral("activation reply was not received")
                              : socket.errorString()};
        SocketFrameDecoder decoder{options.limits};
        if (const auto error = decoder.append(socket.readAll()))
            return SocketTransportError{.reason = SocketDisconnectReason::ProtocolViolation,
                                        .detail = error->detail};
        const auto decoded = decoder.takeFrame();
        if (auto* error = std::get_if<SocketFrameError>(&decoded))
            return SocketTransportError{.reason = SocketDisconnectReason::ProtocolViolation,
                                        .detail = error->detail};
        const auto& response = std::get<std::optional<SocketFrame>>(decoded);
        if (!response.has_value() || response->kind != SocketFrameKind::ActivationReply ||
            response->correlation != 1)
            return SocketTransportError{.reason = SocketDisconnectReason::ProtocolViolation,
                                        .detail = QStringLiteral("invalid activation reply")};
        const auto reply = decodeActivationReply(response->payload, options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&reply))
            return SocketTransportError{.reason = SocketDisconnectReason::ProtocolViolation,
                                        .detail = error->detail};
        return std::get<std::optional<BoundaryError>>(reply);
    }

    SocketDaemonClient::SocketDaemonClient(SocketClientOptions options, QObject* parent)
        : QObject(parent), m_options(std::move(options)), m_decoder(m_options.limits)
    {
    }

    SocketDaemonClient::~SocketDaemonClient()
    {
        disconnectFromDaemon();
    }

    std::optional<SocketTransportError> SocketDaemonClient::connectToDaemon()
    {
        if (isConnected())
            return std::nullopt;
        if (const auto error = validateRuntimeDirectory(m_options))
        {
            m_lastError = error;
            return error;
        }
        m_socket = std::make_unique<QLocalSocket>();
        m_socket->setReadBufferSize(static_cast<qint64>(m_options.limits.maximumFrameBytes));
        connect(m_socket.get(), &QLocalSocket::readyRead, this, &SocketDaemonClient::readSocket);
        connect(m_socket.get(), &QLocalSocket::bytesWritten, this,
                &SocketDaemonClient::writeSocket);
        connect(m_socket.get(), &QLocalSocket::disconnected, this,
                &SocketDaemonClient::socketDisconnected);
        connect(m_socket.get(), &QLocalSocket::errorOccurred, this,
                &SocketDaemonClient::socketError);
        m_socket->connectToServer(m_options.socketPath);
        if (!m_socket->waitForConnected(m_options.responseTimeoutMilliseconds))
        {
            const auto error =
                SocketTransportError{.reason = SocketDisconnectReason::TransportFailure,
                                     .detail = m_socket->errorString()};
            clearSocket(error.reason, error.detail);
            return error;
        }
        if (const auto error = validatePeerCredentials(*m_socket, m_options.enforcePeerCredentials))
        {
            clearSocket(error->reason, error->detail);
            return error;
        }
        m_lastError.reset();
        return std::nullopt;
    }

    void SocketDaemonClient::disconnectFromDaemon()
    {
        if (m_socket != nullptr)
            clearSocket(SocketDisconnectReason::PeerClosed, QStringLiteral("client disconnected"));
    }

    bool SocketDaemonClient::isConnected() const
    {
        return m_socket != nullptr && m_socket->state() == QLocalSocket::ConnectedState;
    }

    std::optional<SocketTransportError> SocketDaemonClient::lastError() const
    {
        return m_lastError;
    }

    std::optional<BoundaryError> SocketDaemonClient::attachEventSink(BoundaryEventSink& sink)
    {
        if (m_eventSink != nullptr && m_eventSink != &sink)
            return BoundaryError{.code = BoundaryErrorCode::Busy,
                                 .field = QStringLiteral("eventSink"),
                                 .detail = QStringLiteral("an event sink is already attached")};
        m_eventSink = &sink;
        return std::nullopt;
    }

    void SocketDaemonClient::detachEventSink(BoundaryEventSink& sink)
    {
        if (m_eventSink == &sink)
            m_eventSink = nullptr;
    }

    std::optional<SocketTransportError> SocketDaemonClient::ensureConnected()
    {
        if (isConnected())
            return std::nullopt;
        if (m_lastError.has_value())
            return m_lastError;
        return SocketTransportError{.reason = SocketDisconnectReason::TransportFailure,
                                    .detail = QStringLiteral("socket client is not connected")};
    }

    std::optional<SocketTransportError> SocketDaemonClient::enqueue(PendingFrame frame)
    {
        const auto bytes = static_cast<std::size_t>(frame.data.size());
        if (m_pendingWrites.size() + (m_currentWrite != nullptr ? 1U : 0U) >=
                m_options.maximumQueuedFrames ||
            m_queuedBytes + bytes > m_options.maximumQueuedBytes)
        {
            return SocketTransportError{.reason = SocketDisconnectReason::QueueOverflow,
                                        .detail = QStringLiteral("socket output queue is full")};
        }
        m_queuedBytes += bytes;
        m_pendingWrites.push_back(std::move(frame));
        pumpWrites();
        return std::nullopt;
    }

    void SocketDaemonClient::readSocket()
    {
        const QScopedValueRollback callbackGuard{m_inSocketCallback, true};
        if (m_socket == nullptr)
            return;
        if (const auto error = m_decoder.append(m_socket->readAll()))
        {
            clearSocket(SocketDisconnectReason::ProtocolViolation, error->detail);
            return;
        }
        while (m_socket != nullptr && !m_processingFrame)
        {
            auto decoded = m_decoder.takeFrame();
            if (auto* error = std::get_if<SocketFrameError>(&decoded))
            {
                clearSocket(SocketDisconnectReason::ProtocolViolation, error->detail);
                return;
            }
            auto& frame = std::get<std::optional<SocketFrame>>(decoded);
            if (!frame.has_value())
                return;
            m_processingFrame = true;
            handleFrame(*frame);
            m_processingFrame = false;
        }
    }

    void SocketDaemonClient::writeSocket(const qint64)
    {
        const QScopedValueRollback callbackGuard{m_inSocketCallback, true};
        pumpWrites();
    }

    void SocketDaemonClient::socketDisconnected()
    {
        const QScopedValueRollback callbackGuard{m_inSocketCallback, true};
        if (m_socket != nullptr)
            clearSocket(SocketDisconnectReason::PeerClosed, QStringLiteral("daemon disconnected"));
    }

    void SocketDaemonClient::socketError()
    {
        const QScopedValueRollback callbackGuard{m_inSocketCallback, true};
        if (m_socket != nullptr)
            clearSocket(SocketDisconnectReason::TransportFailure, m_socket->errorString());
    }

    void SocketDaemonClient::handleFrame(const SocketFrame& frame)
    {
        if (frame.kind == SocketFrameKind::BoundaryEventFrame)
        {
            if (frame.correlation != 0)
            {
                clearSocket(SocketDisconnectReason::ProtocolViolation,
                            QStringLiteral("boundary event correlation is non-zero"));
                return;
            }
            const auto event = decodeBoundaryEvent(frame.payload, m_options.limits);
            if (auto* error = std::get_if<SocketFrameError>(&event))
            {
                clearSocket(SocketDisconnectReason::ProtocolViolation, error->detail);
                return;
            }
            // Boundary handlers may issue synchronous follow-up requests. Deliver outside the
            // socket read callback so their replies can be processed normally.
            auto boundaryEvent = std::get<BoundaryEvent>(std::move(event));
            QMetaObject::invokeMethod(
                this,
                [this, boundaryEvent = std::move(boundaryEvent)]
                {
                    if (m_eventSink != nullptr)
                        m_eventSink->onBoundaryEvent(boundaryEvent);
                },
                Qt::QueuedConnection);
            return;
        }
        if (frame.kind == SocketFrameKind::ProtocolError)
        {
            const auto error = decodeProtocolError(frame.payload, m_options.limits);
            if (auto* frameError = std::get_if<SocketFrameError>(&error))
            {
                clearSocket(SocketDisconnectReason::ProtocolViolation, frameError->detail);
                return;
            }
            const auto detail =
                std::get<std::optional<BoundaryError>>(error)
                    .value_or(
                        BoundaryError{.code = BoundaryErrorCode::ProtocolViolation,
                                      .field = QStringLiteral("socket"),
                                      .detail = QStringLiteral("daemon rejected the socket frame")})
                    .detail;
            clearSocket(SocketDisconnectReason::ProtocolViolation, detail);
            return;
        }
        if (frame.correlation == 0)
        {
            clearSocket(SocketDisconnectReason::ProtocolViolation,
                        QStringLiteral("reply correlation is zero"));
            return;
        }
        if (const auto pending = m_asyncReplies.find(frame.correlation);
            pending != m_asyncReplies.end())
        {
            if (frame.kind != pending->second->expectedKind)
            {
                clearSocket(SocketDisconnectReason::ProtocolViolation,
                            QStringLiteral("socket reply kind is unexpected"));
                return;
            }
            auto reply = std::move(pending->second);
            m_asyncReplies.erase(pending);
            reply->promise.addResult(
                AsyncFrameResult{ReceivedFrame{.kind = frame.kind, .payload = frame.payload}});
            reply->promise.finish();
            return;
        }
        if (m_receivedReplies.contains(frame.correlation))
        {
            clearSocket(SocketDisconnectReason::ProtocolViolation,
                        QStringLiteral("duplicate socket reply correlation"));
            return;
        }
        if (m_receivedReplies.size() + m_asyncReplies.size() >= m_options.maximumQueuedFrames ||
            m_receivedBytes + static_cast<std::size_t>(frame.payload.size()) >
                m_options.maximumQueuedBytes)
        {
            clearSocket(SocketDisconnectReason::ProtocolViolation,
                        QStringLiteral("socket reply queue is full"));
            return;
        }
        m_receivedBytes += static_cast<std::size_t>(frame.payload.size());
        m_receivedReplies.emplace(frame.correlation,
                                  ReceivedFrame{.kind = frame.kind, .payload = frame.payload});
    }

    void SocketDaemonClient::pumpWrites()
    {
        if (m_socket == nullptr)
            return;
        if (m_currentWrite != nullptr)
        {
            auto& completed = *m_currentWrite;
            if (completed.offset != static_cast<std::size_t>(completed.data.size()) ||
                m_socket->bytesToWrite() != 0)
                return;
            m_queuedBytes -= static_cast<std::size_t>(completed.data.size());
            m_currentWrite.reset();
        }
        if (m_pendingWrites.empty())
            return;
        m_currentWrite = std::make_unique<PendingFrame>(std::move(m_pendingWrites.front()));
        m_pendingWrites.pop_front();
        auto& frame = *m_currentWrite;
        while (frame.offset < static_cast<std::size_t>(frame.data.size()))
        {
            const auto remaining = frame.data.size() - static_cast<qsizetype>(frame.offset);
            const auto written = m_socket->write(frame.data.constData() + frame.offset, remaining);
            if (written < 0)
            {
                clearSocket(SocketDisconnectReason::TransportFailure, m_socket->errorString());
                return;
            }
            if (written == 0)
                break;
            frame.offset += static_cast<std::size_t>(written);
        }
        if (frame.offset == static_cast<std::size_t>(frame.data.size()) &&
            m_socket->bytesToWrite() == 0)
        {
            m_queuedBytes -= static_cast<std::size_t>(frame.data.size());
            m_currentWrite.reset();
            pumpWrites();
        }
    }

    void SocketDaemonClient::clearSocket(const SocketDisconnectReason reason, QString detail)
    {
        if (m_inSocketCallback)
        {
            if (m_clearScheduled)
                return;
            m_clearScheduled = true;
            m_lastError = SocketTransportError{.reason = reason, .detail = detail};
            QMetaObject::invokeMethod(
                this,
                [this, reason, detail = std::move(detail)]() mutable
                {
                    m_clearScheduled = false;
                    clearSocket(reason, std::move(detail));
                },
                Qt::QueuedConnection);
            return;
        }
        const bool wasConnected = m_socket != nullptr;
        if (m_socket != nullptr)
        {
            QSignalBlocker blocker{m_socket.get()};
            m_socket->abort();
        }
        m_socket.reset();
        m_decoder.clear();
        m_pendingWrites.clear();
        m_currentWrite.reset();
        m_receivedReplies.clear();
        auto asyncReplies = std::move(m_asyncReplies);
        m_asyncReplies.clear();
        m_queuedBytes = 0;
        m_receivedBytes = 0;
        m_clearScheduled = false;
        m_lastError = SocketTransportError{.reason = reason, .detail = std::move(detail)};
        for (auto& [correlation, reply] : asyncReplies)
        {
            Q_UNUSED(correlation)
            reply->promise.addResult(AsyncFrameResult{*m_lastError});
            reply->promise.finish();
        }
        if (wasConnected)
            Q_EMIT connectionClosed(reason, m_lastError->detail);
    }

    std::variant<SocketDaemonClient::ReceivedFrame, SocketTransportError>
    SocketDaemonClient::request(const SocketFrameKind requestKind, const QByteArray& payload,
                                const SocketFrameKind replyKind)
    {
        if (const auto error = ensureConnected())
            return *error;
        const auto correlation = m_nextCorrelation;
        ++m_nextCorrelation;
        if (m_nextCorrelation == 0)
            ++m_nextCorrelation;
        const auto frameData = encodeSocketFrame(requestKind, correlation, payload,
                                                 m_options.limits.maximumFrameBytes);
        if (auto* error = std::get_if<SocketFrameError>(&frameData))
            return SocketTransportError{.reason = SocketDisconnectReason::ProtocolViolation,
                                        .detail = error->detail};
        if (const auto error = enqueue(PendingFrame{.data = std::get<QByteArray>(frameData),
                                                    .kind = requestKind,
                                                    .correlation = correlation}))
            return *error;
        if (const auto error = waitForReply(correlation, replyKind))
            return *error;
        const auto iterator = m_receivedReplies.find(correlation);
        if (iterator == m_receivedReplies.end())
            return SocketTransportError{.reason = SocketDisconnectReason::TransportFailure,
                                        .detail = QStringLiteral("socket reply was lost")};
        auto reply = std::move(iterator->second);
        m_receivedBytes -= static_cast<std::size_t>(reply.payload.size());
        m_receivedReplies.erase(iterator);
        if (reply.kind != replyKind)
            return SocketTransportError{.reason = SocketDisconnectReason::ProtocolViolation,
                                        .detail =
                                            QStringLiteral("socket reply kind is unexpected")};
        return reply;
    }

    QFuture<SocketDaemonClient::AsyncFrameResult>
    SocketDaemonClient::requestAsync(const SocketFrameKind requestKind, const QByteArray& payload,
                                     const SocketFrameKind replyKind)
    {
        auto pending = std::make_unique<PendingAsyncReply>();
        pending->expectedKind = replyKind;
        pending->promise.start();
        auto future = pending->promise.future();
        const auto failImmediately = [&pending](SocketTransportError error)
        {
            pending->promise.addResult(AsyncFrameResult{std::move(error)});
            pending->promise.finish();
        };

        if (const auto error = ensureConnected())
        {
            failImmediately(*error);
            return future;
        }
        if (m_asyncReplies.size() + m_receivedReplies.size() >= m_options.maximumQueuedFrames)
        {
            failImmediately({.reason = SocketDisconnectReason::QueueOverflow,
                             .detail = QStringLiteral("too many socket replies are pending")});
            return future;
        }
        const auto correlation = m_nextCorrelation;
        ++m_nextCorrelation;
        if (m_nextCorrelation == 0)
            ++m_nextCorrelation;
        const auto frameData = encodeSocketFrame(requestKind, correlation, payload,
                                                 m_options.limits.maximumFrameBytes);
        if (auto* error = std::get_if<SocketFrameError>(&frameData))
        {
            failImmediately(
                {.reason = SocketDisconnectReason::ProtocolViolation, .detail = error->detail});
            return future;
        }
        m_asyncReplies.emplace(correlation, std::move(pending));
        if (const auto error = enqueue(PendingFrame{.data = std::get<QByteArray>(frameData),
                                                    .kind = requestKind,
                                                    .correlation = correlation}))
        {
            if (const auto found = m_asyncReplies.find(correlation); found != m_asyncReplies.end())
            {
                auto failed = std::move(found->second);
                m_asyncReplies.erase(found);
                failed->promise.addResult(AsyncFrameResult{*error});
                failed->promise.finish();
            }
            return future;
        }
        QTimer::singleShot(m_options.responseTimeoutMilliseconds, this,
                           [this, correlation] { timeoutAsyncReply(correlation); });
        return future;
    }

    void SocketDaemonClient::timeoutAsyncReply(const std::uint64_t correlation)
    {
        const auto found = m_asyncReplies.find(correlation);
        if (found == m_asyncReplies.end())
            return;
        auto reply = std::move(found->second);
        m_asyncReplies.erase(found);
        const SocketTransportError error{
            .reason = SocketDisconnectReason::TransportFailure,
            .detail = QStringLiteral("timed out waiting for a socket reply"),
        };
        reply->promise.addResult(AsyncFrameResult{error});
        reply->promise.finish();
        if (isConnected())
            clearSocket(error.reason, error.detail);
    }

    std::optional<SocketTransportError>
    SocketDaemonClient::waitForReply(const std::uint64_t correlation,
                                     const SocketFrameKind replyKind)
    {
        QElapsedTimer timer;
        timer.start();
        while (true)
        {
            if (m_lastError.has_value())
                return m_lastError;
            if (const auto iterator = m_receivedReplies.find(correlation);
                iterator != m_receivedReplies.end())
            {
                if (iterator->second.kind != replyKind)
                    return SocketTransportError{
                        .reason = SocketDisconnectReason::ProtocolViolation,
                        .detail = QStringLiteral("socket reply kind is unexpected")};
                return std::nullopt;
            }
            if (!isConnected())
                return m_lastError.value_or(SocketTransportError{
                    .reason = SocketDisconnectReason::PeerClosed,
                    .detail = QStringLiteral("daemon disconnected before the reply")});
            const auto elapsed = static_cast<int>(timer.elapsed());
            const auto remaining = m_options.responseTimeoutMilliseconds - elapsed;
            if (remaining <= 0 || !m_socket->waitForReadyRead(remaining))
            {
                if (isConnected())
                    clearSocket(SocketDisconnectReason::TransportFailure,
                                QStringLiteral("timed out waiting for a socket reply"));
                return m_lastError.value_or(SocketTransportError{
                    .reason = SocketDisconnectReason::TransportFailure,
                    .detail = QStringLiteral("timed out waiting for a socket reply")});
            }
            readSocket();
        }
    }

    BoundaryError SocketDaemonClient::boundaryError(const SocketTransportError& error) const
    {
        return makeBoundaryError(error);
    }

    CommandReply SocketDaemonClient::submitCommand(CommandRequest request)
    {
        const auto id = request.id;
        const auto encoded = encodeClientRequest(ClientRequest{request}, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&encoded))
            return CommandRejected{
                .id = id,
                .error = boundaryError(SocketTransportError{
                    .reason = SocketDisconnectReason::ProtocolViolation, .detail = error->detail})};
        const auto result = this->request(std::get<EncodedPayload>(encoded).kind,
                                          std::get<EncodedPayload>(encoded).payload,
                                          SocketFrameKind::CommandReplyFrame);
        if (auto* error = std::get_if<SocketTransportError>(&result))
            return CommandRejected{.id = id, .error = boundaryError(*error)};
        const auto reply =
            decodeCommandReply(std::get<ReceivedFrame>(result).payload, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&reply))
            return CommandRejected{
                .id = id,
                .error = boundaryError(SocketTransportError{
                    .reason = SocketDisconnectReason::ProtocolViolation, .detail = error->detail})};
        return std::get<CommandReply>(reply);
    }

    QFuture<CommandReply> SocketDaemonClient::submitCommandAsync(CommandRequest request)
    {
        const auto id = request.id;
        auto promise = std::make_shared<QPromise<CommandReply>>();
        promise->start();
        auto future = promise->future();
        const auto complete = [promise](CommandReply reply)
        {
            promise->addResult(std::move(reply));
            promise->finish();
        };

        const auto encoded = encodeClientRequest(ClientRequest{request}, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&encoded))
        {
            complete(CommandRejected{.id = id,
                                     .error = boundaryError(SocketTransportError{
                                         .reason = SocketDisconnectReason::ProtocolViolation,
                                         .detail = error->detail})});
            return future;
        }

        auto* watcher = new QFutureWatcher<AsyncFrameResult>(this);
        connect(watcher, &QFutureWatcherBase::finished, this,
                [this, watcher, id, complete]
                {
                    const auto result = watcher->result();
                    watcher->deleteLater();
                    if (const auto* error = std::get_if<SocketTransportError>(&result))
                    {
                        complete(CommandRejected{.id = id, .error = boundaryError(*error)});
                        return;
                    }
                    const auto reply = decodeCommandReply(std::get<ReceivedFrame>(result).payload,
                                                          m_options.limits);
                    if (const auto* error = std::get_if<SocketFrameError>(&reply))
                    {
                        complete(
                            CommandRejected{.id = id,
                                            .error = boundaryError(SocketTransportError{
                                                .reason = SocketDisconnectReason::ProtocolViolation,
                                                .detail = error->detail})});
                        return;
                    }
                    complete(std::get<CommandReply>(reply));
                });
        watcher->setFuture(requestAsync(std::get<EncodedPayload>(encoded).kind,
                                        std::get<EncodedPayload>(encoded).payload,
                                        SocketFrameKind::CommandReplyFrame));
        return future;
    }

    MaterializationReply SocketDaemonClient::requestMaterialization(MaterializationRequest request)
    {
        const auto id = request.id;
        const auto encoded = encodeClientRequest(ClientRequest{request}, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&encoded))
            return MaterializationRejected{
                .id = id,
                .error = boundaryError(SocketTransportError{
                    .reason = SocketDisconnectReason::ProtocolViolation, .detail = error->detail})};
        const auto result = this->request(std::get<EncodedPayload>(encoded).kind,
                                          std::get<EncodedPayload>(encoded).payload,
                                          SocketFrameKind::MaterializationReplyFrame);
        if (auto* error = std::get_if<SocketTransportError>(&result))
            return MaterializationRejected{.id = id, .error = boundaryError(*error)};
        const auto reply =
            decodeMaterializationReply(std::get<ReceivedFrame>(result).payload, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&reply))
            return MaterializationRejected{
                .id = id,
                .error = boundaryError(SocketTransportError{
                    .reason = SocketDisconnectReason::ProtocolViolation, .detail = error->detail})};
        return std::get<MaterializationReply>(reply);
    }

    void SocketDaemonClient::cancelMaterializationScope(const ScopeId scope)
    {
        const auto encoded = encodeClientRequest(
            ClientRequest{CancelMaterializationScopeRequest{.scope = scope}}, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&encoded))
        {
            m_lastError = SocketTransportError{.reason = SocketDisconnectReason::ProtocolViolation,
                                               .detail = error->detail};
            return;
        }
        const auto result = request(std::get<EncodedPayload>(encoded).kind,
                                    std::get<EncodedPayload>(encoded).payload,
                                    SocketFrameKind::CancelMaterializationScopeReply);
        if (std::holds_alternative<SocketTransportError>(result))
            return;
        const auto reply =
            decodeOptionalError(std::get<ReceivedFrame>(result).payload, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&reply))
            m_lastError = SocketTransportError{.reason = SocketDisconnectReason::ProtocolViolation,
                                               .detail = error->detail};
    }

    SettingsReadReply SocketDaemonClient::getSettings()
    {
        const auto encoded =
            encodeClientRequest(ClientRequest{GetSettingsRequest{}}, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&encoded))
            return SettingsReadRejected{
                .error = boundaryError(SocketTransportError{
                    .reason = SocketDisconnectReason::ProtocolViolation, .detail = error->detail})};
        const auto result = request(std::get<EncodedPayload>(encoded).kind,
                                    std::get<EncodedPayload>(encoded).payload,
                                    SocketFrameKind::SettingsReadReplyFrame);
        if (auto* error = std::get_if<SocketTransportError>(&result))
            return SettingsReadRejected{.error = boundaryError(*error)};
        const auto reply =
            decodeSettingsReadReply(std::get<ReceivedFrame>(result).payload, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&reply))
            return SettingsReadRejected{
                .error = boundaryError(SocketTransportError{
                    .reason = SocketDisconnectReason::ProtocolViolation, .detail = error->detail})};
        return std::get<SettingsReadReply>(reply);
    }

    SettingsUpdateReply SocketDaemonClient::updateSettings(UpdateSettingsRequest request)
    {
        const auto baseRevision = request.baseRevision;
        const auto encoded = encodeClientRequest(ClientRequest{request}, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&encoded))
            return SettingsUpdateRejected{
                .currentRevision = baseRevision,
                .error = boundaryError(SocketTransportError{
                    .reason = SocketDisconnectReason::ProtocolViolation, .detail = error->detail})};
        const auto result = this->request(std::get<EncodedPayload>(encoded).kind,
                                          std::get<EncodedPayload>(encoded).payload,
                                          SocketFrameKind::SettingsUpdateReplyFrame);
        if (auto* error = std::get_if<SocketTransportError>(&result))
            return SettingsUpdateRejected{.currentRevision = baseRevision,
                                          .error = boundaryError(*error)};
        const auto reply =
            decodeSettingsUpdateReply(std::get<ReceivedFrame>(result).payload, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&reply))
            return SettingsUpdateRejected{
                .currentRevision = baseRevision,
                .error = boundaryError(SocketTransportError{
                    .reason = SocketDisconnectReason::ProtocolViolation, .detail = error->detail})};
        return std::get<SettingsUpdateReply>(reply);
    }

    HandshakeReply SocketDaemonClient::hello(HelloRequest request)
    {
        const auto encoded = encodeClientRequest(ClientRequest{request}, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&encoded))
            return HandshakeRejected{
                .error = boundaryError(SocketTransportError{
                    .reason = SocketDisconnectReason::ProtocolViolation, .detail = error->detail})};
        const auto result =
            this->request(std::get<EncodedPayload>(encoded).kind,
                          std::get<EncodedPayload>(encoded).payload, SocketFrameKind::HelloReply);
        if (auto* error = std::get_if<SocketTransportError>(&result))
            return HandshakeRejected{.error = boundaryError(*error)};
        const auto reply =
            decodeHandshakeReply(std::get<ReceivedFrame>(result).payload, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&reply))
            return HandshakeRejected{
                .error = boundaryError(SocketTransportError{
                    .reason = SocketDisconnectReason::ProtocolViolation, .detail = error->detail})};
        const auto& handshake = std::get<HandshakeReply>(reply);
        if (const auto* ready = std::get_if<ReadyReply>(&handshake);
            ready != nullptr && ready->protocol.major != request.protocol.major)
            return HandshakeRejected{
                .error = {.code = BoundaryErrorCode::IncompatibleBuild,
                          .field = QStringLiteral("hello.protocol"),
                          .detail = QStringLiteral("daemon returned an incompatible protocol")}};
        return handshake;
    }

    std::optional<BoundaryError> SocketDaemonClient::ping()
    {
        const auto encoded = encodeClientRequest(ClientRequest{PingRequest{}}, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&encoded))
            return boundaryError(SocketTransportError{
                .reason = SocketDisconnectReason::ProtocolViolation, .detail = error->detail});
        const auto result =
            request(std::get<EncodedPayload>(encoded).kind,
                    std::get<EncodedPayload>(encoded).payload, SocketFrameKind::PingReply);
        if (auto* error = std::get_if<SocketTransportError>(&result))
            return boundaryError(*error);
        const auto reply =
            decodeOptionalError(std::get<ReceivedFrame>(result).payload, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&reply))
            return boundaryError(SocketTransportError{
                .reason = SocketDisconnectReason::ProtocolViolation, .detail = error->detail});
        return std::get<std::optional<BoundaryError>>(reply);
    }

    std::optional<BoundaryError> SocketDaemonClient::readyForActivation()
    {
        const auto result = request(SocketFrameKind::ReadyForActivationRequest, {},
                                    SocketFrameKind::ReadyForActivationReply);
        if (auto* error = std::get_if<SocketTransportError>(&result))
            return boundaryError(*error);
        const auto reply =
            decodeOptionalError(std::get<ReceivedFrame>(result).payload, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&reply))
            return boundaryError(SocketTransportError{
                .reason = SocketDisconnectReason::ProtocolViolation, .detail = error->detail});
        return std::get<std::optional<BoundaryError>>(reply);
    }

    std::optional<BoundaryError> SocketDaemonClient::acknowledgeCacheAccessSuspended(
        CacheAccessSuspendedAcknowledgement acknowledgement)
    {
        const auto encoded = encodeClientRequest(ClientRequest{acknowledgement}, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&encoded))
            return boundaryError(SocketTransportError{
                .reason = SocketDisconnectReason::ProtocolViolation, .detail = error->detail});
        const auto result = request(std::get<EncodedPayload>(encoded).kind,
                                    std::get<EncodedPayload>(encoded).payload,
                                    SocketFrameKind::CacheAccessSuspendedReply);
        if (auto* error = std::get_if<SocketTransportError>(&result))
            return boundaryError(*error);
        const auto reply =
            decodeOptionalError(std::get<ReceivedFrame>(result).payload, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&reply))
            return boundaryError(SocketTransportError{
                .reason = SocketDisconnectReason::ProtocolViolation, .detail = error->detail});
        return std::get<std::optional<BoundaryError>>(reply);
    }

} // namespace javelin::protocol
