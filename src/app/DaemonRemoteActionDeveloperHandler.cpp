#include "app/DaemonRemoteActionDispatcher.h"

#include "app/DaemonServices.h"
#include "app/DeveloperDiagnostics.h"
#include "app/DeveloperMaintenance.h"
#include "app/LogStore.h"

#include <algorithm>

namespace javelin::app
{
    namespace
    {
        constexpr qsizetype developerLogBatchSize = 50;

        [[nodiscard]] QString boundedDeveloperLogText(const QString& value,
                                                      const qsizetype maximumBytes)
        {
            const QByteArray utf8 = value.toUtf8();
            if (utf8.size() <= maximumBytes)
                return value;
            return QString::fromUtf8(utf8.first(maximumBytes - 8)) + QStringLiteral("…");
        }

        [[nodiscard]] javelin::protocol::DiagnosticLogEntry
        developerDiagnosticLogEntry(const LogEntry& entry)
        {
            return {
                .timestampMilliseconds =
                    static_cast<std::uint64_t>(entry.timestamp.toMSecsSinceEpoch()),
                .level = static_cast<std::uint8_t>(entry.level),
                .subsystem = boundedDeveloperLogText(entry.subsystem, 256),
                .message = boundedDeveloperLogText(entry.message, 4000),
            };
        }
    } // namespace

    javelin::protocol::CommandReply DaemonRemoteActionDispatcher::dispatchDeveloperAction(
        const javelin::protocol::CommandId& id,
        const javelin::protocol::RemoteActionCommand& command)
    {
        namespace actions = javelin::protocol::actions;
        switch (command.action.value)
        {
        case actions::DeveloperDiagnosticsSnapshot::id.value:
            return launchAction<actions::DeveloperDiagnosticsSnapshot>(
                id, m_services.developerDiagnosticsPort().snapshot());
        case actions::DeveloperMailboxClear::id.value:
            return dispatchDecoded<actions::DeveloperMailboxClear>(
                id, command,
                [this, &id](DeveloperMailboxClearCommand clearCommand)
                {
                    return launchAction<actions::DeveloperMailboxClear>(
                        id, m_services.developerMaintenancePort().clearMailboxCache(
                                std::move(clearCommand)));
                });
        case actions::DeveloperLogSetSubscribed::id.value:
            return dispatchDecoded<actions::DeveloperLogSetSubscribed>(
                id, command,
                [this, &id](const bool subscribed)
                {
                    m_daemonLogSubscribed = subscribed;
                    if (subscribed)
                    {
                        const auto entries = LogStore::instance().entries();
                        for (qsizetype offset = 0; offset < entries.size();
                             offset += developerLogBatchSize)
                        {
                            javelin::protocol::DaemonLogEntries event;
                            const qsizetype count =
                                std::min(developerLogBatchSize, entries.size() - offset);
                            event.entries.reserve(static_cast<std::size_t>(count));
                            for (qsizetype index = 0; index < count; ++index)
                                event.entries.push_back(
                                    developerDiagnosticLogEntry(entries.at(offset + index)));
                            m_eventSink.onBoundaryEvent(event);
                        }
                    }
                    return acceptEmpty<actions::DeveloperLogSetSubscribed>(id);
                });
        case actions::DeveloperLogClear::id.value:
            LogStore::instance().clear();
            return acceptEmpty<actions::DeveloperLogClear>(id);
        default:
            return reject(id, QStringLiteral("The developer action is unsupported."),
                          javelin::protocol::BoundaryErrorCode::UnsupportedOperation);
        }
    }
} // namespace javelin::app
