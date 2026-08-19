#pragma once

#include "app/WorkTaskPort.h"
#include "jmap/OperationError.h"
#include "jmap/cache/ThreadReader.h"
#include "jmap/cache/ThreadRepository.h"

#include <QCoroTask>

#include <QString>

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{
    class DatabaseConnection;
}

namespace javelin::app
{
    class ThreadMaterializationCoordinator;

    struct SelectedEmail
    {
        std::string emailId;
    };

    struct SelectedCollapsedThread
    {
        std::string threadId;
    };

    using MessageSelectionItem = std::variant<SelectedEmail, SelectedCollapsedThread>;
    using MessageSelection = std::vector<MessageSelectionItem>;
    using ResolvedMessageSelection = std::variant<std::vector<std::string>, QString>;

    [[nodiscard]] QCoro::Task<std::optional<javelin::jmap::OperationError>>
    ensureMessageSelectionMaterialized(
        javelin::jmap::cache::DatabaseConnection& databaseConnection,
        ThreadMaterializationCoordinator* threadMaterializationCoordinator, std::string accountId,
        std::optional<std::string> sourceMailboxId, MessageSelection selection,
        WorkPriority priority = WorkPriority::Interactive);

    [[nodiscard]] ResolvedMessageSelection
    resolveMessageSelection(const javelin::jmap::cache::ThreadReader& threadReader,
                            const javelin::jmap::cache::ThreadRepository& threadRepository,
                            std::string_view accountId, std::optional<std::string_view> mailboxId,
                            const MessageSelection& selection);

} // namespace javelin::app
