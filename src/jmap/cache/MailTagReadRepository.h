#pragma once

#include "jmap/cache/MailTagReader.h"
#include "storage/sqlite/DatabaseConnection.h"

namespace javelin::jmap::cache
{
    class MailTagReadRepository final : public MailTagReader
    {
      public:
        explicit MailTagReadRepository(DatabaseConnection& connection);
        explicit MailTagReadRepository(ReadOnlyDatabaseConnection& connection);
        explicit MailTagReadRepository(DatabaseReadView connection);

        [[nodiscard]] std::variant<std::vector<std::string>, DatabaseError>
        listUserKeywords(std::string_view accountId,
                         std::string_view mailboxId = {}) const override;
        [[nodiscard]] std::variant<std::vector<std::string>, DatabaseError>
        listTagKeywords(std::string_view accountId) const override;
        [[nodiscard]] std::variant<std::vector<EmailKeywordMembership>, DatabaseError>
        listEmailKeywordMemberships(std::string_view accountId,
                                    const std::vector<std::string>& emailIds) const override;
        [[nodiscard]] std::variant<std::vector<TagDefinition>, DatabaseError>
        listTagDefinitions(std::string_view accountId) const override;

      private:
        DatabaseReadView m_connection;
    };

} // namespace javelin::jmap::cache
