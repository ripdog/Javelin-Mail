#pragma once

#include <QString>

namespace javelin::app
{
    struct MailNotificationDelivery
    {
        QString accountId;
        QString mailboxId;
        QString threadId;
        QString emailId;
        QString mailboxName;
        QString title;
        QString message;
    };

    class MailNotificationDeliveryPort
    {
      public:
        virtual ~MailNotificationDeliveryPort() = default;

        [[nodiscard]] virtual bool deliverNewMail(const MailNotificationDelivery& notification) = 0;
    };
} // namespace javelin::app
