#pragma once

#include "app/MailApplicationTypes.h"
#include "jmap/sync/MailCommitEffects.h"

namespace javelin::app
{
    [[nodiscard]] inline MailBackgroundEffects
    backgroundEffects(const javelin::jmap::sync::MailCommitEffects& effects,
                      const bool accountWideRecovery = false)
    {
        MailBackgroundEffects background;
        if (accountWideRecovery)
            background.offlineCatchUp.accountWide = true;
        else if (effects.mailboxMembershipChanged || effects.sourceIdentityChanged)
        {
            for (const auto& mailboxId : effects.affectedMailboxIds)
                background.offlineCatchUp.addMailbox(QString::fromStdString(mailboxId));
        }
        background.vaultProjectionWorkQueued = effects.mailboxMembershipChanged;
        return background;
    }
} // namespace javelin::app
