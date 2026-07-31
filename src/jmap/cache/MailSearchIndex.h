#pragma once

#include "jmap/cache/Database.h"

#include <QString>

#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{
    struct SearchIndexDocument
    {
        std::string emailId;
        std::string sourceHash;
        QString subject;
        QString body;
    };

    class MailSearchIndex
    {
      public:
        explicit MailSearchIndex(const DatabaseConnection& cacheConnection);
        explicit MailSearchIndex(const ReadOnlyDatabaseConnection& cacheConnection);
        explicit MailSearchIndex(const DatabaseReadView& cacheConnection);

        [[nodiscard]] std::optional<DatabaseError>
        upsert(std::string_view accountId, const SearchIndexDocument& document) const;
        [[nodiscard]] std::optional<DatabaseError> remove(std::string_view accountId,
                                                          std::string_view emailId) const;
        [[nodiscard]] std::variant<std::vector<std::string>, DatabaseError>
        search(std::string_view accountId, std::string_view text, std::size_t limit) const;
        [[nodiscard]] std::variant<std::vector<std::string>, DatabaseError>
        searchAll(std::string_view accountId, std::string_view text) const;
        [[nodiscard]] std::optional<DatabaseError> rebuild(std::string_view accountId) const;

      private:
        DatabaseReadView m_cacheConnection;
        DatabaseConnection* m_writerConnection = nullptr;
    };
} // namespace javelin::jmap::cache
