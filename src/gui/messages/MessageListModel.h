#pragma once

#include "jmap/cache/QueryService.h"

#include <QAbstractListModel>

#include <optional>
#include <string>
#include <vector>

namespace javelin::gui::messages
{

    class MessageListModel : public QAbstractListModel
    {
        Q_OBJECT

      public:
        enum Roles
        {
            EmailIdRole = Qt::UserRole + 1,
            ThreadIdRole,
            SenderDisplayRole,
            SubjectRole,
            PreviewRole,
            ReceivedAtRole,
            HasAttachmentRole,
            IsUnreadRole,
            IsFlaggedRole,
            ThreadMessageCountRole,
        };

        explicit MessageListModel(javelin::jmap::cache::QueryService& queryService,
                                  QObject* parent = nullptr);
        ~MessageListModel() override;

        [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex{}) const override;
        [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;

        void setMailboxContext(std::optional<std::string> accountId,
                               std::optional<std::string> mailboxId);
        void refresh();

      private:
        void reload();

        javelin::jmap::cache::QueryService& m_queryService;
        std::optional<std::string> m_accountId;
        std::optional<std::string> m_mailboxId;
        std::vector<javelin::jmap::cache::MessageListItem> m_items;
    };

} // namespace javelin::gui::messages
