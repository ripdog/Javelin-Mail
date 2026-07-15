#include "app/ApplicationErrorCoordinator.h"

#include <QSettings>

#include <algorithm>

namespace javelin::app
{
    namespace
    {
        constexpr auto errorStateGroup = "errorHandling";
        constexpr auto authenticationPauseGroup = "authenticationPauses";

        [[nodiscard]] QString connectionKey(const std::string_view connectionId)
        {
            return QString::fromStdString(std::string{connectionId});
        }
    } // namespace

    ApplicationErrorCoordinator::ApplicationErrorCoordinator(QObject* parent) : QObject(parent)
    {
    }

    void ApplicationErrorCoordinator::reportFailure(const AccountConnectionSettings& settings,
                                                    const std::string_view accountId,
                                                    QString operation,
                                                    const javelin::jmap::OperationError& error)
    {
        if (javelin::jmap::isCancellation(error) || settings.connectionId.empty())
            return;

        const auto key = incidentKey(settings.connectionId, error.code);
        if (!m_activeIncidents.insert(key.toStdString()).second)
            return;

        const bool authentication = javelin::jmap::isAuthenticationError(error);
        if (authentication)
        {
            persistAuthenticationPause(settings.connectionId, settings.revision);
            Q_EMIT authenticationPauseChanged(connectionKey(settings.connectionId), true);
        }

        const auto account = QString::fromStdString(std::string{accountId});
        Q_EMIT incidentRaised(connectionKey(settings.connectionId), account, userTitle(error),
                              userMessage(account, operation, error), authentication,
                              authentication);
    }

    void ApplicationErrorCoordinator::reportSuccess(const std::string_view connectionId)
    {
        const auto prefix = connectionKey(connectionId) + QLatin1Char(':');
        const auto authenticationKey =
            incidentKey(connectionId, javelin::jmap::OperationErrorCode::AuthenticationRequired)
                .toStdString();
        const bool authenticationBlocked = blockedRevision(connectionId).has_value();
        std::erase_if(m_activeIncidents,
                      [&prefix, &authenticationKey, authenticationBlocked](const std::string& key)
                      {
                          return QString::fromStdString(key).startsWith(prefix) &&
                                 (!authenticationBlocked || key != authenticationKey);
                      });
    }

    void ApplicationErrorCoordinator::settingsApplied(const std::string_view connectionId,
                                                      const std::uint64_t revision)
    {
        const auto blocked = blockedRevision(connectionId);
        if (!blocked.has_value() || revision <= *blocked)
            return;
        clearAuthenticationPause(connectionId);
        reportSuccess(connectionId);
        Q_EMIT authenticationPauseChanged(connectionKey(connectionId), false);
    }

    void ApplicationErrorCoordinator::forgetConnection(const std::string_view connectionId)
    {
        clearAuthenticationPause(connectionId);
        reportSuccess(connectionId);
    }

    bool ApplicationErrorCoordinator::authenticationPaused(const std::string_view connectionId,
                                                           const std::uint64_t revision) const
    {
        const auto blocked = blockedRevision(connectionId);
        return blocked.has_value() && revision <= *blocked;
    }

    QString ApplicationErrorCoordinator::incidentKey(const std::string_view connectionId,
                                                     const javelin::jmap::OperationErrorCode code)
    {
        return QStringLiteral("%1:%2").arg(connectionKey(connectionId),
                                           QString::fromUtf8(javelin::jmap::toString(code).data()));
    }

    QString ApplicationErrorCoordinator::userTitle(const javelin::jmap::OperationError& error)
    {
        if (javelin::jmap::isAuthenticationError(error))
            return QStringLiteral("Account sign-in required");
        if (javelin::jmap::isTransientError(error))
            return QStringLiteral("Connection problem");
        if (error.code == javelin::jmap::OperationErrorCode::PermissionDenied)
            return QStringLiteral("Permission denied");
        if (error.code == javelin::jmap::OperationErrorCode::LocalStorageFailure)
            return QStringLiteral("Local storage error");
        return QStringLiteral("Javelin Mail error");
    }

    QString ApplicationErrorCoordinator::userMessage(const QString& accountId,
                                                     const QString& operation,
                                                     const javelin::jmap::OperationError& error)
    {
        const auto account = accountId.isEmpty() ? QStringLiteral("this account") : accountId;
        if (javelin::jmap::isAuthenticationError(error))
            return QStringLiteral("Authentication failed for %1. Update and save its connection "
                                  "settings to resume synchronization.")
                .arg(account);
        if (javelin::jmap::isTransientError(error))
            return QStringLiteral("%1 could not complete for %2. Javelin Mail will keep retrying.")
                .arg(operation, account);
        if (error.code == javelin::jmap::OperationErrorCode::PermissionDenied)
            return QStringLiteral("The server denied %1 for %2.").arg(operation, account);
        if (error.code == javelin::jmap::OperationErrorCode::Conflict)
            return QStringLiteral("%1 conflicted with a newer server change. Refresh and try "
                                  "again.")
                .arg(operation);
        if (error.code == javelin::jmap::OperationErrorCode::UnsupportedCapability)
            return QStringLiteral("The server does not support %1 for %2.").arg(operation, account);
        if (error.code == javelin::jmap::OperationErrorCode::SchedulingUnsupported)
            return QStringLiteral("The server cannot schedule this calendar event.");
        if (error.code == javelin::jmap::OperationErrorCode::InvalidUserInput ||
            error.code == javelin::jmap::OperationErrorCode::PreconditionFailed)
            return error.message;
        return QStringLiteral("%1 failed for %2. See the application log for details.")
            .arg(operation, account);
    }

    void
    ApplicationErrorCoordinator::persistAuthenticationPause(const std::string_view connectionId,
                                                            const std::uint64_t revision)
    {
        QSettings settings;
        settings.beginGroup(QLatin1StringView{errorStateGroup});
        settings.beginGroup(QLatin1StringView{authenticationPauseGroup});
        settings.setValue(connectionKey(connectionId), static_cast<qulonglong>(revision));
        settings.endGroup();
        settings.endGroup();
        settings.sync();
    }

    void ApplicationErrorCoordinator::clearAuthenticationPause(const std::string_view connectionId)
    {
        QSettings settings;
        settings.beginGroup(QLatin1StringView{errorStateGroup});
        settings.beginGroup(QLatin1StringView{authenticationPauseGroup});
        settings.remove(connectionKey(connectionId));
        settings.endGroup();
        settings.endGroup();
        settings.sync();
    }

    std::optional<std::uint64_t>
    ApplicationErrorCoordinator::blockedRevision(const std::string_view connectionId) const
    {
        QSettings settings;
        settings.beginGroup(QLatin1StringView{errorStateGroup});
        settings.beginGroup(QLatin1StringView{authenticationPauseGroup});
        const auto value = settings.value(connectionKey(connectionId));
        settings.endGroup();
        settings.endGroup();
        if (!value.isValid())
            return std::nullopt;
        return value.toULongLong();
    }

} // namespace javelin::app
