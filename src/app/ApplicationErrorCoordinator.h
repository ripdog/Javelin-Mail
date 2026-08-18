#pragma once

#include "app/AccountConnectionSettings.h"
#include "jmap/OperationError.h"

#include <QObject>
#include <QString>

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace javelin::jmap::cache
{
    class AccountReader;
}

namespace javelin::app
{

    class ApplicationErrorCoordinator final : public QObject
    {
        Q_OBJECT

      public:
        explicit ApplicationErrorCoordinator(javelin::jmap::cache::AccountReader& accountReader,
                                             QObject* parent = nullptr);

        void reportFailure(const AccountConnectionSettings& settings, std::string_view accountId,
                           QString operation, const javelin::jmap::OperationError& error);
        void reportSuccess(std::string_view connectionId);
        void settingsApplied(std::string_view connectionId, std::uint64_t revision);
        void forgetConnection(std::string_view connectionId);

        [[nodiscard]] bool authenticationPaused(std::string_view connectionId,
                                                std::uint64_t revision) const;

      Q_SIGNALS:
        void incidentRaised(const QString& connectionId, const QString& accountId,
                            const QString& title, const QString& message, bool persistent,
                            bool opensSettings);
        void authenticationPauseChanged(const QString& connectionId, bool paused);

      private:
        [[nodiscard]] static QString incidentKey(std::string_view connectionId,
                                                 javelin::jmap::OperationErrorCode code);
        [[nodiscard]] static QString serviceIncidentKey(const AccountConnectionSettings& settings);
        [[nodiscard]] static QString serviceName(const AccountConnectionSettings& settings);
        [[nodiscard]] static bool isServiceOutage(const javelin::jmap::OperationError& error);
        [[nodiscard]] static QString userTitle(const javelin::jmap::OperationError& error);
        [[nodiscard]] QString accountName(const AccountConnectionSettings& settings,
                                          std::string_view accountId) const;
        [[nodiscard]] static QString userMessage(const QString& accountName,
                                                 const QString& operation,
                                                 const javelin::jmap::OperationError& error);
        void persistAuthenticationPause(std::string_view connectionId, std::uint64_t revision);
        void clearAuthenticationPause(std::string_view connectionId);
        [[nodiscard]] std::optional<std::uint64_t>
        blockedRevision(std::string_view connectionId) const;

        javelin::jmap::cache::AccountReader& m_accountReader;
        std::unordered_set<std::string> m_activeIncidents;
        std::unordered_map<std::string, std::string> m_connectionServices;
    };

} // namespace javelin::app
