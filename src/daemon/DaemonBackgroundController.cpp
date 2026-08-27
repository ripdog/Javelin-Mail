#include "daemon/DaemonBackgroundController.h"

#include "app/AccountRuntimeManager.h"
#include "app/ApplicationErrorCoordinator.h"
#include "app/CalendarApplicationService.h"
#include "app/CalendarInvitationService.h"
#include "app/CalendarNotificationService.h"
#include "app/ComposePreferences.h"
#include "app/DeferredSendService.h"
#include "app/FullMailSyncService.h"
#include "app/LocalMaintenanceService.h"
#include "app/MailApplicationEventsPorts.h"
#include "app/MailApplicationPorts.h"
#include "app/MailIndexService.h"
#include "app/MailMutationApplicationService.h"
#include "app/MailNotificationService.h"
#include "app/MailQueryApplicationService.h"
#include "app/MailboxSelectionMutation.h"
#include "app/MessageSelection.h"
#include "daemon/DaemonServices.h"
#include "desktop/notifications/DesktopNotificationController.h"
#include "desktop/tray/DaemonTrayController.h"

#include <KLocalizedString>
#include <QCoroTask>

#include <QDateTime>
#include <QDebug>
#include <QNetworkInformation>

#include <utility>

namespace javelin::app
{
    DaemonBackgroundController::DaemonBackgroundController(DaemonServices& services,
                                                           QObject* parent)
        : DaemonBackgroundController(services, std::make_unique<DesktopNotificationController>(),
                                     parent)
    {
    }

    DaemonBackgroundController::DaemonBackgroundController(
        DaemonServices& services, std::unique_ptr<DesktopNotificationController> notifications,
        QObject* parent)
        : QObject(parent), m_services(services), m_notifications(std::move(notifications)),
          m_tray(std::make_unique<DaemonTrayController>(services.workScheduler(), this))
    {
        m_notifications->setParent(this);
        connect(m_notifications.get(), &DesktopNotificationController::notificationActivated, this,
                [this](const QString& accountId, const QString& mailboxId,
                       const QString& mailboxName, const QString& threadId, const QString& emailId,
                       const QString& activationToken)
                {
                    Q_EMIT activationRequested(protocol::OpenMessageRoute{
                        .accountId = accountId,
                        .mailboxId = mailboxId,
                        .mailboxName = mailboxName,
                        .threadId = threadId,
                        .emailId = emailId,
                        .activationToken = activationToken,
                    });
                });
        connect(m_notifications.get(), &DesktopNotificationController::mailArchiveRequested, this,
                [this](const QString& accountId, const QString& mailboxId, const QString& emailId)
                {
                    if (accountId.isEmpty() || mailboxId.isEmpty() || emailId.isEmpty())
                        return;
                    MessageSelection selection;
                    selection.emplace_back(SelectedEmail{.emailId = emailId.toStdString()});
                    auto task = m_services.mailCommandPort().queueMailboxSelectionMutation({
                        .accountId = accountId.toStdString(),
                        .selection = std::move(selection),
                        .operation = MailboxSelectionOperation::Archive,
                        .sourceMailboxId = mailboxId.toStdString(),
                        .destinationMailboxId = std::nullopt,
                    });
                    QCoro::connect(
                        std::move(task), this,
                        [this, accountId = accountId.toStdString()](
                            QueuedMailboxSelectionMutationResult result)
                        {
                            if (const auto* error =
                                    std::get_if<javelin::jmap::OperationError>(&result))
                            {
                                m_notifications->notifyError({}, i18n("Unable to archive message"),
                                                             error->message, false, false);
                                return;
                            }
                            const auto& queued = std::get<QueuedMailboxSelectionMutation>(result);
                            if (queued.queuedEmailCount == 0 || queued.queuedMutations.empty())
                                return;
                            submitNotificationMutations(
                                accountId, queued.queuedMutations.front().patch.operationGroupId,
                                i18n("Unable to archive message"));
                        });
                });
        connect(
            m_notifications.get(), &DesktopNotificationController::mailMarkReadRequested, this,
            [this](const QString& accountId, const QString& emailId)
            {
                if (accountId.isEmpty() || emailId.isEmpty())
                    return;
                auto task = m_services.mailCommandPort().queueMarkEmailRead(accountId.toStdString(),
                                                                            emailId.toStdString());
                QCoro::connect(
                    std::move(task), this,
                    [this, accountId =
                               accountId.toStdString()](QueuedMessageSelectionMutationResult result)
                    {
                        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                        {
                            m_notifications->notifyError({}, i18n("Unable to mark message read"),
                                                         error->message, false, false);
                            return;
                        }
                        const auto& queued = std::get<QueuedMessageSelectionMutation>(result);
                        if (queued.queuedEmailCount == 0 || queued.queuedMutations.empty())
                            return;
                        submitNotificationMutations(
                            accountId, queued.queuedMutations.front().patch.operationGroupId,
                            i18n("Unable to mark message read"));
                    });
            });
        connect(
            m_notifications.get(), &DesktopNotificationController::mailReplyRequested, this,
            [this](const QString& accountId, const QString& emailId, const QString& activationToken)
            {
                if (accountId.isEmpty() || emailId.isEmpty())
                    return;
                Q_EMIT activationRequested(protocol::ReplyMessageRoute{
                    .accountId = accountId,
                    .emailId = emailId,
                    .activationToken = activationToken,
                });
            });
        connect(m_notifications.get(), &DesktopNotificationController::errorNotificationActivated,
                this,
                [this](const QString& connectionId, const QString& activationToken)
                {
                    Q_EMIT activationRequested(protocol::OpenSettingsRoute{
                        .connectionId = connectionId,
                        .activationToken = activationToken,
                    });
                });
        m_notificationRetryTimer.setSingleShot(true);
        m_notificationRetryTimer.setInterval(60000);
        connect(&m_notificationRetryTimer, &QTimer::timeout, this,
                &DaemonBackgroundController::retryMailNotifications);
    }

