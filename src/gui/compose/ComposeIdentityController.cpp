#include "gui/compose/ComposeIdentityController.h"

#include "app/ComposeApplicationPorts.h"
#include "gui/compose/IdentityPresentation.h"
#include "gui/settings/ConnectionSettingsAdapter.h"
#include "gui/settings/GuiSettings.h"
#include "jmap/cache/AccountReadRepository.h"
#include "jmap/cache/IdentityReader.h"

#include <QCoroTask>

#include <KLocalizedString>

#include <QComboBox>
#include <QSignalBlocker>

#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace javelin::gui::compose
{
    ComposeIdentityController::ComposeIdentityController(
        javelin::gui::settings::GuiSettings& settings,
        javelin::app::ComposeCommandPort& composeCommandPort,
        javelin::jmap::cache::AccountReader& accountReader,
        javelin::jmap::cache::IdentityReader& identityReader, QComboBox& combo, QObject& context,
        std::function<void(QString, int)> statusMessage,
        std::function<void()> asynchronousReloadRequested)
        : m_settings(settings), m_composeCommandPort(composeCommandPort),
          m_accountReader(accountReader), m_identityReader(identityReader), m_combo(combo),
          m_context(context), m_statusMessage(std::move(statusMessage)),
          m_asynchronousReloadRequested(std::move(asynchronousReloadRequested))
    {
    }

    void ComposeIdentityController::load(const std::string& selectedAccountId,
                                         const std::string& selectedIdentityId)
    {
        const QSignalBlocker blocker{&m_combo};
        const auto selectedAccount = QString::fromStdString(selectedAccountId);
        const auto selectedIdentity = QString::fromStdString(selectedIdentityId);
        int selectedIndex = -1;

        const auto accountsResult = m_accountReader.listAll();
        const auto* accounts =
            std::get_if<std::vector<javelin::jmap::cache::CachedAccount>>(&accountsResult);
        if (accounts == nullptr)
        {
            if (m_statusMessage)
                m_statusMessage(
                    std::get<javelin::jmap::cache::DatabaseError>(accountsResult).message, 10000);
            return;
        }

        std::unordered_set<std::string> mailAccountIds;
        mailAccountIds.reserve(accounts->size());
        for (const auto& account : *accounts)
        {
            if (account.hasMailCapability && account.hasSubmissionCapability)
                mailAccountIds.insert(account.accountId);
        }

        struct SenderIdentityOption
        {
            QString accountId;
            QString accountDisplayName;
            javelin::jmap::domain::Identity identity;
        };
        std::vector<SenderIdentityOption> options;
        std::unordered_map<std::string, std::size_t> identityCountByEmail;
        std::unordered_set<std::string> accountsWithIdentities;

        m_combo.clear();
        for (const auto& connection : m_settings.accounts())
        {
            const auto accountDisplayName =
                connection.displayName.isEmpty() ? connection.loginEmail : connection.displayName;
            for (const auto& cachedAccountId : connection.cachedAccountIds)
            {
                const auto accountId = cachedAccountId.toStdString();
                if (!mailAccountIds.contains(accountId))
                    continue;
                const auto identitiesResult = m_identityReader.listByAccount(accountId);
                if (const auto* error =
                        std::get_if<javelin::jmap::cache::DatabaseError>(&identitiesResult))
                {
                    if (m_statusMessage)
                        m_statusMessage(error->message, 10000);
                    continue;
                }

                const auto& identities =
                    std::get<std::vector<javelin::jmap::domain::Identity>>(identitiesResult);
                bool hasSenderIdentity = false;
                for (const auto& identity : identities)
                {
                    if (isWildcardSenderIdentity(identity))
                        continue;
                    hasSenderIdentity = true;
                    const auto emailKey = QString::fromStdString(identity.email)
                                              .trimmed()
                                              .toCaseFolded()
                                              .toStdString();
                    ++identityCountByEmail[emailKey];
                    accountsWithIdentities.insert(accountId);
                    options.push_back({.accountId = cachedAccountId,
                                       .accountDisplayName = accountDisplayName,
                                       .identity = identity});
                }

                if (!hasSenderIdentity && !m_identityLoadsStarted.contains(accountId) &&
                    !connection.sessionUrl.isEmpty() && !connection.loginEmail.isEmpty() &&
                    connection.hasCredentials)
                {
                    m_identityLoadsStarted.insert(accountId);
                    auto task = m_composeCommandPort.loadSenderIdentities(
                        javelin::gui::settings::toAccountConnectionSettings(connection), accountId);
                    QCoro::connect(std::move(task), &m_context,
                                   [this](std::variant<std::vector<javelin::jmap::domain::Identity>,
                                                       javelin::jmap::OperationError>
                                              result)
                                   {
                                       if (const auto* error =
                                               std::get_if<javelin::jmap::OperationError>(&result))
                                       {
                                           if (m_statusMessage)
                                               m_statusMessage(error->message, 10000);
                                           return;
                                       }
                                       if (m_asynchronousReloadRequested)
                                           m_asynchronousReloadRequested();
                                   });
                }
            }
        }

        const bool includeAccountName = accountsWithIdentities.size() > 1;
        for (const auto& option : options)
        {
            const auto emailKey = QString::fromStdString(option.identity.email)
                                      .trimmed()
                                      .toCaseFolded()
                                      .toStdString();
            const int index = m_combo.count();
            m_combo.addItem(composeIdentityDisplayText(option.identity, option.accountDisplayName,
                                                       identityCountByEmail[emailKey],
                                                       includeAccountName));
            m_combo.setItemData(index, QString::fromStdString(option.identity.id), identityIdRole);
            m_combo.setItemData(index, option.accountId, accountIdRole);
            m_combo.setItemData(index, QString::fromStdString(option.identity.email), emailRole);
            m_combo.setItemData(index,
                                option.identity.textSignature.has_value()
                                    ? QString::fromStdString(*option.identity.textSignature)
                                    : QString{},
                                textSignatureRole);
            m_combo.setItemData(index,
                                option.identity.htmlSignature.has_value()
                                    ? QString::fromStdString(*option.identity.htmlSignature)
                                    : QString{},
                                htmlSignatureRole);
            QStringList bcc;
            for (const auto& address : option.identity.bcc)
            {
                if (address.name.has_value() && !address.name->empty())
                {
                    bcc.push_back(
                        QStringLiteral("%1 <%2>").arg(QString::fromStdString(*address.name),
                                                      QString::fromStdString(address.email)));
                }
                else
                    bcc.push_back(QString::fromStdString(address.email));
            }
            m_combo.setItemData(index, bcc.join(QStringLiteral(", ")), bccRole);
            if (option.accountId == selectedAccount &&
                QString::fromStdString(option.identity.id) == selectedIdentity)
            {
                selectedIndex = index;
            }
        }

        if (selectedIndex >= 0)
        {
            m_combo.setCurrentIndex(selectedIndex);
        }
        else if (!selectedIdentity.isEmpty())
        {
            m_combo.setPlaceholderText(i18n("Sender identity unavailable — choose another"));
            m_combo.setCurrentIndex(-1);
            if (m_statusMessage)
            {
                m_statusMessage(i18n("The draft's sender identity is no longer available. Choose "
                                     "another sender before sending."),
                                10000);
            }
        }
        else if (!options.empty())
        {
            m_combo.setCurrentIndex(0);
        }
        else if (m_statusMessage)
        {
            m_statusMessage(i18n("No sender identities are available for configured accounts."),
                            10000);
        }
    }
} // namespace javelin::gui::compose
