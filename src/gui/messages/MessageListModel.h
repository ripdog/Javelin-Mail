#pragma once

#include "jmap/cache/QueryService.h"

#include <QAbstractListModel>

#include <cstddef>
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
            ThreadMessageCountRole,
            RowKindRole,
            IsExpandedRole,
            CanExpandRole,
        };

        explicit MessageListModel(javelin::jmap::cache::QueryService& queryService,
                                  QObject* parent = nullptr);
        ~MessageListModel() override;

        [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex{}) const override;
        [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
        [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
        [[nodiscard]] QStringList mimeTypes() const override;
        [[nodiscard]] QMimeData* mimeData(const QModelIndexList& indexes) const override;
        [[nodiscard]] Qt::DropActions supportedDragActions() const override;

        void setPage(std::optional<std::string> accountId,
                     std::vector<javelin::jmap::cache::MessageListItem> items);
        void clear();
        [[nodiscard]] bool setThreadExpanded(std::string_view threadId, bool expanded);
        [[nodiscard]] bool toggleThreadExpanded(std::string_view threadId);
        [[nodiscard]] bool isThreadExpanded(std::string_view threadId) const;
        [[nodiscard]] std::optional<std::string>
        summaryEmailIdForThread(std::string_view threadId) const;

      private:
        struct ThreadEntry
        {
            javelin::jmap::cache::MessageListItem summary;
            std::vector<javelin::jmap::cache::MessageListItem> members;
            bool membersLoaded = false;
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
        [[nodiscard]] bool loadThreadMembers(std::size_t threadIndex);
        [[nodiscard]] int visibleBlockStartForThread(std::size_t threadIndex) const;
        [[nodiscard]] int visibleBlockSizeForThread(std::size_t threadIndex) const;
        void reindexVisibleRows();
        void rebuildVisibleRows();

        javelin::jmap::cache::QueryService& m_queryService;
        std::optional<std::string> m_accountId;
        std::vector<ThreadEntry> m_threads;
        std::vector<VisibleRow> m_rows;
        std::vector<std::string> m_expandedThreadIds;
    };

} // namespace javelin::gui::messages
