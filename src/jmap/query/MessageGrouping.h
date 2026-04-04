#pragma once

#include "jmap/cache/QueryService.h"

#include <QDate>

#include <cstddef>
#include <string>
#include <vector>

namespace javelin::jmap::query
{

    enum class MessageGroupGranularity
    {
        Day,
        Month,
    };

    struct MessageGroup
    {
        std::string id;
        std::string label;
        MessageGroupGranularity granularity = MessageGroupGranularity::Day;
        std::size_t startIndex = 0;
        std::size_t count = 0;
    };

    [[nodiscard]] std::vector<MessageGroup>
    groupMessagesByTime(const std::vector<javelin::jmap::cache::MessageListItem>& items,
                        const QDate& referenceDateUtc);

} // namespace javelin::jmap::query
