#pragma once

#include "storage/DatabaseError.h"

#include <QObject>

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{
    class ContactReader;
}

namespace javelin::jmap::contacts
{
    struct ContactIdentity
    {
        std::string contactId;
        std::string displayName;
        std::optional<std::string> organization;
        std::string email;
    };

    class ContactIdentityLookup : public QObject
    {
        Q_OBJECT

      public:
        explicit ContactIdentityLookup(javelin::jmap::cache::ContactReader& repository);

        [[nodiscard]] std::variant<std::optional<ContactIdentity>,
                                   javelin::jmap::cache::DatabaseError>
        resolve(std::string_view accountId, std::string_view email) const;
        [[nodiscard]] std::variant<std::vector<ContactIdentity>,
                                   javelin::jmap::cache::DatabaseError>
        suggestions(std::optional<std::string_view> accountId = std::nullopt) const;

      Q_SIGNALS:
        void contactDataChanged(const QString& accountId);

      private:
        javelin::jmap::cache::ContactReader& m_repository;
    };
} // namespace javelin::jmap::contacts
