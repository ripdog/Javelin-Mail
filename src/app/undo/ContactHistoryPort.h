#pragma once

#include "app/undo/HistoryTypes.h"
#include "jmap/contacts/ContactResults.h"
#include "jmap/contacts/ContactTypes.h"

#include <QCoroTask>

#include <string>
#include <variant>
#include <vector>

namespace javelin::app::undo
{
    struct AuthoritativeContacts
    {
        std::string state;
        std::vector<javelin::jmap::contacts::ContactSummary> contacts;
    };

    using AuthoritativeContactsResult =
        std::variant<AuthoritativeContacts, javelin::jmap::OperationError>;

    class ContactHistoryPort
    {
      public:
        virtual ~ContactHistoryPort() = default;

        [[nodiscard]] virtual QCoro::Task<AuthoritativeContactsResult>
        getAuthoritativeContacts(std::string ownerAccountId, std::string accountId) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        applyContactCardsFromHistory(std::string ownerAccountId,
                                     javelin::jmap::api::ContactCardSetRequest request,
                                     CommandOrigin origin) = 0;
    };
} // namespace javelin::app::undo
