#include "app/MailNotificationService.h"

#include "app/MailNotificationDeliveryPort.h"
#include "app/MessageSubject.h"
#include "jmap/cache/MailboxRepository.h"
#include "jmap/cache/NotificationRepository.h"

#include <KLocalizedString>

#include <QDebug>

#include <algorithm>
#include <chrono>
#include <map>
#include <utility>
#include <vector>

namespace javelin::app
{
    namespace
    {
        constexpr unsigned int mailNotificationLocalRetryMaximumExponent = 5;
        constexpr auto mailNotificationDeliveryRetryDelay = std::chrono::seconds{60};
    } // namespace

    MailNotificationService::MailNotificationService(
        javelin::jmap::cache::DatabaseConnection& databaseConnection, QObject* parent)
        : QObject(parent), m_databaseConnection(databaseConnection)
    {
        m_localRetryTimer.setSingleShot(true);
        connect(&m_localRetryTimer, &QTimer::timeout, this,
                &MailNotificationService::retryLocalFailures);
        m_deliveryRetryTimer.setSingleShot(true);
        m_deliveryRetryTimer.setInterval(mailNotificationDeliveryRetryDelay);
        connect(&m_deliveryRetryTimer, &QTimer::timeout, this,
                &MailNotificationService::retryDeliveries);
    }

    void MailNotificationService::setDeliveryPort(MailNotificationDeliveryPort* deliveryPort)
    {
        m_deliveryPort = deliveryPort;
        if (m_deliveryPort == nullptr)
        {
            m_deliveryRetryTimer.stop();
            return;
        }
        if (!m_deliveryRetryAccounts.isEmpty() && !m_deliveryRetryTimer.isActive())
            m_deliveryRetryTimer.start(0);
    }

