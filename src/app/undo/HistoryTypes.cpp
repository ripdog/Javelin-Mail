#include "app/undo/HistoryTypes.h"

namespace javelin::app::undo
{

    QString toString(const HistoryStack value)
    {
        switch (value)
        {
        case HistoryStack::Undo:
            return QStringLiteral("undo");
        case HistoryStack::Redo:
            return QStringLiteral("redo");
        }
        return {};
    }

    std::optional<HistoryStack> historyStackFromString(const QString& value)
    {
        if (value == QStringLiteral("undo"))
            return HistoryStack::Undo;
        if (value == QStringLiteral("redo"))
            return HistoryStack::Redo;
        return std::nullopt;
    }

    QString toString(const HistoryEntryStatus value)
    {
        switch (value)
        {
        case HistoryEntryStatus::Preparing:
            return QStringLiteral("preparing");
        case HistoryEntryStatus::ExecutingForward:
            return QStringLiteral("executing_forward");
        case HistoryEntryStatus::Ready:
            return QStringLiteral("ready");
        case HistoryEntryStatus::ExecutingUndo:
            return QStringLiteral("executing_undo");
        case HistoryEntryStatus::ExecutingRedo:
            return QStringLiteral("executing_redo");
        case HistoryEntryStatus::BlockedUnknown:
            return QStringLiteral("blocked_unknown");
        case HistoryEntryStatus::BlockedPartial:
            return QStringLiteral("blocked_partial");
        case HistoryEntryStatus::Impossible:
            return QStringLiteral("impossible");
        case HistoryEntryStatus::Expired:
            return QStringLiteral("expired");
        }
        return {};
    }

    std::optional<HistoryEntryStatus> historyEntryStatusFromString(const QString& value)
    {
        if (value == QStringLiteral("preparing"))
            return HistoryEntryStatus::Preparing;
        if (value == QStringLiteral("executing_forward"))
            return HistoryEntryStatus::ExecutingForward;
        if (value == QStringLiteral("ready"))
            return HistoryEntryStatus::Ready;
        if (value == QStringLiteral("executing_undo"))
            return HistoryEntryStatus::ExecutingUndo;
        if (value == QStringLiteral("executing_redo"))
            return HistoryEntryStatus::ExecutingRedo;
        if (value == QStringLiteral("blocked_unknown"))
            return HistoryEntryStatus::BlockedUnknown;
        if (value == QStringLiteral("blocked_partial"))
            return HistoryEntryStatus::BlockedPartial;
        if (value == QStringLiteral("impossible"))
            return HistoryEntryStatus::Impossible;
        if (value == QStringLiteral("expired"))
            return HistoryEntryStatus::Expired;
        return std::nullopt;
    }

    QString toString(const HistoryDomain value)
    {
        switch (value)
        {
        case HistoryDomain::Mail:
            return QStringLiteral("mail");
        case HistoryDomain::Calendar:
            return QStringLiteral("calendar");
        case HistoryDomain::Contacts:
            return QStringLiteral("contacts");
        case HistoryDomain::LocalPreference:
            return QStringLiteral("local_preference");
        case HistoryDomain::DeferredSend:
            return QStringLiteral("deferred_send");
        }
        return {};
    }

    std::optional<HistoryDomain> historyDomainFromString(const QString& value)
    {
        if (value == QStringLiteral("mail"))
            return HistoryDomain::Mail;
        if (value == QStringLiteral("calendar"))
            return HistoryDomain::Calendar;
        if (value == QStringLiteral("contacts"))
            return HistoryDomain::Contacts;
        if (value == QStringLiteral("local_preference"))
            return HistoryDomain::LocalPreference;
        if (value == QStringLiteral("deferred_send"))
            return HistoryDomain::DeferredSend;
        return std::nullopt;
    }

} // namespace javelin::app::undo
