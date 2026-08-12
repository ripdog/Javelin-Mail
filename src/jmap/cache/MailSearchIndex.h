#pragma once

#include "storage/sqlite/DatabaseConnection.h"

#include <QString>

#include <memory>
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

    class MailSearchIndexWriter final
    {
      public:
        MailSearchIndexWriter(const MailSearchIndexWriter&) = delete;
        MailSearchIndexWriter& operator=(const MailSearchIndexWriter&) = delete;
        MailSearchIndexWriter(MailSearchIndexWriter&&) noexcept;
        MailSearchIndexWriter& operator=(MailSearchIndexWriter&&) noexcept;
        ~MailSearchIndexWriter();

        [[nodiscard]] static std::variant<MailSearchIndexWriter, DatabaseError>
        open(const DatabaseConnection& cacheConnection, std::string_view accountId);
        [[nodiscard]] std::optional<DatabaseError> upsert(const SearchIndexDocument& document);
        [[nodiscard]] std::optional<DatabaseError> commit();

      private:
        struct State;
        explicit MailSearchIndexWriter(std::unique_ptr<State> state);
        std::unique_ptr<State> m_state;
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
        [[nodiscard]] std::variant<std::vector<std::string>, DatabaseError>
        searchAllBody(std::string_view accountId, std::string_view text) const;
        [[nodiscard]] std::optional<DatabaseError> rebuild(std::string_view accountId) const;

      private:
        DatabaseReadView m_cacheConnection;
        DatabaseConnection* m_writerConnection = nullptr;
    };
} // namespace javelin::jmap::cache