    void MailNotificationService::accountChanged(const QString& accountId)
    {
        if (accountId.isEmpty())
            return;
        if (m_deliveryPort == nullptr)
        {
            queueDeliveryRetry(accountId);
            return;
        }

        javelin::jmap::cache::NotificationRepository notifications{m_databaseConnection};
        const auto claimed = notifications.claimPendingEvents(accountId.toStdString());
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&claimed))
        {
            qWarning().noquote() << "Claim mail notification delivery failed" << error->message;
            queueDeliveryRetry(accountId);
            return;
        }

        const auto& pending =
            std::get<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(claimed);
        if (pending.empty())
            return;

        std::map<std::string,
                 std::vector<const javelin::jmap::cache::MailNotificationPendingEvent*>>
            byMailbox;
        for (const auto& event : pending)
            byMailbox[event.mailboxId].push_back(&event);

        javelin::jmap::cache::MailboxRepository mailboxes{m_databaseConnection};
        for (const auto& [mailboxId, events] : byMailbox)
        {
            QString mailboxName = QString::fromStdString(mailboxId);
            const auto mailboxResult = mailboxes.find(accountId.toStdString(), mailboxId);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&mailboxResult))
            {
                qWarning().noquote() << "Read notification mailbox name failed" << error->message;
            }
            else if (const auto& mailbox =
                         std::get<std::optional<javelin::jmap::domain::Mailbox>>(mailboxResult);
                     mailbox.has_value())
            {
                mailboxName = QString::fromStdString(mailbox->name);
            }

            const auto& target = *events.front();
            const auto title = events.size() == 1
                                   ? i18n("New mail in %1", mailboxName)
                                   : i18np("%1 new message in %2", "%1 new messages in %2",
                                           events.size(), mailboxName);
            const auto message = subjectForDisplay(target.subject);
            QStringList deliveredEmailIds;
            deliveredEmailIds.reserve(static_cast<qsizetype>(events.size()));
            for (const auto* event : events)
                deliveredEmailIds.push_back(QString::fromStdString(event->emailId));

            const MailNotificationDelivery delivery{
                .accountId = accountId,
                .mailboxId = QString::fromStdString(mailboxId),
                .threadId = QString::fromStdString(target.threadId),
                .emailId = QString::fromStdString(target.emailId),
                .mailboxName = mailboxName,
                .title = title,
                .message = message,
            };
            if (m_deliveryPort->deliverNewMail(delivery))
            {
                if (const auto error = markDelivered(accountId.toStdString(), deliveredEmailIds))
                    qWarning().noquote() << "Record mail notification delivery" << error->message;
                continue;
            }

            if (const auto error = releaseDispatches(accountId.toStdString(), deliveredEmailIds))
            {
                qWarning().noquote() << "Release mail notification delivery" << error->message;
                continue;
            }
            queueDeliveryRetry(accountId);
        }
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MailNotificationService::markDelivered(const std::string_view accountId,
                                           const QStringList& emailIds)
    {
        std::vector<std::string> ids;
        ids.reserve(static_cast<std::size_t>(emailIds.size()));
        for (const auto& emailId : emailIds)
            ids.push_back(emailId.toStdString());
        javelin::jmap::cache::NotificationRepository notifications{m_databaseConnection};
        const auto error = notifications.markDelivered(accountId, ids);
        if (error.has_value())
            rememberLocalRetry(m_markDeliveredRetries, QString::fromUtf8(accountId), emailIds);
        return error;
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MailNotificationService::releaseDispatches(const std::string_view accountId,
                                               const QStringList& emailIds)
    {
        std::vector<std::string> ids;
        ids.reserve(static_cast<std::size_t>(emailIds.size()));
        for (const auto& emailId : emailIds)
            ids.push_back(emailId.toStdString());
        javelin::jmap::cache::NotificationRepository notifications{m_databaseConnection};
        const auto error = notifications.releaseDispatches(accountId, ids);
        if (error.has_value())
            rememberLocalRetry(m_releaseDispatchRetries, QString::fromUtf8(accountId), emailIds);
        return error;
    }

    void MailNotificationService::rememberLocalRetry(RetryMap& retries, QString accountId,
                                                     const QStringList& emailIds)
    {
        if (emailIds.isEmpty())
            return;
        auto& pending = retries[std::move(accountId)];
        for (const auto& emailId : emailIds)
            pending.insert(emailId);
        scheduleLocalRetry();
    }

    void MailNotificationService::scheduleLocalRetry()
    {
        if (m_localRetryTimer.isActive() ||
            (m_markDeliveredRetries.isEmpty() && m_releaseDispatchRetries.isEmpty()))
            return;

        if (m_localRetryAttempts == 0)
        {
            ++m_localRetryAttempts;
            m_localRetryTimer.start(0);
            return;
        }

        const auto exponent =
            std::min(m_localRetryAttempts - 1, mailNotificationLocalRetryMaximumExponent);
        ++m_localRetryAttempts;
        m_localRetryTimer.start(
            static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::seconds{1U << exponent})
                                 .count()));
    }

    void MailNotificationService::retryLocalFailures()
    {
        auto deliveredRetries = std::exchange(m_markDeliveredRetries, {});
        auto releaseRetries = std::exchange(m_releaseDispatchRetries, {});

        for (auto it = deliveredRetries.cbegin(); it != deliveredRetries.cend(); ++it)
        {
            QStringList emailIds;
            emailIds.reserve(it.value().size());
            for (const auto& emailId : it.value())
                emailIds.push_back(emailId);
            if (const auto error = markDelivered(it.key().toStdString(), emailIds))
                qWarning().noquote()
                    << "Retry mail notification delivery acknowledgement failed" << error->message;
        }

        for (auto it = releaseRetries.cbegin(); it != releaseRetries.cend(); ++it)
        {
            QStringList emailIds;
            emailIds.reserve(it.value().size());
            for (const auto& emailId : it.value())
                emailIds.push_back(emailId);
            if (const auto error = releaseDispatches(it.key().toStdString(), emailIds))
            {
                qWarning().noquote()
                    << "Retry mail notification dispatch release failed" << error->message;
                continue;
            }
            queueDeliveryRetry(it.key());
        }

        if (m_markDeliveredRetries.isEmpty() && m_releaseDispatchRetries.isEmpty())
            m_localRetryAttempts = 0;
        else
            scheduleLocalRetry();
    }

    void MailNotificationService::queueDeliveryRetry(QString accountId)
    {
        if (accountId.isEmpty())
            return;
        m_deliveryRetryAccounts.insert(std::move(accountId));
        if (m_deliveryPort != nullptr && !m_deliveryRetryTimer.isActive())
            m_deliveryRetryTimer.start();
    }

    void MailNotificationService::retryDeliveries()
    {
        if (m_deliveryPort == nullptr)
            return;
        const auto accounts = std::exchange(m_deliveryRetryAccounts, {});
        for (const auto& accountId : accounts)
            accountChanged(accountId);
    }

    std::optional<javelin::jmap::cache::DatabaseError> MailNotificationService::recoverDispatches()
    {
        javelin::jmap::cache::NotificationRepository notifications{m_databaseConnection};
        return notifications.recoverDispatches();
    }
} // namespace javelin::app
