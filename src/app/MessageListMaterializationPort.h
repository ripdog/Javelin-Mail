#pragma once

#include "jmap/OperationError.h"
#include "jmap/query/EmailListSort.h"
#include "jmap/search/EmailSearch.h"

#include <QCoroTask>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace javelin::app
{
    struct MailboxWindowIntent
    {
        std::string accountId;
        std::string mailboxId;
        std::size_t offset = 0;
        std::size_t limit = 0;
        javelin::jmap::query::EmailListSort sort;
        bool forceRefresh = false;
        std::optional<std::string> anchor;
        std::int64_t anchorOffset = 1;
    };

    struct SearchWindowIntent
    {
        std::string accountId;
        javelin::jmap::search::EmailSearchCriteria criteria;
        std::size_t offset = 0;
        std::size_t limit = 0;
        javelin::jmap::query::EmailListSort sort;
        std::optional<std::string> anchor;
        std::string windowKey;
    };

    struct ThreadMaterializationIntent
    {
        std::string accountId;
        std::string threadId;
    };

    struct MailboxWindowSummary
    {
        std::string accountId;
        std::string mailboxId;
        std::size_t offset = 0;
        std::size_t limit = 0;
        std::size_t position = 0;
        std::size_t returnedLimit = 0;
        std::size_t representativeCount = 0;
        std::optional<std::size_t> total;
        std::string queryState;
    };

    using MailboxWindowResult = std::variant<MailboxWindowSummary, javelin::jmap::OperationError>;

    struct SearchWindowSummary
    {
        std::string accountId;
        std::string queryKey;
        std::size_t offset = 0;
        std::size_t limit = 0;
        std::size_t position = 0;
        std::size_t returnedLimit = 0;
        std::size_t representativeCount = 0;
        std::optional<std::size_t> total;
        std::string queryState;
    };

    using SearchWindowResult = std::variant<SearchWindowSummary, javelin::jmap::OperationError>;

    class MailboxObservationLease final
    {
      public:
        MailboxObservationLease() = default;
        explicit MailboxObservationLease(std::function<void()> release)
            : m_release(std::move(release))
        {
        }
        ~MailboxObservationLease()
        {
            reset();
        }

        MailboxObservationLease(const MailboxObservationLease&) = delete;
        MailboxObservationLease& operator=(const MailboxObservationLease&) = delete;
        MailboxObservationLease(MailboxObservationLease&& other) noexcept
            : m_release(std::exchange(other.m_release, {}))
        {
        }
        MailboxObservationLease& operator=(MailboxObservationLease&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                m_release = std::exchange(other.m_release, {});
            }
            return *this;
        }

        void reset()
        {
            if (!m_release)
                return;
            auto release = std::exchange(m_release, {});
            release();
        }

        [[nodiscard]] explicit operator bool() const
        {
            return static_cast<bool>(m_release);
        }

      private:
        std::function<void()> m_release;
    };

    class MessageListMaterializationPort
    {
      public:
        virtual ~MessageListMaterializationPort() = default;

        [[nodiscard]] virtual MailboxObservationLease
        beginMailboxObservation(std::string accountId, std::string mailboxId) = 0;
        [[nodiscard]] virtual QCoro::Task<MailboxWindowResult>
        requestMailboxWindow(MailboxWindowIntent intent) = 0;
        [[nodiscard]] virtual QCoro::Task<SearchWindowResult>
        requestSearchWindow(SearchWindowIntent intent) = 0;
        virtual void ensureThread(ThreadMaterializationIntent intent) = 0;
        virtual void retireSearchWindow(std::string accountId, std::string windowKey) = 0;
    };
} // namespace javelin::app
