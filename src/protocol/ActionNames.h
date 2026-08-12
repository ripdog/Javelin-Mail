#pragma once

#include "protocol/ActionContract.h"

#include <QString>

#include <array>
#include <string_view>

namespace javelin::protocol
{
    inline constexpr std::array<std::string_view, 89> actionNames{
        "RemoveConfiguredAccount",
        "CalendarReadCached",
        "CalendarReadAccounts",
        "CalendarReadCalendars",
        "CalendarRequestRange",
        "CalendarCreateEvent",
        "CalendarUpdateEvent",
        "CalendarDeleteEvent",
        "CalendarSetDefault",
        "CalendarCreate",
        "CalendarDelete",
        "CalendarSetVisible",
        "ComposeOpen",
        "ComposeLoadSenderIdentities",
        "ComposeSaveDraft",
        "ComposeSend",
        "ComposeLoadWorkingCopy",
        "ComposeStoreWorkingCopy",
        "ComposeDiscard",
        "ContactRequestRefresh",
        "ContactMutateAddressBook",
        "ContactSave",
        "ContactSetStarred",
        "ContactDelete",
        "ContactCreateGroup",
        "ContactDeleteGroup",
        "ContactSetGroupMembership",
        "ContactCopy",
        "ContactImport",
        "ContactMerge",
        "ContactUploadMedia",
        "ContactDownloadMedia",
        "MailQueueMailboxMutation",
        "MailQueueDestroy",
        "MailQueueMarkUnread",
        "MailQueueMarkRead",
        "MailQueueSetFlagged",
        "MailSubmitPending",
        "SieveList",
        "SieveGet",
        "SieveValidate",
        "SieveSave",
        "SieveDelete",
        "SieveActivate",
        "IdentityList",
        "IdentitySave",
        "IdentityDelete",
        "AccountBootstrap",
        "MessageContent",
        "AttachmentDownload",
        "MessageSource",
        "MailboxObserve",
        "MailboxUnobserve",
        "MailboxWindow",
        "SearchWindow",
        "SearchRetire",
        "Undo",
        "Redo",
        "UndoAcknowledgeRemove",
        "UndoForget",
        "UndoSnapshot",
        "ReloadSettings",
        "WorkPause",
        "WorkResume",
        "WorkRetry",
        "WorkList",
        "WorkSummary",
        "OnboardingDiscover",
        "OnboardingStartOAuth",
        "OnboardingFinishOAuth",
        "OnboardingAuthenticateManually",
        "OnboardingRevokeOAuth",
        "OnboardingCancelOAuth",
        "CalendarSetSubscribed",
        "DeveloperDiagnosticsSnapshot",
        "DeveloperMailboxClear",
        "AcknowledgeRemoteActionResult",
        "DeveloperLogSetSubscribed",
        "DeveloperLogClear",
        "CalendarRespondEvent",
        "MailQueueSetTag",
        "MailSaveTagDefinition",
        "MailDeleteTag",
        "MailSetMailboxSubscribed",
        "MailCreateMailbox",
        "MailDestroyMailbox",
        "ComposeScheduleSend",
        "MailQueueSetSelectionFlagged",
        "ThreadEnsure",
    };

    [[nodiscard]] inline QString actionDisplayName(const ActionId action)
    {
        if (action.value >= actionNames.size())
            return QString{};
        const auto name = actionNames[action.value];
        return QString::fromLatin1(name.data(), static_cast<qsizetype>(name.size()));
    }
} // namespace javelin::protocol
