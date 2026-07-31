#include "gui/shell/AccountRefreshController.h"

#include "app/AccountRefreshApplicationPorts.h"
#include "gui/settings/ConnectionSettingsAdapter.h"
#include "gui/settings/PreferencesDialog.h"
#include "jmap/cache/AccountReadRepository.h"

#include <QCoroTask>

#include <QDebug>

#include <utility>
#include <variant>
#include <vector>

namespace javelin::gui::shell
{
    AccountRefreshController::AccountRefreshController(
        javelin::app::AccountRefreshPort& commandPort,
        javelin::jmap::cache::AccountReader& accountReader, QObject* parent)
        : QObject(parent), m_commandPort(commandPort), m_accountReader(accountReader)
    {
    }

    void AccountRefreshController::refreshAccount(std::string accountId)
    {
        if (m_commandPort.requestAccountSynchronization(accountId))
        {
            Q_EMIT statusMessage(QStringLiteral("Synchronizing account..."), 0);
            return;
        }

        refreshConnection(javelin::gui::settings::PreferencesDialog::loadSettingsForAccount(
            QString::fromStdString(accountId)));
    }

    void
    AccountRefreshController::refreshConnection(javelin::gui::settings::ConnectionSettings settings)
    {
        if (m_refreshInFlight)
            return;

        if (settings.loginEmail.isEmpty() || settings.apiKey.isEmpty())
        {
            Q_EMIT userInterventionRequired(
                QStringLiteral("Set Session URL, Login Email, and API Key in Preferences first."));
            return;
        }

        m_refreshInFlight = true;
        Q_EMIT busyChanged(true);
        Q_EMIT statusMessage(QStringLiteral("Refreshing mail from server..."), 0);
        qInfo().noquote() << "GUI refresh requested" << settings.loginEmail << settings.sessionUrl;

        std::vector<std::string> mailboxIds;
        for (const auto& accountId : settings.cachedAccountIds)
        {
            const auto syncedMailboxIds =
                javelin::gui::settings::PreferencesDialog::syncedMailboxIds(accountId);
            for (const auto& mailboxId : syncedMailboxIds)
                mailboxIds.push_back(mailboxId.toStdString());
        }

        auto task = m_commandPort.bootstrapAccount(
            javelin::gui::settings::toAccountBootstrapIntent(settings, std::move(mailboxIds)));
        QCoro::connect(
            std::move(task), this,
            [this, settings = std::move(settings)](javelin::jmap::LiveRefreshResult result)
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
                javelin::gui::settings::PreferencesDialog::saveResolvedSessionUrl(
                    settings.id, QString::fromStdString(summary.resolvedSessionUrl));
                const auto ownedAccounts = m_accountReader.listOwnedBy(summary.accountId);
                if (const auto* accounts =
                        std::get_if<std::vector<javelin::jmap::cache::CachedAccount>>(
                            &ownedAccounts))
                {
                    for (const auto& account : *accounts)
                    {
                        javelin::gui::settings::PreferencesDialog::associateCachedAccount(
                            settings.id, QString::fromStdString(account.accountId));
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
