#pragma once

#include "storage/sqlite/DatabaseConnection.h"

#include "app/ThreadMaterializationCoordinator.h"

#include <QObject>
#include <QStringList>

namespace javelin::app
{
    class AccountConnectionProvider;
}
namespace javelin::jmap::api
{
    class JmapMethodTransport;
}
namespace javelin::jmap::cache
{
    class DatabaseConnection;
}

namespace javelin::app
{
    class ThreadMembershipMaterializationWorker final : public QObject,
                                                        public ThreadMaterializationWorker
    {
        Q_OBJECT

      public:
        ThreadMembershipMaterializationWorker(
            javelin::jmap::cache::DatabaseConnection& databaseConnection,
            javelin::jmap::api::JmapMethodTransport& methodTransport,
            const AccountConnectionProvider& connectionProvider, QObject* parent = nullptr);

        [[nodiscard]] QCoro::Task<ThreadMaterializationResult>
        materialize(ThreadMaterializationTarget target) override;

      Q_SIGNALS:
        void membershipCommitted(QString accountId, QStringList threadIds);
        void childEmailsCommitted(QString accountId, QStringList threadIds, QStringList emailIds);
        void progressChanged(QString accountId, quint64 completedThreadCount,
                             quint64 totalThreadCount);
        void childProgressChanged(QString accountId, quint64 completedEmailCount);

      private:
        javelin::jmap::cache::DatabaseConnection& m_databaseConnection;
        javelin::jmap::api::JmapMethodTransport& m_methodTransport;
        const AccountConnectionProvider& m_connectionProvider;
    };
} // namespace javelin::app
