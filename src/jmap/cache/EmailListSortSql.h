#pragma once

#include "jmap/query/EmailListSort.h"

#include <QString>

namespace javelin::jmap::cache::detail
{
    [[nodiscard]] inline QString
    emailListSortKeyExpression(const javelin::jmap::query::EmailListSortProperty property)
    {
        switch (property)
        {
        case javelin::jmap::query::EmailListSortProperty::ReceivedAt:
            return QStringLiteral("e.received_at");
        case javelin::jmap::query::EmailListSortProperty::SentAt:
            return QStringLiteral("COALESCE(e.sent_at, e.received_at)");
        case javelin::jmap::query::EmailListSortProperty::From:
            return QStringLiteral("LOWER(COALESCE((SELECT a.address FROM email_addresses a "
                                  "WHERE a.account_id = e.account_id AND a.email_id = e.email_id "
                                  "AND a.field_name = 'from' ORDER BY a.position LIMIT 1), ''))");
        case javelin::jmap::query::EmailListSortProperty::To:
            return QStringLiteral("LOWER(COALESCE((SELECT a.address FROM email_addresses a "
                                  "WHERE a.account_id = e.account_id AND a.email_id = e.email_id "
                                  "AND a.field_name = 'to' ORDER BY a.position LIMIT 1), ''))");
        case javelin::jmap::query::EmailListSortProperty::Subject:
            return QStringLiteral("LOWER(COALESCE(e.subject, ''))");
        case javelin::jmap::query::EmailListSortProperty::Size:
            return QStringLiteral("e.size");
        }
        return QStringLiteral("e.received_at");
    }
} // namespace javelin::jmap::cache::detail
