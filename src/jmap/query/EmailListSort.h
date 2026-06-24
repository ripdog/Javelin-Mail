#pragma once

#include "jmap/api/MailMethods.h"

#include <string>

namespace javelin::jmap::query
{

    enum class EmailListSortProperty
    {
        ReceivedAt = 0,
        SentAt = 1,
        From = 2,
        To = 3,
        Subject = 4,
        Size = 5,
    };

    enum class EmailListSortDirection
    {
        Descending = 0,
        Ascending = 1,
    };

    struct EmailListSort
    {
        EmailListSortProperty property = EmailListSortProperty::ReceivedAt;
        EmailListSortDirection direction = EmailListSortDirection::Descending;
    };

    [[nodiscard]] inline std::string propertyName(const EmailListSortProperty property)
    {
        switch (property)
        {
        case EmailListSortProperty::ReceivedAt:
            return "receivedAt";
        case EmailListSortProperty::SentAt:
            return "sentAt";
        case EmailListSortProperty::From:
            return "from";
        case EmailListSortProperty::To:
            return "to";
        case EmailListSortProperty::Subject:
            return "subject";
        case EmailListSortProperty::Size:
            return "size";
        }

        return "receivedAt";
    };

    [[nodiscard]] inline bool isAscending(const EmailListSort& sort)
    {
        return sort.direction == EmailListSortDirection::Ascending;
    }

    [[nodiscard]] inline javelin::jmap::api::EmailQuerySort
    toEmailQuerySort(const EmailListSort& sort)
    {
        return javelin::jmap::api::EmailQuerySort{
            .property = propertyName(sort.property),
            .isAscending = isAscending(sort),
        };
    }

} // namespace javelin::jmap::query
