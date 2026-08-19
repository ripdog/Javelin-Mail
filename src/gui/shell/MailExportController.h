#pragma once

#include "jmap/OperationError.h"

#include <QObject>

#include <string>

class QWidget;

namespace javelin::app
{
    class MailExportPort;
}
namespace javelin::jmap::cache
{
    class AccountReader;
    class MailboxReader;
} // namespace javelin::jmap::cache
namespace javelin::gui::settings
{
    class GuiSettings;
}

namespace javelin::gui::shell
{
    class MailExportController final : public QObject
    {
        Q_OBJECT

      public:
        MailExportController(javelin::app::MailExportPort& exportPort,
                             javelin::jmap::cache::AccountReader& accountReader,
                             javelin::jmap::cache::MailboxReader& mailboxReader,
                             javelin::gui::settings::GuiSettings& settings, QWidget& dialogParent,
                             QObject* parent = nullptr);

        void exportMailbox(std::string accountId, std::string mailboxId);
        void exportAccount(std::string accountId);

      Q_SIGNALS:
        void statusMessage(QString message, int durationMilliseconds);
        void operationFailed(javelin::jmap::OperationError error);

      private:
        void startExport(std::string accountId, std::string mailboxId, QString displayName,
                         bool accountScope);

        javelin::app::MailExportPort& m_exportPort;
        javelin::jmap::cache::AccountReader& m_accountReader;
        javelin::jmap::cache::MailboxReader& m_mailboxReader;
        javelin::gui::settings::GuiSettings& m_settings;
        QWidget& m_dialogParent;
    };
} // namespace javelin::gui::shell
