#pragma once

#include "app/MailApplicationService.h"
#include "app/MessageListSession.h"
#include "jmap/cache/QueryReader.h"
#include "jmap/query/EmailListSort.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace javelin::app
{
    struct RestoredMailboxState
    {
        MessageListPage page;
    };

    class MailboxSession final : public MessageListSession
    {
      public:
        MailboxSession(std::string accountId, std::string mailboxId, QString title,
                       std::optional<std::string> role, javelin::jmap::query::EmailListSort sort,
                       javelin::jmap::cache::QueryReader& queryReader,
                       MailApplicationService& mailService, std::size_t pageSize,
                       std::optional<RestoredMailboxState> restored = std::nullopt,
                       QObject* parent = nullptr);

        [[nodiscard]] const std::string& accountId() const override;
        [[nodiscard]] const std::string& mailboxId() const;
        [[nodiscard]] QString title() const override;
        [[nodiscard]] const std::optional<std::string>& role() const;
        [[nodiscard]] const MessageListPage& page() const override;
        void updateMetadata(QString title, std::optional<std::string> role);

        void loadCachedPage(bool forceReload = false) override;
        void refresh() override;
        void markStale() override;
        void setSort(javelin::jmap::query::EmailListSort sort) override;
        void reveal(std::string emailId);
        [[nodiscard]] bool goToPage(std::size_t pageIndex) override;
        [[nodiscard]] bool goToPreviousPage() override;
        [[nodiscard]] bool goToNextPage() override;

      private:
        void reloadProjectedPage();
        void applyCachedPage(javelin::jmap::cache::MailboxWindowPage page);
        void resetForPageChange();
        [[nodiscard]] std::string queryKey() const;

        std::string m_accountId;
        std::string m_mailboxId;
        QString m_title;
        std::optional<std::string> m_role;
        javelin::jmap::query::EmailListSort m_sort;
        javelin::jmap::cache::QueryReader& m_queryReader;
        MailApplicationService& m_mailService;
        std::size_t m_pageSize;
        MessageListPage m_page;
        MailboxObservation m_observation;
        std::int64_t m_anchorOffset = 1;
        std::uint64_t m_generation = 0;
        bool m_projectedReloadInFlight = false;
        bool m_projectedReloadPending = false;
    };
} // namespace javelin::app
