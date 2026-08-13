#pragma once

#include "jmap/OperationError.h"
#include "jmap/api/LiveConnectionSettings.h"

#include <QCoroTask>

#include <functional>
#include <memory>
#include <string>
#include <variant>

namespace javelin::jmap::cache
{
    class DatabaseConnection;
}

namespace javelin::jmap::api
{
    class JmapMethodTransport;
}

namespace javelin::jmap
{
    struct MailCapabilityContext;

    struct MailboxSubscriptionChange
    {
        std::string accountId;
        std::string mailboxId;
        bool subscribed = true;
    };

    using MailboxSubscriptionChangeResult = std::variant<MailboxSubscriptionChange, OperationError>;

    struct MailboxCreateChange
    {
        std::string accountId;
        std::string mailboxId;
        std::string name;
    };

    using MailboxCreateResult = std::variant<MailboxCreateChange, OperationError>;

    struct MailboxDestroyChange
    {
        std::string accountId;
        std::string mailboxId;
    };

    using MailboxDestroyResult = std::variant<MailboxDestroyChange, OperationError>;

    class MailboxMutationEngine
    {
      public:
        MailboxMutationEngine(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                              javelin::jmap::api::JmapMethodTransport& methodTransport);
        ~MailboxMutationEngine();

        [[nodiscard]] QCoro::Task<MailboxSubscriptionChangeResult>
        setSubscribed(LiveConnectionSettings settings, std::string accountId, std::string mailboxId,
                      bool subscribed, std::function<void()> projectionCommitted = {});
        [[nodiscard]] QCoro::Task<MailboxSubscriptionChangeResult>
        reconcileSubscription(LiveConnectionSettings settings, std::string accountId);
        [[nodiscard]] QCoro::Task<MailboxCreateResult>
        create(LiveConnectionSettings settings, std::string accountId, std::string name,
               std::function<void()> projectionCommitted = {});
        [[nodiscard]] QCoro::Task<MailboxCreateResult>
        reconcileCreate(LiveConnectionSettings settings, std::string accountId);
        [[nodiscard]] QCoro::Task<MailboxDestroyResult>
        destroy(LiveConnectionSettings settings, std::string accountId, std::string mailboxId,
                std::function<void()> projectionCommitted = {});
        [[nodiscard]] QCoro::Task<MailboxDestroyResult>
        reconcileDestroy(LiveConnectionSettings settings, std::string accountId);

      private:
        std::unique_ptr<MailCapabilityContext> m_impl;
    };

} // namespace javelin::jmap
