#include "app/ProcessInstanceLock.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>

namespace javelin::app
{
    namespace
    {
        enum class ProcessOwnerState
        {
            Missing,
            CurrentProcess,
            ExpectedProcess,
            DifferentProcess,
            Unknown,
        };

        [[nodiscard]] ProcessOwnerState processOwnerState(const qint64 pid,
                                                          const QString& expectedExecutableName)
        {
            if (pid <= 0)
                return ProcessOwnerState::Missing;
            if (pid == QCoreApplication::applicationPid())
                return ProcessOwnerState::CurrentProcess;

#ifdef Q_OS_LINUX
            const auto processDirectory = QStringLiteral("/proc/%1").arg(pid);
            if (!QFileInfo::exists(processDirectory))
                return ProcessOwnerState::Missing;

            QFile commandNameFile{processDirectory + QStringLiteral("/comm")};
            if (!commandNameFile.open(QIODevice::ReadOnly))
                return QFileInfo::exists(processDirectory) ? ProcessOwnerState::Unknown
                                                           : ProcessOwnerState::Missing;

            const auto commandName = QString::fromUtf8(commandNameFile.readAll()).trimmed();
            return commandName == QFileInfo{expectedExecutableName}.fileName()
                       ? ProcessOwnerState::ExpectedProcess
                       : ProcessOwnerState::DifferentProcess;
#else
            Q_UNUSED(expectedExecutableName);
            return ProcessOwnerState::Unknown;
#endif
        }
    } // namespace

    ProcessLockRecoveryResult recoverAbandonedProcessLock(QLockFile& lockFile,
                                                          const QString& expectedExecutableName)
    {
        qint64 pid = 0;
        QString hostname;
        QString recordedApplicationName;
        if (!lockFile.getLockInfo(&pid, &hostname, &recordedApplicationName))
            return ProcessLockRecoveryResult::LockInfoUnavailable;

        const auto expectedName = QFileInfo{expectedExecutableName}.fileName();
        const auto ownerState = processOwnerState(pid, expectedName);
        const bool recordedNameMatches =
            recordedApplicationName.isEmpty() ||
            QFileInfo{recordedApplicationName}.fileName() == expectedName;
        if (ownerState == ProcessOwnerState::ExpectedProcess && recordedNameMatches)
            return ProcessLockRecoveryResult::OwnerMayBeAlive;
        if (ownerState == ProcessOwnerState::Unknown && recordedNameMatches)
            return ProcessLockRecoveryResult::OwnerMayBeAlive;

        return QFile::remove(lockFile.fileName()) ? ProcessLockRecoveryResult::Removed
                                                  : ProcessLockRecoveryResult::RemovalFailed;
    }

} // namespace javelin::app
