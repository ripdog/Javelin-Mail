#pragma once

#include "app/MailApplicationPorts.h"
#include "app/MessageSelection.h"
#include "gui/shell/MessageTransferDestinationPresentation.h"
#include "jmap/OperationError.h"

#include <QModelIndex>
#include <QObject>
#include <QPointer>
#include <QString>

#include <cstddef>
#include <optional>
#include <string>

class QListView;
class QMenu;
class QWidget;

namespace javelin::jmap::cache
{
    class AccountReader;
    class MailboxReader;
} // namespace javelin::jmap::cache

namespace javelin::gui::shell
{

    enum class MessageTransferOperation
    {
        Move,
        Copy,
    };

    struct EmailMutationSubmissionSummary
    {
        std::string accountId;
        std::size_t updatedEmailCount = 0;
        std::size_t failedEmailCount = 0;
    };

    class MessageCommandController final : public QObject
    {
        Q_OBJECT

      public:
        MessageCommandController(javelin::app::MailCommandPort& mailCommandPort,
                                 javelin::jmap::cache::AccountReader& accountReader,
                                 javelin::jmap::cache::MailboxReader& mailboxReader,
                                 MessageTransferAccountDisplayName accountDisplayName,
                                 QListView& messageView, QWidget* dialogParent,
                                 QObject* parent = nullptr);

        [[nodiscard]] javelin::app::MessageSelection
        selectedActionItems(bool excludeUnread = false) const;

        void archiveSelection(std::optional<std::string> accountId,
                              std::optional<std::string> sourceMailboxId, bool searchTab);
        void deleteSelection(std::optional<std::string> accountId,
                             std::optional<std::string> sourceMailboxId);
        void permanentlyDeleteSelection(std::optional<std::string> accountId,
                                        std::optional<std::string> sourceMailboxId);
        void showTransferMenu(MessageTransferOperation operation,
                              std::optional<std::string> accountId,
                              std::optional<std::string> sourceMailboxId, bool searchTab);
        [[nodiscard]] bool populateDestinationMenus(QMenu* moveMenu, QMenu* copyMenu,
                                                    std::string accountId,
                                                    std::optional<std::string> sourceMailboxId,
                                                    javelin::app::MessageSelection selection);
        void queueTransfer(std::string sourceAccountId, std::optional<std::string> sourceMailboxId,
                           std::string destinationAccountId, std::string destinationMailboxId,
                           javelin::app::MessageSelection selection,
                           MessageTransferOperation operation, QString successMessage);
        void markEmailRead(std::string accountId, std::string emailId);
        void toggleFlagged(std::optional<std::string> accountId, const QModelIndex& index);
        void setSelectionFlagged(std::optional<std::string> accountId,
                                 std::optional<std::string> sourceMailboxId, bool flagged);
        void markSelectionUnread(std::optional<std::string> accountId,
                                 std::optional<std::string> sourceMailboxId);
        void setSelectionJunk(std::optional<std::string> accountId,
                              std::optional<std::string> sourceMailboxId, bool junk);
        void setEmailJunk(std::string accountId, std::optional<std::string> sourceMailboxId,
                          std::string emailId, bool junk);
        void setSelectionTag(std::optional<std::string> accountId,
                             std::optional<std::string> sourceMailboxId, std::string keyword,
                             bool enabled);

      Q_SIGNALS:
        void statusMessage(QString message, int durationMilliseconds);
        void operationFailed(javelin::jmap::OperationError error);
        void mailboxMembershipChanged(QString accountId);
        void messageMetadataChanged(QString accountId);
        void junkStateChanged(QString accountId);
        void emailMarkedRead(QString accountId, QString emailId);
        void emailMutationsSubmitted(const EmailMutationSubmissionSummary& summary);

      private:
        void queueArchive(std::string accountId, std::optional<std::string> sourceMailboxId,
                          javelin::app::MessageSelection selection);
        void queueDelete(std::string accountId, std::optional<std::string> sourceMailboxId,
                         javelin::app::MessageSelection selection);
        void queueDestroy(std::string accountId, std::optional<std::string> sourceMailboxId,
                          javelin::app::MessageSelection selection);
        void queueJunk(std::string accountId, std::optional<std::string> sourceMailboxId,
                       javelin::app::MessageSelection selection, bool junk);
        void submitQueuedMutations(std::string accountId,
                                   std::optional<std::string> operationGroupId);
        [[nodiscard]] bool
        confirmPermanentDelete(const javelin::app::MessageSelection& selection) const;

        javelin::app::MailCommandPort& m_mailCommandPort;
        javelin::jmap::cache::AccountReader& m_accountReader;
        javelin::jmap::cache::MailboxReader& m_mailboxReader;
        MessageTransferAccountDisplayName m_accountDisplayName;
        QListView& m_messageView;
        QPointer<QWidget> m_dialogParent;
    };

} // namespace javelin::gui::shell
