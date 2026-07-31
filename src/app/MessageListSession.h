#pragma once

#include "jmap/OperationError.h"
#include "jmap/cache/QueryService.h"
#include "jmap/query/EmailListSort.h"

#include <QObject>
#include <QString>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace javelin::app
{
    [[nodiscard]] inline std::size_t messageListPageCount(const std::size_t total,
                                                          const std::size_t effectiveLimit)
    {
        if (total == 0)
            return 0;
        const auto step = std::max<std::size_t>(effectiveLimit, 1);
        return 1 + ((total - 1) / step);
    }

    [[nodiscard]] inline std::size_t messageListPageOffset(const std::size_t pageIndex,
                                                           const std::size_t effectiveLimit)
    {
        return pageIndex * std::max<std::size_t>(effectiveLimit, 1);
    }

    [[nodiscard]] inline std::size_t
    normalizedMessageListPageOffset(const std::size_t currentOffset, const std::size_t total,
                                    const std::size_t effectiveLimit)
    {
        if (total == 0)
            return 0;
        if (currentOffset < total)
            return currentOffset;
        const auto step = std::max<std::size_t>(effectiveLimit, 1);
        return ((total - 1) / step) * step;
    }

    struct MessageListPage
    {
        std::size_t offset = 0;
        std::optional<std::size_t> installedOffset;
        std::optional<std::size_t> pendingOffset;
        std::size_t position = 0;
        std::size_t returnedLimit = 0;
        std::optional<std::size_t> total;
        std::string queryState;
        std::optional<std::string> anchor;
        std::vector<javelin::jmap::cache::MessageListItem> items;
        bool cacheLoaded = false;
        bool refreshInFlight = false;
        bool stale = false;
        QString refreshError;
    };

    class MessageListSession : public QObject
    {
        Q_OBJECT

      public:
        using QObject::QObject;
        ~MessageListSession() override = default;

        [[nodiscard]] virtual const std::string& accountId() const = 0;
        [[nodiscard]] virtual QString title() const = 0;
        [[nodiscard]] virtual const MessageListPage& page() const = 0;
        virtual void loadCachedPage(bool forceReload = false) = 0;
        virtual void refresh() = 0;
        virtual void markStale() = 0;
        virtual void setSort(javelin::jmap::query::EmailListSort sort) = 0;
        [[nodiscard]] virtual bool goToPage(std::size_t pageIndex) = 0;
        [[nodiscard]] virtual bool goToPreviousPage() = 0;
        [[nodiscard]] virtual bool goToNextPage() = 0;

      Q_SIGNALS:
        void pageChanged();
        void refreshFailed(javelin::jmap::OperationError error);
    };
} // namespace javelin::app
