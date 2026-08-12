#pragma once

#include "jmap/cache/MessageViewReader.h"
#include "storage/sqlite/DatabaseConnection.h"

namespace javelin::jmap::cache
{

    class MessageViewService final : public MessageViewReader
    {
      public:
        explicit MessageViewService(DatabaseConnection& connection);
        explicit MessageViewService(ReadOnlyDatabaseConnection& connection);

        [[nodiscard]] MessageViewResult load(std::string_view accountId,
                                             std::string_view emailId) const override;
        [[nodiscard]] QFuture<MessageViewResult> loadAsync(std::string accountId,
                                                           std::string emailId) const override;

      private:
        DatabaseReadView m_connection;
    };

} // namespace javelin::jmap::cache
