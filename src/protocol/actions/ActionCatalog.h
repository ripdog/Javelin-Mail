#pragma once

#include "protocol/actions/AccountActions.h"
#include "protocol/actions/CalendarActions.h"
#include "protocol/actions/ComposeActions.h"
#include "protocol/actions/ContactActions.h"
#include "protocol/actions/DeveloperActions.h"
#include "protocol/actions/HistoryActions.h"
#include "protocol/actions/IdentityActions.h"
#include "protocol/actions/MailActions.h"
#include "protocol/actions/SieveActions.h"
#include "protocol/actions/WorkActions.h"

#include <array>
#include <optional>

namespace javelin::protocol::actions
{
    inline constexpr std::array actionCatalog{
        metadata<RemoveConfiguredAccount>(u"RemoveConfiguredAccount"),
        metadata<CalendarReadCached>(u"CalendarReadCached"),
        metadata<CalendarReadAccounts>(u"CalendarReadAccounts"),
        metadata<CalendarReadCalendars>(u"CalendarReadCalendars"),
        metadata<CalendarRequestRange>(u"CalendarRequestRange"),
        metadata<CalendarCreateEvent>(u"CalendarCreateEvent"),
        metadata<CalendarUpdateEvent>(u"CalendarUpdateEvent"),
        metadata<CalendarDeleteEvent>(u"CalendarDeleteEvent"),
        metadata<CalendarSetDefault>(u"CalendarSetDefault"),
        metadata<CalendarCreate>(u"CalendarCreate"),
        metadata<CalendarDelete>(u"CalendarDelete"),
        metadata<CalendarSetVisible>(u"CalendarSetVisible"),
        metadata<ComposeOpen>(u"ComposeOpen"),
        metadata<ComposeLoadSenderIdentities>(u"ComposeLoadSenderIdentities"),
        metadata<ComposeSaveDraft>(u"ComposeSaveDraft"),
        metadata<ComposeSend>(u"ComposeSend"),
        metadata<ComposeLoadWorkingCopy>(u"ComposeLoadWorkingCopy"),
        metadata<ComposeStoreWorkingCopy>(u"ComposeStoreWorkingCopy"),
        metadata<ComposeDiscard>(u"ComposeDiscard"),
        metadata<ContactRequestRefresh>(u"ContactRequestRefresh"),
        metadata<ContactMutateAddressBook>(u"ContactMutateAddressBook"),
        metadata<ContactSave>(u"ContactSave"),
        metadata<ContactSetStarred>(u"ContactSetStarred"),
        metadata<ContactDelete>(u"ContactDelete"),
        metadata<ContactCreateGroup>(u"ContactCreateGroup"),
        metadata<ContactDeleteGroup>(u"ContactDeleteGroup"),
        metadata<ContactSetGroupMembership>(u"ContactSetGroupMembership"),
        metadata<ContactCopy>(u"ContactCopy"),
        metadata<ContactImport>(u"ContactImport"),
        metadata<ContactMerge>(u"ContactMerge"),
        metadata<ContactUploadMedia>(u"ContactUploadMedia"),
        metadata<ContactDownloadMedia>(u"ContactDownloadMedia"),
        metadata<MailQueueMailboxMutation>(u"MailQueueMailboxMutation"),
        metadata<MailQueueDestroy>(u"MailQueueDestroy"),
        metadata<MailQueueMarkUnread>(u"MailQueueMarkUnread"),
        metadata<MailQueueMarkRead>(u"MailQueueMarkRead"),
        metadata<MailQueueSetFlagged>(u"MailQueueSetFlagged"),
        metadata<MailSubmitPending>(u"MailSubmitPending"),
        metadata<SieveList>(u"SieveList"),
        metadata<SieveGet>(u"SieveGet"),
        metadata<SieveValidate>(u"SieveValidate"),
        metadata<SieveSave>(u"SieveSave"),
        metadata<SieveDelete>(u"SieveDelete"),
        metadata<SieveActivate>(u"SieveActivate"),
        metadata<IdentityList>(u"IdentityList"),
        metadata<IdentitySave>(u"IdentitySave"),
        metadata<IdentityDelete>(u"IdentityDelete"),
        metadata<AccountBootstrap>(u"AccountBootstrap"),
        metadata<MessageContent>(u"MessageContent"),
        metadata<AttachmentDownload>(u"AttachmentDownload"),
        metadata<MessageSource>(u"MessageSource"),
        metadata<MailboxObserve>(u"MailboxObserve"),
        metadata<MailboxUnobserve>(u"MailboxUnobserve"),
        metadata<MailboxWindow>(u"MailboxWindow"),
        metadata<SearchWindow>(u"SearchWindow"),
        metadata<SearchRetire>(u"SearchRetire"),
        metadata<Undo>(u"Undo"),
        metadata<Redo>(u"Redo"),
        metadata<UndoAcknowledgeRemove>(u"UndoAcknowledgeRemove"),
        metadata<UndoForget>(u"UndoForget"),
        metadata<UndoSnapshot>(u"UndoSnapshot"),
        metadata<ReloadSettings>(u"ReloadSettings"),
        metadata<WorkPause>(u"WorkPause"),
        metadata<WorkResume>(u"WorkResume"),
        metadata<WorkRetry>(u"WorkRetry"),
        metadata<WorkList>(u"WorkList"),
        metadata<WorkSummary>(u"WorkSummary"),
        metadata<OnboardingDiscover>(u"OnboardingDiscover"),
        metadata<OnboardingStartOAuth>(u"OnboardingStartOAuth"),
        metadata<OnboardingFinishOAuth>(u"OnboardingFinishOAuth"),
        metadata<OnboardingAuthenticateManually>(u"OnboardingAuthenticateManually"),
        metadata<OnboardingRevokeOAuth>(u"OnboardingRevokeOAuth"),
        metadata<OnboardingCancelOAuth>(u"OnboardingCancelOAuth"),
        metadata<CalendarSetSubscribed>(u"CalendarSetSubscribed"),
        metadata<DeveloperDiagnosticsSnapshot>(u"DeveloperDiagnosticsSnapshot"),
        metadata<DeveloperMailboxClear>(u"DeveloperMailboxClear"),
        metadata<AcknowledgeRemoteActionResult>(u"AcknowledgeRemoteActionResult"),
        metadata<DeveloperLogSetSubscribed>(u"DeveloperLogSetSubscribed"),
        metadata<DeveloperLogClear>(u"DeveloperLogClear"),
        metadata<CalendarRespondEvent>(u"CalendarRespondEvent"),
        metadata<MailQueueSetTag>(u"MailQueueSetTag"),
        metadata<MailSaveTagDefinition>(u"MailSaveTagDefinition"),
        metadata<MailDeleteTag>(u"MailDeleteTag"),
        metadata<MailSetMailboxSubscribed>(u"MailSetMailboxSubscribed"),
        metadata<MailCreateMailbox>(u"MailCreateMailbox"),
        metadata<MailDestroyMailbox>(u"MailDestroyMailbox"),
        metadata<ComposeScheduleSend>(u"ComposeScheduleSend"),
        metadata<MailQueueSetSelectionFlagged>(u"MailQueueSetSelectionFlagged"),
        metadata<ThreadEnsure>(u"ThreadEnsure"),
    };

    static_assert(actionCatalog.size() == 89);
    static_assert(
        []
        {
            for (std::size_t index = 0; index < actionCatalog.size(); ++index)
            {
                if (actionCatalog[index].id.value != index)
                    return false;
            }
            return true;
        }(),
        "Action ids are part of the wire contract and must remain stable");

    [[nodiscard]] constexpr std::optional<ActionMetadata> findActionMetadata(const ActionId id)
    {
        for (const auto& item : actionCatalog)
        {
            if (item.id == id)
                return item;
        }
        return std::nullopt;
    }

    [[nodiscard]] constexpr bool isKnownAction(const ActionId id)
    {
        return findActionMetadata(id).has_value();
    }

    [[nodiscard]] inline QString actionName(const ActionId id)
    {
        if (const auto item = findActionMetadata(id))
            return item->name.toString();
        return QStringLiteral("UnknownAction(%1)").arg(id.value);
    }
} // namespace javelin::protocol::actions
