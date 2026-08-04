#pragma once

#include <QString>

class QLockFile;

namespace javelin::app
{
    enum class ProcessLockRecoveryResult
    {
        OwnerMayBeAlive,
        Removed,
        LockInfoUnavailable,
        RemovalFailed,
    };

    // Call this only after the process's authoritative endpoint (for example its local socket)
    // has been checked. PIDs recorded inside sandboxed lock files may be namespace-local and reused
    // by a later process, so endpoint liveness must take precedence over PID inspection.
    [[nodiscard]] ProcessLockRecoveryResult
    recoverAbandonedProcessLock(QLockFile& lockFile, const QString& expectedExecutableName);

} // namespace javelin::app
