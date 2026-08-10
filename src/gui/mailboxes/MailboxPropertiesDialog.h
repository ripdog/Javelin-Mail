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
                                bool availableOffline, bool notificationsEnabled,
                                QWidget* parent = nullptr);

        [[nodiscard]] bool deleteRequested() const;

      private:
        bool m_deleteRequested = false;
    };

} // namespace javelin::gui::mailboxes
