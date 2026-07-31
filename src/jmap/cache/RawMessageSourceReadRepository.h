#pragma once

#include "jmap/cache/Database.h"
#include "jmap/cache/RawMessageSourceRepository.h"

namespace javelin::jmap::cache
{

    class RawMessageSourceReadRepository final
    {
      public:
        explicit RawMessageSourceReadRepository(const DatabaseReadView& connection);

        [[nodiscard]] std::variant<std::optional<RawMessageSource>, DatabaseError>
        find(std::string_view accountId, std::string_view emailId) const;

      private:
        DatabaseReadView m_connection;
    };

} // namespace javelin::jmap::cache
