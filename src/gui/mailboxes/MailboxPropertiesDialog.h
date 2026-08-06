#pragma once

#include "jmap/cache/MailboxReadRepository.h"

#include <QDialog>
#include <QString>

namespace javelin::gui::mailboxes
{

    class MailboxPropertiesDialog final : public QDialog
    {
      public:
        MailboxPropertiesDialog(QString accountName, QString parentMailboxName,
                                const javelin::jmap::cache::MailboxTreeItem& mailbox,
                                QWidget* parent = nullptr);
    };

} // namespace javelin::gui::mailboxes
