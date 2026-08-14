#pragma once

#include "app/AccountRefreshApplicationPorts.h"
#include "gui/settings/ConnectionSettings.h"
#include "jmap/AccountBootstrapClient.h"
#include "jmap/OperationError.h"
#include "jmap/contacts/ContactResults.h"

#include <QObject>
#include <QString>

#include <string>

namespace javelin::gui::settings
{
    class GuiSettings;
}

namespace javelin::app
{
    class AccountRefreshPort;
}

namespace javelin::jmap::cache
{
    class AccountReader;
}

namespace javelin::gui::shell
{
    class AccountRefreshController final : public QObject
    {
        Q_OBJECT

      public:
        AccountRefreshController(javelin::gui::settings::GuiSettings& settings,
                                 javelin::app::AccountRefreshPort& commandPort,
                                 javelin::jmap::cache::AccountReader& accountReader,
                                 QObject* parent = nullptr);

        void refreshAccount(std::string accountId);
        void refreshConnection(javelin::gui::settings::ConnectionSettings settings);

      Q_SIGNALS:
        void busyChanged(bool busy);
        void statusMessage(const QString& message, int durationMilliseconds);
        void userInterventionRequired(const QString& message);
        void operationFailed(const javelin::jmap::OperationError& error);
        void accountRefreshed(const javelin::jmap::LiveRefreshSummary& summary);
        void contactsRefreshed(const javelin::jmap::contacts::ContactRefreshSummary& summary);

      private:
        javelin::gui::settings::GuiSettings& m_settings;
        javelin::app::AccountRefreshPort& m_commandPort;
        javelin::jmap::cache::AccountReader& m_accountReader;
        bool m_refreshInFlight = false;
    };
} // namespace javelin::gui::shell
