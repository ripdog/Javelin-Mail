#include "gui/settings/GuiSettings.h"

#include <algorithm>
#include <ranges>
#include <utility>

namespace javelin::gui::settings
{
    GuiSettings::GuiSettings(javelin::app::SettingsPort& port) : m_port(&port)
    {
    }

    GuiSettings::GuiSettings(javelin::protocol::SettingsSnapshot snapshot)
        : m_localSnapshot(std::move(snapshot))
    {
    }

    const javelin::protocol::SettingsSnapshot& GuiSettings::snapshot() const
    {
        return m_port != nullptr ? m_port->settings() : *m_localSnapshot;
    }

    std::vector<ConnectionSettings> GuiSettings::accounts() const
    {
        std::vector<ConnectionSettings> result;
        result.reserve(snapshot().accounts.size());
        for (const auto& account : snapshot().accounts)
        {
            const auto loginEmail = account.loginEmail.trimmed();
            auto displayName = account.displayName.trimmed();
            if (displayName.isEmpty())
                displayName = loginEmail;
            result.push_back({
                .id = account.id,
                .revision = account.revision,
                .displayName = displayName,
                .sessionUrl = account.sessionUrl.trimmed(),
                .loginEmail = loginEmail,
                .apiKey = account.apiKey.trimmed(),
                .cachedAccountIds = stringList(account.cachedAccountIds),
            });
        }
        return result;
    }

    ConnectionSettings GuiSettings::accountForCachedId(const QStringView accountId) const
    {
        const auto values = accounts();
        const auto found = std::ranges::find_if(
            values, [accountId](const auto& account)
            { return account.cachedAccountIds.contains(accountId.toString()); });
        return found == values.end() ? ConnectionSettings{} : *found;
    }

    QStringList GuiSettings::syncedMailboxIds(const QStringView accountId) const
    {
        return mailboxIds(snapshot().syncedMailboxSelections, accountId);
    }

    QStringList GuiSettings::notificationMailboxIds(const QStringView accountId) const
    {
        return mailboxIds(snapshot().notificationMailboxSelections, accountId);
    }

    bool GuiSettings::hasNotificationMailboxSelection(const QStringView accountId) const
    {
        const auto found = std::ranges::find(snapshot().notificationMailboxSelections,
                                             accountId.toString(),
                                             &javelin::protocol::MailboxSelectionSettings::accountId);
        return found != snapshot().notificationMailboxSelections.end() && found->configured;
    }

    QStringList GuiSettings::remoteContentSenders() const
    {
        return stringList(snapshot().remoteContentSenders);
    }

    QStringList GuiSettings::remoteContentDomains() const
    {
        return stringList(snapshot().remoteContentDomains);
    }

    javelin::app::TranslationSettings GuiSettings::translationSettings() const
    {
        const auto& value = snapshot().translation;
        return {
            .enabled = value.enabled,
            .apiKeyOverride = value.apiKeyOverride,
            .targetLanguage = value.targetLanguage,
            .autoTranslateSenders = stringList(value.autoTranslateSenders),
            .autoTranslateDomains = stringList(value.autoTranslateDomains),
        };
    }

    javelin::gui::messageview::MessageAppearanceSettings
    GuiSettings::messageAppearanceSettings() const
    {
        return {.colorMode = javelin::gui::messageview::messageColorModeFromStorage(
                    snapshot().appearance.messageColorMode)};
    }

    AttachmentSaveSettings GuiSettings::attachmentSaveSettings() const
    {
        return {.alwaysAsk = snapshot().attachments.alwaysAsk,
                .directory = snapshot().attachments.directory};
    }

    int GuiSettings::undoSendDelaySeconds() const
    {
        return snapshot().undoSendDelaySeconds;
    }

    std::optional<javelin::protocol::BoundaryError>
    GuiSettings::update(javelin::protocol::SettingsUpdate update)
    {
        return this->update(snapshot().revision, std::move(update));
    }

