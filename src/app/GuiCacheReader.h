#pragma once

#include <QString>

#include <cstdint>
#include <variant>
#include <vector>

namespace javelin::app
{

    class GuiCacheReader final
    {
      public:
        struct AccountSummary
        {
            QString accountId;
            QString address;
        };

        struct MailboxSummary
        {
            QString accountId;
            QString mailboxId;
            QString name;
            QString role;
            std::uint64_t totalEmails = 0;
            std::uint64_t unreadEmails = 0;
        };

        struct MessageSummary
        {
            QString accountId;
            QString emailId;
            QString threadId;
            QString mailboxId;
            QString subject;
            QString preview;
            QString sender;
            QString receivedAt;
            bool unread = false;
            bool flagged = false;
            bool hasAttachment = false;
        };

        struct MessageDetail
        {
            QString subject;
            QString sender;
            QString receivedAt;
            QString preview;
            QString body;
            bool bodyIsHtml = false;
        };

        struct CacheSnapshot
        {
            std::vector<AccountSummary> accounts;
            std::vector<MailboxSummary> mailboxes;
        };

        using SnapshotResult = std::variant<CacheSnapshot, QString>;
        using MessageListResult = std::variant<std::vector<MessageSummary>, QString>;
        using MessageDetailResult = std::variant<MessageDetail, QString>;

        explicit GuiCacheReader(QString databasePath);

        [[nodiscard]] SnapshotResult loadSnapshot() const;
        [[nodiscard]] MessageListResult loadMessages(const QString& accountId,
                                                     const QString& mailboxId,
                                                     std::size_t limit = 100) const;
        [[nodiscard]] MessageDetailResult loadMessage(const QString& accountId,
                                                      const QString& emailId) const;

      private:
        QString m_databasePath;
    };

} // namespace javelin::app
