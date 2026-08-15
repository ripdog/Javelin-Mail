#pragma once

#include "jmap/OperationError.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <QString>

#include <optional>
#include <string>
#include <variant>

namespace javelin::app::undo
{
    class UndoManager;

    using MailTransferHistoryFinalizationResult =
        std::variant<std::optional<QString>, javelin::jmap::OperationError>;

    class MailTransferHistoryCoordinator
    {
      public:
        MailTransferHistoryCoordinator(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                                       UndoManager& undoManager);

        [[nodiscard]] MailTransferHistoryFinalizationResult
        finalizeCompleted(std::string operationId);

      private:
        javelin::jmap::cache::DatabaseConnection& m_databaseConnection;
        UndoManager& m_undoManager;
    };

} // namespace javelin::app::undo
