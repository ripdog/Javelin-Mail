#include "gui/shell/AccountRefreshController.h"

#include "app/AccountRefreshApplicationPorts.h"
#include "gui/settings/ConnectionSettingsAdapter.h"
#include "gui/settings/GuiSettings.h"
#include "jmap/cache/AccountReadRepository.h"
#include "jmap/cache/MailboxReadRepository.h"

#include <QCoroTask>

#include <KLocalizedString>

#include <QDebug>

#include <algorithm>
#include <utility>
#include <variant>
#include <vector>

namespace javelin::gui::shell
{
    AccountRefreshController::AccountRefreshController(
        javelin::gui::settings::GuiSettings& settings,
        javelin::app::AccountRefreshPort& commandPort,
        javelin::jmap::cache::AccountReader& accountReader,
        javelin::jmap::cache::MailboxReader& mailboxReader, QObject* parent)
        : QObject(parent), m_settings(settings), m_commandPort(commandPort),
          m_accountReader(accountReader), m_mailboxReader(mailboxReader)
    {
    }

    void AccountRefreshController::refreshAccount(std::string accountId)
    {
        if (m_commandPort.requestAccountSynchronization(accountId))
        {
            Q_EMIT statusMessage(i18n("Synchronizing account..."), 0);
            return;
        }

        refreshConnection(m_settings.accountForCachedId(QString::fromStdString(accountId)));
    }

    void
    AccountRefreshController::refreshConnection(javelin::gui::settings::ConnectionSettings settings)
    {
        if (m_refreshInFlight)
            return;

        if (settings.loginEmail.isEmpty() || !settings.hasCredentials)
        {
            Q_EMIT userInterventionRequired(i18n("Sign in to this account in Preferences first."));
            return;
        }

        const bool initialAccountBootstrap = settings.cachedAccountIds.isEmpty();
        m_refreshInFlight = true;
        Q_EMIT busyChanged(true);
        Q_EMIT statusMessage(i18n("Refreshing mail from server..."), 0);
        qInfo().noquote() << "GUI refresh requested" << settings.loginEmail << settings.sessionUrl;

        std::vector<std::string> mailboxIds;
        for (const auto& accountId : settings.cachedAccountIds)
        {
            const auto syncedMailboxIds = m_settings.syncedMailboxIds(accountId);
            for (const auto& mailboxId : syncedMailboxIds)
                mailboxIds.push_back(mailboxId.toStdString());
        }

        auto task = m_commandPort.bootstrapAccount(
            javelin::gui::settings::toAccountBootstrapIntent(settings, std::move(mailboxIds)));
        QCoro::connect(
            std::move(task), this,
            [this, settings = std::move(settings),
             initialAccountBootstrap](javelin::jmap::LiveRefreshResult result)
            {
                m_refreshInFlight = false;
                Q_EMIT busyChanged(false);

                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    qWarning().noquote() << "GUI refresh failed" << error->message;
                    Q_EMIT operationFailed(*error);
                    return;
                }

                const auto summary = std::get<javelin::jmap::LiveRefreshSummary>(std::move(result));
                if (const auto error = m_settings.saveResolvedSessionUrl(
                        settings.id, QString::fromStdString(summary.resolvedSessionUrl)))
                    Q_EMIT userInterventionRequired(error->detail);

                if (initialAccountBootstrap)
                {
                    const auto mailboxResult = m_mailboxReader.listMailboxTree(summary.accountId);
                    if (const auto* error =
                            std::get_if<javelin::jmap::cache::DatabaseError>(&mailboxResult))
                    {
                        qWarning().noquote()
                            << "Could not inspect Inbox for initial notification settings"
                            << error->message;
                        Q_EMIT userInterventionRequired(
                            i18n("Could not configure the default Inbox notifications."));
                    }
                    else
                    {
                        const auto& mailboxes =
                            std::get<std::vector<javelin::jmap::cache::MailboxTreeItem>>(
                                mailboxResult);
                        const auto inbox =
                            std::ranges::find(mailboxes, std::optional<std::string>{"inbox"},
                                              &javelin::jmap::cache::MailboxTreeItem::role);
                        if (inbox != mailboxes.end())
                        {
                            if (const auto error = m_settings.ensureNotificationMailboxSelected(
                                    QString::fromStdString(summary.accountId),
                                    QString::fromStdString(inbox->id)))
                                Q_EMIT userInterventionRequired(error->detail);
                        }
                    }
                }

                const auto ownedAccounts = m_accountReader.listOwnedBy(summary.accountId);
                if (const auto* accounts =
                        std::get_if<std::vector<javelin::jmap::cache::CachedAccount>>(
                            &ownedAccounts))
                {
                    for (const auto& account : *accounts)
                    {
                        if (const auto error = m_settings.associateCachedAccount(
                                settings.id, QString::fromStdString(account.accountId)))
                            Q_EMIT userInterventionRequired(error->detail);
                    }
                }

                qInfo().noquote() << "GUI refresh succeeded"
                                  << QString::fromStdString(summary.accountId)
                                  << static_cast<qulonglong>(summary.mailboxCount)
                                  << static_cast<qulonglong>(summary.emailCount);
                Q_EMIT accountRefreshed(summary);

                auto contactsTask = m_commandPort.requestContacts(summary.accountId);
                QCoro::connect(
                    std::move(contactsTask), this,
                    [this](javelin::jmap::contacts::ContactRefreshResult contactsResult)
                    {
                        if (const auto* error =
                                std::get_if<javelin::jmap::OperationError>(&contactsResult))
                        {
                            qWarning().noquote() << "Contacts refresh failed" << error->message;
                            return;
                        }

                        const auto contactsSummary =
                            std::get<javelin::jmap::contacts::ContactRefreshSummary>(
                                std::move(contactsResult));
                        qInfo() << "Contacts cache refreshed" << contactsSummary.contactCount;
                        Q_EMIT contactsRefreshed(contactsSummary);
                    });
            });
    }
} // namespace javelin::gui::shell
