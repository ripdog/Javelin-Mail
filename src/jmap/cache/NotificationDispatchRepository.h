#pragma once

#include "jmap/cache/Database.h"

#include <string_view>
#include <variant>

namespace javelin::jmap::cache
{

    enum class NotificationDispatchKind
    {
        Mail,
        Calendar,
    };

    class NotificationDispatchRepository
    {
      public:
        explicit NotificationDispatchRepository(DatabaseConnection& connection);

        [[nodiscard]] std::variant<bool, DatabaseError> claim(DatabaseTransaction& transaction,
                                                              NotificationDispatchKind kind,
                                                              std::string_view claimKey);
        [[nodiscard]] std::optional<DatabaseError> release(DatabaseTransaction& transaction,
                                                           NotificationDispatchKind kind,
                                                           std::string_view claimKey);
        [[nodiscard]] std::optional<DatabaseError> recover(NotificationDispatchKind kind);

      private:
        DatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::cache