    DaemonBackgroundController::~DaemonBackgroundController()
    {
        stop();
    }

    void DaemonBackgroundController::start(const bool enableNetworkReachability)
    {
        if (m_started)
            return;
        m_started = true;

        auto& accountRuntime = m_services.accountRuntimeManager();
        auto& notificationService = m_services.mailNotificationService();
        connect(&notificationService, &MailNotificationService::notificationRaised, this,
                [this](const QString& accountId, const QString& mailboxId, const QString& threadId,
                       const QString& emailId, const QString& mailboxName, const QString& title,
                       const QString& message, const QStringList& deliveredEmailIds)
                {
                    if (m_notifications->notifyNewMail(accountId, mailboxId, threadId, emailId,
                                                       mailboxName, title, message))
                    {
                        if (const auto error = m_services.mailNotificationService().markDelivered(
                                accountId.toStdString(), deliveredEmailIds))
                            qWarning().noquote()
                                << QStringLiteral("Record mail notification delivery:")
                                << error->message;
                        return;
                    }

                    if (const auto error = m_services.mailNotificationService().releaseDispatches(
                            accountId.toStdString(), deliveredEmailIds))
                        qWarning().noquote()
                            << QStringLiteral("Release mail notification delivery:")
                            << error->message;
                    queueNotificationRetry(accountId);
                });
        const auto cacheCommitted = [this](MailCacheChange change)
        {
            m_services.localMaintenanceService().requestReplay();
            const bool mailCacheChanged = !change.mailboxIds.isEmpty() ||
                                          !change.queryWindows.empty() ||
                                          !change.searchWindows.empty() ||
                                          change.mailboxTreeChanged || change.emailObjectsChanged;
            if (!change.optimisticProjection && mailCacheChanged)
                m_services.fullMailSyncService().requestCatchUp(change.accountId.toStdString());
            if (change.emailObjectsChanged)
                m_services.mailIndexService().requestIndex(change.accountId.toStdString());
            if (mailCacheChanged)
                refreshTrayUnreadCount();
        };
        connect(&accountRuntime, &AccountRuntimeManager::cacheCommitted, this, cacheCommitted);
        connect(&accountRuntime, &AccountRuntimeManager::accountStatusChanged, this,
                [this](const QString&, AccountSyncCoordinator::Status)
                { refreshTrayUnreadCount(); });
        connect(&m_services.mailQueryApplicationService(),
                &MailQueryApplicationService::cacheCommitted, this, cacheCommitted);
        connect(&m_services.mailMutationApplicationService(),
                &MailMutationApplicationService::cacheCommitted, this, cacheCommitted);
        connect(&m_services.errorCoordinator(), &ApplicationErrorCoordinator::incidentRaised, this,
                [this](const QString& connectionId, const QString&, const QString& title,
                       const QString& message, const bool persistent, const bool opensSettings)
                {
                    m_notifications->notifyError(connectionId, title, message, persistent,
                                                 opensSettings);
                });
        connect(&m_services.calendarNotificationService(),
                &CalendarNotificationService::reminderDue, this,
                [this](const QString& key, const QString& title, const QString& message)
                {
                    if (m_notifications->notifyCalendarEvent(key, title, message))
                        m_services.calendarNotificationService().deliveryAccepted(key);
                    else
                        m_services.calendarNotificationService().deliveryFailed(key);
                });
        connect(m_notifications.get(), &DesktopNotificationController::calendarNotificationAction,
                this,
                [this](const QString& key, const bool snooze)
                {
                    if (snooze)
                        m_services.calendarNotificationService().snooze(key);
                    else
                        m_services.calendarNotificationService().dismiss(key);
                });
        connect(&m_services.calendarInvitationService(),
                &CalendarInvitationService::invitationReady, this,
                [this](const QString& key, const QString& calendarAccountId, const QString& eventId,
                       const QString& recurrenceId, const QString& navigationDate,
                       const QString& title, const QString& message)
                {
                    if (m_notifications->notifyCalendarInvitation(key, calendarAccountId, eventId,
                                                                  recurrenceId, navigationDate,
                                                                  title, message))
                        m_services.calendarInvitationService().deliveryAccepted(key);
                    else
                        m_services.calendarInvitationService().deliveryFailed(key);
                });
        connect(&m_services.calendarInvitationService(),
                &CalendarInvitationService::invitationResolved, m_notifications.get(),
                &DesktopNotificationController::closeCalendarInvitation);
        connect(m_notifications.get(), &DesktopNotificationController::calendarInvitationActivated,
                this,
                [this](const QString&, const QString& calendarAccountId, const QString& eventId,
                       const QString& recurrenceId, const QString& navigationDate,
                       const QString& activationToken)
                {
                    Q_EMIT activationRequested(protocol::OpenCalendarEventRoute{
                        .calendarAccountId = calendarAccountId,
                        .eventId = eventId,
                        .recurrenceId = recurrenceId.isEmpty()
                                            ? std::nullopt
                                            : std::optional<QString>{recurrenceId},
                        .navigationDate = navigationDate,
                        .activationToken = activationToken,
                    });
                });
        connect(&m_services.deferredSendService(), &DeferredSendService::undoableSendScheduled,
                this,
                [this](const QString& sendId, const QString& title, const QString& message,
                       const int timeoutMs)
                {
                    if (ComposePreferences::undoSendUsesDialog())
                    {
                        Q_EMIT activationRequested(protocol::ShowUndoSendDialogRoute{
                            .sendId = sendId,
                            .title = title,
                            .message = message,
                            .deadlineEpochMilliseconds =
                                QDateTime::currentMSecsSinceEpoch() + timeoutMs,
                        });
                        return;
                    }
                    const bool lifetimeTracked =
                        m_notifications->notifyUndoableSend(sendId, title, message, timeoutMs);
                    if (lifetimeTracked)
                        m_services.deferredSendService().notificationWindowPresented(sendId);
                });
        connect(
            &m_services.deferredSendService(), &DeferredSendService::undoableSendWaiting,
            m_notifications.get(),
            [this](const QString& sendId, const QString& title, const QString& message)
            { static_cast<void>(m_notifications->notifyUndoableSend(sendId, title, message, 0)); });
        connect(&m_services.deferredSendService(), &DeferredSendService::undoableSendClosed, this,
                [this](const QString& sendId)
                {
                    m_notifications->closeUndoableSendNotification(sendId);
                    Q_EMIT activationRequested(
                        protocol::CloseUndoSendDialogRoute{.sendId = sendId});
                });
        connect(m_notifications.get(), &DesktopNotificationController::undoableSendWindowEnded,
                this, [this](const QString& sendId, DesktopNotificationCloseReason)
                { m_services.deferredSendService().notificationWindowEnded(sendId); });
        connect(m_notifications.get(), &DesktopNotificationController::undoSendRequested, this,
                [this](const QString& sendId)
                {
                    const auto cancelled = m_services.deferredSendService().cancelTargeted(sendId);
                    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&cancelled))
                    {
                        m_notifications->notifyError({}, QStringLiteral("Unable to undo message"),
                                                     error->message, false, false);
                    }
                    else if (!std::get<bool>(cancelled))
                    {
                        m_notifications->notifyError(
                            {}, QStringLiteral("Unable to undo message"),
                            QStringLiteral("The message has already started sending."), false,
                            false);
                    }
                });
        connect(&m_services.deferredSendService(), &DeferredSendService::draftRestoreRequested,
                this,
                [this](const QString& accountId, const QString& draftEmailId,
                       const QString& composeSessionId)
                {
                    Q_EMIT activationRequested(protocol::RestoreDraftRoute{
                        .accountId = accountId,
                        .draftEmailId = draftEmailId,
                        .composeSessionId = composeSessionId,
                        .activationToken = {},
                    });
                });
        connect(&m_services.deferredSendService(), &DeferredSendService::sendFailed,
                m_notifications.get(),
                [this](const QString&, const QString& message)
                {
                    m_notifications->notifyError({}, QStringLiteral("Unable to send message"),
                                                 message, true, false);
                });
        connect(m_tray.get(), &DaemonTrayController::raiseGuiRequested, this,
                [this](const QString& activationToken)
                {
                    Q_EMIT activationRequested(
                        protocol::RaiseGuiRoute{.activationToken = activationToken});
                });
        connect(m_tray.get(), &DaemonTrayController::newMessageRequested, this,
                [this] { Q_EMIT activationRequested(protocol::NewMessageRoute{}); });
        connect(m_tray.get(), &DaemonTrayController::inboxRequested, this,
                [this]
                {
                    Q_EMIT activationRequested(protocol::OpenWorkspaceRoute{
                        .section = protocol::WorkspaceSection::Inbox,
                        .activationToken = {},
                    });
                });
        connect(m_tray.get(), &DaemonTrayController::contactsRequested, this,
                [this]
                {
                    Q_EMIT activationRequested(protocol::OpenWorkspaceRoute{
                        .section = protocol::WorkspaceSection::Contacts,
                        .activationToken = {},
                    });
                });
        connect(m_tray.get(), &DaemonTrayController::calendarRequested, this,
                [this]
                {
                    Q_EMIT activationRequested(protocol::OpenWorkspaceRoute{
                        .section = protocol::WorkspaceSection::Calendar,
                        .activationToken = {},
                    });
                });
        connect(m_tray.get(), &DaemonTrayController::taskCenterRequested, this,
                [this] { Q_EMIT activationRequested(protocol::OpenTaskCenterRoute{}); });
        connect(m_tray.get(), &DaemonTrayController::stopBackgroundServiceRequested, this,
                &DaemonBackgroundController::shutdownRequested);

        if (const auto error = notificationService.recoverDispatches())
            qWarning().noquote() << QStringLiteral("Recover mail notification delivery:")
                                 << error->message;
        else
            for (const auto& accountId : accountRuntime.configuredAccountIds())
                notificationService.accountChanged(QString::fromStdString(accountId));
        m_services.deferredSendService().start();
        m_services.calendarNotificationService().start();
        m_services.calendarInvitationService().start();
        for (const auto& ownerAccountId :
             m_services.calendarApplicationService().calendarMetadataReadyOwners())
        {
            m_services.calendarInvitationService().accountChanged(
                QString::fromStdString(ownerAccountId));
        }
        if (enableNetworkReachability)
            setupNetworkReachability();
        refreshTrayUnreadCount();
        if (!m_tray->start())
            qInfo() << QStringLiteral("Tray integration is unavailable; daemon services continue");
    }

    void DaemonBackgroundController::stop()
    {
        m_notifications->closeAllUndoableSendNotifications();
        if (!m_started && !m_tray->isAvailable())
            return;
        m_notificationRetryTimer.stop();
        m_notificationRetryAccounts.clear();
        m_tray->stop();
        m_started = false;
    }

    void DaemonBackgroundController::setupNetworkReachability()
    {
        if (!QNetworkInformation::loadDefaultBackend())
        {
            qWarning() << QStringLiteral(
                "Network reachability backend unavailable; resume watchdog remains active");
            return;
        }

        auto* networkInformation = QNetworkInformation::instance();
        if (networkInformation == nullptr)
            return;

        connect(networkInformation, &QNetworkInformation::reachabilityChanged, this,
                [this](const QNetworkInformation::Reachability reachability)
                {
                    if (reachability != QNetworkInformation::Reachability::Online)
                        return;
                    qInfo() << QStringLiteral(
                        "Network became reachable; reconnecting account synchronization");
                    m_services.accountRuntimeManager().networkBecameReachable();
                });
    }

    void DaemonBackgroundController::retryMailNotifications()
    {
        const auto accounts = std::exchange(m_notificationRetryAccounts, {});
        for (const auto& accountId : accounts)
            m_services.mailNotificationService().accountChanged(accountId);
    }

    void DaemonBackgroundController::refreshTrayUnreadCount()
    {
        std::uint64_t unreadCount = 0;
        bool attentionRequired = false;
        for (const auto& [accountId, status] : m_services.accountRuntimeManager().accountStatuses())
        {
            attentionRequired =
                attentionRequired || status == AccountSyncCoordinator::Status::AuthenticationPaused;
            const auto mailboxes = m_services.mailboxReader().listMailboxTree(accountId);
            const auto* items =
                std::get_if<std::vector<javelin::jmap::cache::MailboxTreeItem>>(&mailboxes);
            if (items == nullptr)
                continue;

            for (const auto& mailbox : *items)
            {
                if (mailbox.role.has_value() && *mailbox.role == "inbox")
                    unreadCount += mailbox.unreadEmails;
            }
        }
        m_tray->setInboxUnreadCount(unreadCount);
        m_tray->setAttentionRequired(attentionRequired);
    }

    void DaemonBackgroundController::submitNotificationMutations(
        std::string accountId, std::optional<std::string> operationGroupId, QString failureTitle)
    {
        auto task = m_services.mailCommandPort().submitPendingEmailMutations(
            std::move(accountId), std::move(operationGroupId));
        QCoro::connect(
            std::move(task), this,
            [this, failureTitle =
                       std::move(failureTitle)](javelin::jmap::SubmittedEmailMutationsResult result)
            {
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    m_notifications->notifyError({}, failureTitle, error->message, false, false);
                    return;
                }

                const auto& submitted = std::get<javelin::jmap::SubmittedEmailMutations>(result);
                if (submitted.failedEmailCount == 0)
                    return;

                QString detail = i18n("The server rejected the requested message change.");
                for (const auto& item : submitted.items)
                {
                    if (item.error.has_value() && !item.error->empty())
                    {
                        detail = QString::fromStdString(*item.error);
                        break;
                    }
                }
                m_notifications->notifyError({}, failureTitle, detail, false, false);
            });
    }

    void DaemonBackgroundController::queueNotificationRetry(const QString& accountId)
    {
        m_notificationRetryAccounts.insert(accountId);
        if (!m_notificationRetryTimer.isActive())
            m_notificationRetryTimer.start();
    }
} // namespace javelin::app
