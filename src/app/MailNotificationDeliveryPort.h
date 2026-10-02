#pragma once

#include <QString>

#include <optional>

namespace javelin::app
{
    enum class MailNotificationAuthActionKind
    {
        CopyCode,
        OpenLink,
    };

    struct MailNotificationAuthAction
    {
        MailNotificationAuthActionKind kind = MailNotificationAuthActionKind::CopyCode;
        QString value;

        friend bool operator==(const MailNotificationAuthAction&,
                               const MailNotificationAuthAction&) = default;
    };

    struct MailNotificationDelivery
    {
        QString accountId;
        QString mailboxId;
        QString threadId;
        QString emailId;
        QString mailboxName;
        QString title;
        QString message;
        std::optional<MailNotificationAuthAction> authAction = std::nullopt;
    };

    class MailNotificationDeliveryPort
    {
      public:
        virtual ~MailNotificationDeliveryPort() = default;

        [[nodiscard]] virtual bool deliverNewMail(const MailNotificationDelivery& notification) = 0;
    };
} // namespace javelin::app
