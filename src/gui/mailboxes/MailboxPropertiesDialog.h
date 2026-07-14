#pragma once

#include "jmap/cache/QueryService.h"

#include <QDialog>
#include <QString>

namespace javelin::gui::mailboxes
{

    class MailboxPropertiesDialog final : public QDialog
    {
      public:
        MailboxPropertiesDialog(QString accountId,
                                const javelin::jmap::cache::MailboxTreeItem& mailbox,
                                QWidget* parent = nullptr);
    };

} // namespace javelin::gui::mailboxes
