#pragma once

#include "jmap/OperationError.h"

#include <QObject>
#include <QStringList>

#include <optional>
#include <string>

class QWidget;

namespace javelin::app
{
    class MailImportPort;
}
namespace javelin::jmap::cache
{
    class AccountReader;
    class MailboxReader;
} // namespace javelin::jmap::cache

namespace javelin::gui::shell
{
    class MailImportController final : public QObject
    {
        Q_OBJECT

      public:
        MailImportController(javelin::app::MailImportPort& importPort,
                             javelin::jmap::cache::AccountReader& accountReader,
                             javelin::jmap::cache::MailboxReader& mailboxReader,
                             QWidget& dialogParent, QObject* parent = nullptr);

        void importMessages(std::optional<std::string> accountId = std::nullopt,
                            std::optional<std::string> mailboxId = std::nullopt);
        void importFolderTree(std::optional<std::string> accountId = std::nullopt,
                              std::optional<std::string> parentMailboxId = std::nullopt);
        void importDroppedPaths(QStringList paths, std::string accountId, std::string mailboxId);

      Q_SIGNALS:
        void statusMessage(QString message, int durationMilliseconds);
        void operationFailed(javelin::jmap::OperationError error);

      private:
        void promptAndStart(QStringList sourcePaths, std::optional<std::string> accountId,
                            std::optional<std::string> mailboxId, bool hierarchySource,
                            bool forceHierarchyChoice);

        javelin::app::MailImportPort& m_importPort;
        javelin::jmap::cache::AccountReader& m_accountReader;
        javelin::jmap::cache::MailboxReader& m_mailboxReader;
        QWidget& m_dialogParent;
    };
} // namespace javelin::gui::shell