    std::optional<javelin::protocol::BoundaryError>
    GuiSettings::update(const javelin::protocol::SettingsRevision baseRevision,
                        javelin::protocol::SettingsUpdate update)
    {
        if (m_port != nullptr)
            return m_port->updateSettings(baseRevision, std::move(update));

        auto& value = *m_localSnapshot;
        if (baseRevision != value.revision)
        {
            return javelin::protocol::BoundaryError{
                .code = javelin::protocol::BoundaryErrorCode::StaleSettingsRevision,
                .field = QStringLiteral("settings.revision"),
                .detail = QStringLiteral("The settings snapshot is stale."),
            };
        }
        if (update.accounts.has_value())
            value.accounts = std::move(*update.accounts);
        if (update.syncedMailboxSelections.has_value())
            value.syncedMailboxSelections = std::move(*update.syncedMailboxSelections);
        if (update.notificationMailboxSelections.has_value())
            value.notificationMailboxSelections = std::move(*update.notificationMailboxSelections);
        if (update.remoteContentSenders.has_value())
            value.remoteContentSenders = std::move(*update.remoteContentSenders);
        if (update.remoteContentDomains.has_value())
            value.remoteContentDomains = std::move(*update.remoteContentDomains);
        if (update.translation.has_value())
            value.translation = std::move(*update.translation);
        if (update.appearance.has_value())
            value.appearance = std::move(*update.appearance);
        if (update.attachments.has_value())
            value.attachments = std::move(*update.attachments);
        if (update.undoSendDelaySeconds.has_value())
            value.undoSendDelaySeconds = *update.undoSendDelaySeconds;
        ++value.revision.value;
        return std::nullopt;
    }

    std::optional<javelin::protocol::BoundaryError>
    GuiSettings::associateCachedAccount(const QString& configuredAccountId,
                                        const QString& cachedAccountId)
    {
        auto values = snapshot().accounts;
        const auto found = std::ranges::find(values, configuredAccountId,
                                             &javelin::protocol::AccountSettings::id);
        if (found == values.end() ||
            std::ranges::contains(found->cachedAccountIds, cachedAccountId))
            return std::nullopt;
        found->cachedAccountIds.push_back(cachedAccountId);
        javelin::protocol::SettingsUpdate update;
        update.accounts = std::move(values);
        return this->update(std::move(update));
    }

    std::optional<javelin::protocol::BoundaryError>
    GuiSettings::saveResolvedSessionUrl(const QString& configuredAccountId, const QString& sessionUrl)
    {
        if (sessionUrl.isEmpty())
            return std::nullopt;
        auto values = snapshot().accounts;
        const auto found = std::ranges::find(values, configuredAccountId,
                                             &javelin::protocol::AccountSettings::id);
        if (found == values.end() || found->sessionUrl == sessionUrl)
            return std::nullopt;
        found->sessionUrl = sessionUrl;
        javelin::protocol::SettingsUpdate update;
        update.accounts = std::move(values);
        return this->update(std::move(update));
    }

    QMetaObject::Connection GuiSettings::connectChanged(QObject* context,
                                                        std::function<void()> callback)
    {
        return m_port == nullptr ? QMetaObject::Connection{}
                                 : m_port->connectSettingsChanged(context, std::move(callback));
    }

    std::vector<javelin::protocol::AccountSettings>
    GuiSettings::protocolAccounts(const std::vector<ConnectionSettings>& accounts)
    {
        std::vector<javelin::protocol::AccountSettings> result;
        result.reserve(accounts.size());
        for (const auto& account : accounts)
        {
            result.push_back({
                .id = account.id,
                .revision = account.revision,
                .displayName = account.displayName,
                .sessionUrl = account.sessionUrl,
                .loginEmail = account.loginEmail,
                .apiKey = account.apiKey,
                .cachedAccountIds = {account.cachedAccountIds.begin(), account.cachedAccountIds.end()},
            });
        }
        return result;
    }

    QStringList GuiSettings::stringList(const std::vector<QString>& values)
    {
        return {values.begin(), values.end()};
    }

    QStringList GuiSettings::mailboxIds(
        const std::vector<javelin::protocol::MailboxSelectionSettings>& selections,
        const QStringView accountId)
    {
        const auto found = std::ranges::find(selections, accountId.toString(),
                                             &javelin::protocol::MailboxSelectionSettings::accountId);
        return found == selections.end() ? QStringList{} : stringList(found->mailboxIds);
    }
} // namespace javelin::gui::settings
