#pragma once

#include "protocol/ActionContract.h"
#include "protocol/ActivationContract.h"
#include "protocol/CacheContract.h"
#include "protocol/HandshakeContract.h"
#include "protocol/SettingsContract.h"

#include <QString>

#include <cstdint>
#include <variant>
#include <vector>

namespace javelin::protocol
{
    struct DaemonShutdownRequested
    {
    };

    struct DaemonStatusChanged
    {
        DaemonStatus status;
    };

    struct DiagnosticLogEntry
    {
        std::uint64_t timestampMilliseconds = 0;
        std::uint8_t level = 0;
        QString subsystem;
        QString message;
    };

    struct DaemonLogEntries
    {
        std::vector<DiagnosticLogEntry> entries;
    };

    using BoundaryEvent =
        std::variant<CacheInvalidation, OperationFailed, OperationCompleted, SettingsUpdated,
                     ActivationRequested, DaemonStatusChanged, CacheAccessSuspendRequested,
                     CacheAccessResumed, DaemonShutdownRequested, DaemonLogEntries,
                     ThreadMaterializationProgress>;

    class BoundaryEventSink
    {
      public:
        virtual ~BoundaryEventSink() = default;
        virtual void onBoundaryEvent(const BoundaryEvent& event) = 0;
    };
} // namespace javelin::protocol
