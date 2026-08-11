#pragma once

#include "jmap/cache/QueryReader.h"

#include <QAbstractListModel>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace javelin::gui::messages
{

    class MessageListModel : public QAbstractListModel
    {
        Q_OBJECT

      public:
        enum class RowKind
        {
            ThreadSummary = 0,
            ThreadMember = 1,
        };

        enum Roles
        {
            EmailIdRole = Qt::UserRole + 1,
            ThreadIdRole,
            SenderDisplayRole,
            SenderEmailRole,
            SenderNameRole,
            SubjectRole,
            PreviewRole,
            ReceivedAtRole,
            HasAttachmentRole,
            IsUnreadRole,
            IsFlaggedRole,
            IsJunkRole,
            ThreadMessageCountRole,
            GlobalThreadMessageCountRole,
            RowKindRole,
            IsExpandedRole,
            CanExpandRole,
            MailboxNamesRole,
            TagNamesRole,
            TagColorsRole,
            IsSearchResultRole,
        };

        explicit MessageListModel(javelin::jmap::cache::QueryReader& queryReader,
                                  QObject* parent = nullptr);
        ~MessageListModel() override;

        [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex{}) const override;
        [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
        [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
        [[nodiscard]] QStringList mimeTypes() const override;
        [[nodiscard]] QMimeData* mimeData(const QModelIndexList& indexes) const override;
        [[nodiscard]] Qt::DropActions supportedDragActions() const override;

        void setItems(std::optional<std::string> accountId, std::optional<std::string> mailboxId,
                      const std::vector<javelin::jmap::cache::MessageListItem>& items);
        void clear();
        [[nodiscard]] bool setThreadExpanded(std::string_view threadId, bool expanded);
        [[nodiscard]] bool toggleThreadExpanded(std::string_view threadId);
        [[nodiscard]] bool isThreadExpanded(std::string_view threadId) const;
        [[nodiscard]] std::optional<std::string>
        summaryEmailIdForThread(std::string_view threadId) const;
        [[nodiscard]] bool setEmailRead(std::string_view emailId);

      private:
        struct ThreadEntry
        {
            javelin::jmap::cache::MessageListItem summary;
            std::vector<javelin::jmap::cache::MessageListItem> members;
            bool membersLoaded = false;
            bool membersLoading = false;
        };

        struct VisibleRow
        {
            RowKind kind = RowKind::ThreadSummary;
            std::size_t threadIndex = 0;
            std::optional<std::size_t> memberIndex;
        };

        [[nodiscard]] const javelin::jmap::cache::MessageListItem&
        itemForRow(const VisibleRow& row) const;
        [[nodiscard]] std::optional<std::size_t> findThreadIndex(std::string_view threadId) const;
        [[nodiscard]] std::optional<int> visibleSummaryRowForThread(std::size_t threadIndex) const;
        void startThreadMembersLoad(std::size_t threadIndex);
        [[nodiscard]] int visibleBlockStartForThread(std::size_t threadIndex) const;
        [[nodiscard]] int visibleBlockSizeForThread(std::size_t threadIndex) const;
        void reindexVisibleRows();
        void rebuildVisibleRows();

        javelin::jmap::cache::QueryReader& m_queryReader;
        std::optional<std::string> m_accountId;
        std::optional<std::string> m_mailboxId;
        std::vector<ThreadEntry> m_threads;
        std::vector<VisibleRow> m_rows;
        std::vector<std::string> m_expandedThreadIds;
        std::uint64_t m_generation = 0;
    };

} // namespace javelin::gui::messages
