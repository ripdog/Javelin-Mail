#pragma once

#include "app/undo/ContactHistoryPort.h"

namespace javelin::app::undo
{
    struct AuthoritativeAddressBooks
    {
        std::string state;
        std::vector<javelin::jmap::api::AddressBook> addressBooks;
    };

    using AuthoritativeAddressBooksResult =
        std::variant<AuthoritativeAddressBooks, javelin::jmap::OperationError>;

    class AddressBookHistoryPort
    {
      public:
        virtual ~AddressBookHistoryPort() = default;

        [[nodiscard]] virtual AuthoritativeAddressBooksResult
        getEffectiveAddressBooks(std::string_view accountId) = 0;
        [[nodiscard]] virtual QCoro::Task<AuthoritativeAddressBooksResult>
        getAuthoritativeAddressBooks(std::string ownerAccountId, std::string accountId) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        applyAddressBooksFromHistory(std::string ownerAccountId,
                                     javelin::jmap::api::AddressBookSetRequest request,
                                     CommandOrigin origin) = 0;
    };
} // namespace javelin::app::undo
