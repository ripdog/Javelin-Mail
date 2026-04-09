#pragma once

#include "jmap/cache/Database.h"

#include <optional>
#include <string>

namespace javelin::jmap::cache
{

    struct SubmissionRecord
    {
        std::string accountId;
        std::string submissionId;
        std::string emailId;
        std::optional<std::string> threadId;
        std::optional<QString> undoStatus;
        std::optional<QString> deliveryStatusJson;
    };

    class SubmissionRepository
    {
      public:
        explicit SubmissionRepository(DatabaseConnection& connection);

        [[nodiscard]] std::optional<DatabaseError> upsert(const SubmissionRecord& record);

      private:
        DatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::cache
