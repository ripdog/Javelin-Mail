#pragma once

#include "app/MessageSelection.h"
#include "jmap/OperationError.h"

#include <QModelIndex>
#include <QObject>
#include <QPointer>
#include <QString>

#include <optional>
#include <string>

class QListView;
class QMenu;
class QWidget;

namespace javelin::app
{
    class MailApplicationService;
}

namespace javelin::jmap::cache
{
    class QueryService;
}

namespace javelin::gui::shell
{

    enum class MessageTransferOperation
    {
        Move,
        Copy,
    };

    class MessageCommandController final : public QObject
    {
        Q_OBJECT

      public:
        MessageCommandController(javelin::app::MailApplicationService& mailService,
                                 javelin::jmap::cache::QueryService& queryService,
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
        void populateDestinationMenus(QMenu* moveMenu, QMenu* copyMenu, std::string accountId,
                                      std::optional<std::string> sourceMailboxId,
                                      javelin::app::MessageSelection selection);
        void queueTransfer(std::string accountId, std::optional<std::string> sourceMailboxId,
                           std::string destinationMailboxId,
                           javelin::app::MessageSelection selection,
                           MessageTransferOperation operation, QString successMessage);
        void markEmailRead(std::string accountId, std::string emailId);
        void toggleFlagged(std::optional<std::string> accountId, const QModelIndex& index);
        void markSelectionUnread(std::optional<std::string> accountId,
                                 std::optional<std::string> sourceMailboxId);

      Q_SIGNALS:
        void statusMessage(QString message, int durationMilliseconds);
        void operationFailed(javelin::jmap::OperationError error);
        void mailboxMembershipChanged(QString accountId);
        void messageMetadataChanged(QString accountId);
        void submitRequested(QString accountId);

      private:
        void queueArchive(std::string accountId, std::optional<std::string> sourceMailboxId,
                          javelin::app::MessageSelection selection);
        void queueDelete(std::string accountId, std::string sourceMailboxId,
                         javelin::app::MessageSelection selection);
        void queueDestroy(std::string accountId, std::optional<std::string> sourceMailboxId,
                          javelin::app::MessageSelection selection);
        [[nodiscard]] bool confirmPermanentDelete(std::size_t selectionItemCount) const;

        javelin::app::MailApplicationService& m_mailService;
        javelin::jmap::cache::QueryService& m_queryService;
        QListView& m_messageView;
        QPointer<QWidget> m_dialogParent;
    };

} // namespace javelin::gui::shell
