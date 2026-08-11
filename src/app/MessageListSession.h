#pragma once

#include "jmap/OperationError.h"
#include "jmap/cache/QueryService.h"
#include "jmap/query/EmailListSort.h"

#include <QObject>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace javelin::app
{
    inline constexpr std::size_t maximumRestoredMessageListWindows = 256;

    enum class MessageListRefreshMode
    {
        Materialize,
        RefreshFromServer,
    };

    struct MessageListState
    {
        std::optional<std::size_t> total;
        std::vector<javelin::jmap::cache::MessageListItem> items;
        std::uint64_t itemsRevision = 0;
        bool cacheLoaded = false;
        bool refreshInFlight = false;
        bool loadMoreInFlight = false;
        bool stale = false;
        QString refreshError;
        QString loadMoreError;
    };

    struct MessageListWindowRequest
    {
        std::size_t offset = 0;
        std::size_t limit = 0;

        [[nodiscard]] bool operator==(const MessageListWindowRequest&) const = default;
    };

    struct MessageListWindow
    {
        std::size_t requestedOffset = 0;
        std::size_t requestedLimit = 0;
        std::size_t position = 0;
        std::size_t returnedLimit = 0;
        std::optional<std::size_t> total;
        std::string queryState;
        std::size_t itemCount = 0;
        bool displayCurrent = false;
    };

    class MessageListSession : public QObject
    {
        Q_OBJECT

      public:
        using QObject::QObject;
        ~MessageListSession() override = default;

        [[nodiscard]] virtual const std::string& accountId() const = 0;
        [[nodiscard]] virtual QString title() const = 0;
        [[nodiscard]] virtual const MessageListState& state() const = 0;
        virtual void loadCachedState(bool forceReload = false) = 0;
        virtual void refresh(MessageListRefreshMode mode = MessageListRefreshMode::Materialize) = 0;
        virtual void markStale() = 0;
        virtual void setSort(javelin::jmap::query::EmailListSort sort) = 0;
        [[nodiscard]] virtual bool canLoadMore() const = 0;
        [[nodiscard]] virtual bool loadMore() = 0;
        virtual void ensureThreadMaterialized(std::string threadId) = 0;

      Q_SIGNALS:
        void stateChanged();
        void refreshFailed(javelin::jmap::OperationError error);
    };
} // namespace javelin::app
