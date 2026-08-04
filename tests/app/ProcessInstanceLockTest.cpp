#include "app/ProcessInstanceLock.h"

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace
{
    void writeSyntheticLock(const QString& path, const qint64 pid,
                            const QByteArray& applicationName)
    {
        QByteArray contents;
        {
            QLockFile templateLock{path};
            REQUIRE(templateLock.tryLock(0));
            QFile input{path};
            REQUIRE(input.open(QIODevice::ReadOnly));
            contents = input.readAll();
            templateLock.unlock();
        }

        auto lines = contents.split('\n');
        REQUIRE(lines.size() >= 3);
        lines[0] = QByteArray::number(pid);
        lines[1] = applicationName;

        QFile output{path};
        REQUIRE(output.open(QIODevice::WriteOnly | QIODevice::Truncate));
        REQUIRE(output.write(lines.join('\n')) >= 0);
    }
} // namespace

TEST_CASE("process lock recovers a sandbox PID reused by the current process")
{
    QTemporaryDir temporaryDirectory;
    REQUIRE(temporaryDirectory.isValid());
    const auto path = temporaryDirectory.filePath(QStringLiteral("javelin.lock"));
    const auto executableName = QFileInfo{QCoreApplication::applicationFilePath()}.fileName();
    writeSyntheticLock(path, QCoreApplication::applicationPid(), executableName.toUtf8());

    QLockFile contender{path};
    contender.setStaleLockTime(0);
    CHECK(javelin::app::recoverAbandonedProcessLock(contender, executableName) ==
          javelin::app::ProcessLockRecoveryResult::Removed);
    CHECK(contender.tryLock(0));
}

TEST_CASE("process lock removes a lock whose recorded process no longer exists")
{
    QTemporaryDir temporaryDirectory;
    REQUIRE(temporaryDirectory.isValid());
    const auto path = temporaryDirectory.filePath(QStringLiteral("javelind.lock"));
    writeSyntheticLock(path, 1'000'000'000, QByteArrayLiteral("javelind"));

    QLockFile contender{path};
    contender.setStaleLockTime(0);
    CHECK(javelin::app::recoverAbandonedProcessLock(contender, QStringLiteral("javelind")) ==
          javelin::app::ProcessLockRecoveryResult::Removed);
    CHECK(contender.tryLock(0));
}

TEST_CASE("process lock preserves a lock owned by a matching live process")
{
    const auto sleepExecutable = QStandardPaths::findExecutable(QStringLiteral("sleep"));
    REQUIRE_FALSE(sleepExecutable.isEmpty());

    QProcess process;
    process.start(sleepExecutable, {QStringLiteral("30")});
    REQUIRE(process.waitForStarted());

    QTemporaryDir temporaryDirectory;
    REQUIRE(temporaryDirectory.isValid());
    const auto path = temporaryDirectory.filePath(QStringLiteral("sleep.lock"));
    writeSyntheticLock(path, process.processId(), QByteArrayLiteral("sleep"));

    QLockFile contender{path};
    contender.setStaleLockTime(0);
    REQUIRE_FALSE(contender.tryLock(0));
    CHECK(javelin::app::recoverAbandonedProcessLock(contender, QStringLiteral("sleep")) ==
          javelin::app::ProcessLockRecoveryResult::OwnerMayBeAlive);
    CHECK(QFile::exists(path));

    process.kill();
    REQUIRE(process.waitForFinished());
}

TEST_CASE("process lock removes a PID reused by another executable")
{
    const auto sleepExecutable = QStandardPaths::findExecutable(QStringLiteral("sleep"));
    REQUIRE_FALSE(sleepExecutable.isEmpty());

    QProcess process;
    process.start(sleepExecutable, {QStringLiteral("30")});
    REQUIRE(process.waitForStarted());

    QTemporaryDir temporaryDirectory;
    REQUIRE(temporaryDirectory.isValid());
    const auto path = temporaryDirectory.filePath(QStringLiteral("javelind.lock"));
    writeSyntheticLock(path, process.processId(), QByteArrayLiteral("javelind"));

    QLockFile contender{path};
    contender.setStaleLockTime(0);
    CHECK(javelin::app::recoverAbandonedProcessLock(contender, QStringLiteral("javelind")) ==
          javelin::app::ProcessLockRecoveryResult::Removed);
    CHECK_FALSE(QFile::exists(path));

    process.kill();
    REQUIRE(process.waitForFinished());
}
