#include "jmap/query/MessageGrouping.h"

#include <QDateTime>

namespace javelin::jmap::query
{

    namespace
    {

        struct GroupIdentity
        {
            std::string id;
            std::string label;
            MessageGroupGranularity granularity = MessageGroupGranularity::Day;
        };

        [[nodiscard]] GroupIdentity classifyTimestamp(const std::string_view timestamp,
                                                      const QDate& referenceDateUtc)
        {
            const auto dateTime =
                QDateTime::fromString(QString::fromStdString(std::string{timestamp}), Qt::ISODate);
            const auto utcDate = dateTime.toUTC().date();
            const auto daysAgo = utcDate.daysTo(referenceDateUtc);
            if (daysAgo <= 6)
            {
                const auto isoDate = utcDate.toString(Qt::ISODate).toStdString();
                return GroupIdentity{
                    .id = "day:" + isoDate,
                    .label = isoDate,
                    .granularity = MessageGroupGranularity::Day,
                };
            }

            const auto yearMonth = utcDate.toString("yyyy-MM").toStdString();
            return GroupIdentity{
                .id = "month:" + yearMonth,
                .label = yearMonth,
                .granularity = MessageGroupGranularity::Month,
            };
        }

    } // namespace

    std::vector<MessageGroup>
    groupMessagesByTime(const std::vector<javelin::jmap::cache::MessageListItem>& items,
                        const QDate& referenceDateUtc)
    {
        std::vector<MessageGroup> groups;
        if (items.empty())
        {
            return groups;
        }

        for (std::size_t index = 0; index < items.size(); ++index)
        {
            const auto identity = classifyTimestamp(items[index].receivedAt, referenceDateUtc);
            if (groups.empty() || groups.back().id != identity.id)
            {
                groups.push_back(MessageGroup{
                    .id = identity.id,
                    .label = identity.label,
                    .granularity = identity.granularity,
                    .startIndex = index,
                    .count = 1,
                });
                continue;
            }

            ++groups.back().count;
        }

        return groups;
    }

} // namespace javelin::jmap::query
