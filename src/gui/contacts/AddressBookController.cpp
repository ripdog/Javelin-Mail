#include "gui/contacts/AddressBookController.h"

#include <QCoroTask>

#include <algorithm>
#include <ranges>
#include <utility>
#include <variant>

namespace javelin::gui::contacts
{
    AddressBookController::AddressBookController(
        javelin::app::ContactCommandPort& commandPort,
        javelin::jmap::cache::ContactReader& repository, QObject& context,
        std::function<std::optional<std::string>(std::string_view)> ownerAccountId,
        std::function<void(bool)> setBusy, std::function<void(QString, int)> statusMessage)
        : m_commandPort(commandPort), m_repository(repository), m_context(context),
          m_ownerAccountId(std::move(ownerAccountId)), m_setBusy(std::move(setBusy)),
          m_statusMessage(std::move(statusMessage))
    {
    }

    bool AddressBookController::canSetSubscription(
        const std::vector<javelin::jmap::cache::ContactAccount>& accounts,
        const std::string_view accountId, const javelin::jmap::api::AddressBook& book,
        const bool subscribed) const
    {
        const auto account = std::ranges::find(accounts, accountId,
                                               &javelin::jmap::cache::ContactAccount::accountId);
        if (account == accounts.end() || account->isReadOnly || book.isSubscribed == subscribed)
            return false;
        if (subscribed)
            return true;
        const auto listedBooks = m_repository.listAddressBooks(accountId);
        const auto* books = std::get_if<std::vector<javelin::jmap::api::AddressBook>>(&listedBooks);
        return books != nullptr && books->size() > 1;
    }

    void AddressBookController::mutate(javelin::app::AddressBookCommand command,
                                       QString progressMessage)
    {
        const auto accountId =
            std::visit([](const auto& value) { return value.accountId; }, command);
        const auto owner = m_ownerAccountId(accountId).value_or(std::string{});
        m_setBusy(true);
        m_statusMessage(std::move(progressMessage), 5000);
        auto task = m_commandPort.mutateAddressBook(owner, std::move(command));
        QCoro::connect(std::move(task), &m_context,
                       [this](javelin::jmap::contacts::ContactMutationResult result)
                       {
                           m_setBusy(false);
                           if (const auto* error =
                                   std::get_if<javelin::jmap::OperationError>(&result))
                               m_statusMessage(error->message, 10000);
                       });
    }
} // namespace javelin::gui::contacts
