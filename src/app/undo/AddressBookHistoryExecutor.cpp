#include "app/undo/AddressBookHistoryExecutor.h"

#include "app/undo/ContactHistoryExecutor.h"

#include <glaze/glaze.hpp>

#include <ranges>
#include <utility>

namespace javelin::app::undo
{
    namespace
    {
        [[nodiscard]] HistoryExecutionResult conflict(QString message)
        {
            return {
                .outcome = HistoryExecutionOutcome::Conflict,
                .updatedPayload = std::nullopt,
                .refreshScope = {},
                .summary = std::move(message),
                .objectFailures = {},
                .mayRemoveFromHistory = true,
            };
        }

        [[nodiscard]] HistoryExecutionResult failure(const javelin::jmap::OperationError& error)
        {
            using enum javelin::jmap::OperationErrorCode;
            const auto outcome =
                isTransientError(error)
                    ? HistoryExecutionOutcome::Unknown
                    : (error.code == Conflict || error.code == PreconditionFailed ||
                               error.code == NotFound
                           ? HistoryExecutionOutcome::Conflict
                           : HistoryExecutionOutcome::DefinitiveFailure);
            return {
                .outcome = outcome,
                .updatedPayload = std::nullopt,
                .refreshScope = {},
                .summary = error.message,
                .objectFailures = {},
                .mayRemoveFromHistory = !isTransientError(error),
            };
        }

        [[nodiscard]] std::optional<javelin::jmap::api::AddressBook>
        parseBook(const std::optional<std::string>& json,
                  const std::optional<std::string>& currentId)
        {
            if (!json.has_value())
                return std::nullopt;
            auto parsed = javelin::jmap::api::parseAddressBookDocument(
                currentId.value_or(std::string{}), *json);
            return parsed.value;
        }

        [[nodiscard]] bool sameBook(javelin::jmap::api::AddressBook left,
                                    javelin::jmap::api::AddressBook right,
                                    const std::string& currentId)
        {
            left.id = currentId;
            right.id = currentId;
            left.isDefault = right.isDefault;
            left.myRights = right.myRights;
            return left == right;
        }

        [[nodiscard]] std::optional<std::string>
        remapAddressBook(std::string json, const std::string& oldId, const std::string& newId)
        {
            glz::generic value;
            if (glz::read_json(value, json) || !value.is_object())
                return std::nullopt;
            auto books = value.get_object().find("addressBookIds");
            if (books == value.get_object().end() || !books->second.is_object())
                return std::nullopt;
            auto& memberships = books->second.get_object();
            const auto old = memberships.find(oldId);
            if (old != memberships.end())
            {
                memberships[newId] = old->second;
                memberships.erase(old);
            }
            std::string result;
            if (glz::write_json(value, result))
                return std::nullopt;
            return result;
        }

        [[nodiscard]] HistoryExecutionResult success(AddressBookHistory history)
        {
            const auto accountId = QString::fromStdString(history.accountId);
            return {
                .outcome = HistoryExecutionOutcome::Success,
                .updatedPayload = std::move(history),
                .refreshScope =
                    {
                        .accountIds = {accountId},
                        .objectTypes = {QStringLiteral("AddressBook"),
                                        QStringLiteral("ContactCard")},
                        .views = {QStringLiteral("contacts")},
                    },
                .summary = {},
                .objectFailures = {},
                .mayRemoveFromHistory = false,
            };
        }
    } // namespace

    AddressBookHistoryExecutor::AddressBookHistoryExecutor(AddressBookHistoryPort& addressBooks,
                                                           ContactHistoryPort& contacts)
        : m_addressBooks(addressBooks), m_contacts(contacts)
    {
    }

    QCoro::Task<HistoryExecutionResult>
    AddressBookHistoryExecutor::execute(HistoryEntry entry,
                                        const HistoryExecutionDirection direction)
    {
        auto* history = std::get_if<AddressBookHistory>(&entry.payload);
        if (history == nullptr || direction == HistoryExecutionDirection::Recover)
            co_return conflict(QStringLiteral(
                "The address-book operation requires authoritative reconciliation."));
        const bool undo = direction == HistoryExecutionDirection::Undo;
        const auto expected =
            parseBook(undo ? history->afterDocumentJson : history->beforeDocumentJson,
                      history->currentAddressBookId);
        auto desired = parseBook(undo ? history->beforeDocumentJson : history->afterDocumentJson,
                                 history->currentAddressBookId);
        auto loaded = co_await m_addressBooks.getAuthoritativeAddressBooks(history->connectionId,
                                                                           history->accountId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&loaded))
            co_return failure(*error);
        const auto& authoritative = std::get<AuthoritativeAddressBooks>(loaded);
        const javelin::jmap::api::AddressBook* current = nullptr;
        if (history->currentAddressBookId.has_value())
        {
            const auto found =
                std::ranges::find(authoritative.addressBooks, *history->currentAddressBookId,
                                  &javelin::jmap::api::AddressBook::id);
            if (found != authoritative.addressBooks.end())
                current = &*found;
        }
        if (expected.has_value())
        {
            if (current == nullptr || !history->currentAddressBookId.has_value() ||
                !sameBook(*current, *expected, *history->currentAddressBookId))
                co_return conflict(QStringLiteral("The address book changed on another client."));
        }
        else if (current != nullptr)
            co_return conflict(
                QStringLiteral("An address book exists where this history expected none."));

        javelin::jmap::api::AddressBookSetRequest request{
            .accountId = history->accountId,
            .ifInState =
                authoritative.state.empty() ? std::nullopt : std::optional{authoritative.state},
            .create = {},
            .update = {},
            .destroy = {},
            .onDestroyRemoveContents = false,
            .onSuccessSetIsDefault =
                undo ? history->beforeDefaultAddressBookId : history->afterDefaultAddressBookId,
        };
        std::optional<std::string> oldId = history->currentAddressBookId;
        if (!desired.has_value())
        {
            if (current == nullptr)
                co_return conflict(QStringLiteral("The address-book identity is unavailable."));
            request.destroy.push_back(current->id);
            request.onDestroyRemoveContents = !history->affectedCards.empty();
        }
        else if (current != nullptr)
            request.update.emplace(current->id,
                                   javelin::jmap::api::addressBookUpdateDocument(*desired));
        else
            request.create.emplace("history-address-book",
                                   javelin::jmap::api::addressBookCreateDocument(*desired));

        auto mutated = co_await m_addressBooks.applyAddressBooksFromHistory(
            history->connectionId, std::move(request),
            undo ? CommandOrigin::Undo : CommandOrigin::Redo);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&mutated))
            co_return failure(*error);
        const auto& summary = std::get<javelin::jmap::contacts::ContactMutationSummary>(mutated);
        if (current == nullptr && desired.has_value())
        {
            if (!summary.createdId.has_value())
                co_return conflict(
                    QStringLiteral("The recreated address book has no server identity."));
            history->currentAddressBookId = summary.createdId;
            if (!history->affectedCards.empty() && oldId.has_value())
            {
                for (auto& item : history->affectedCards)
                {
                    auto rewritten =
                        remapAddressBook(*item.beforeDocumentJson, *oldId, *summary.createdId);
                    if (!rewritten.has_value())
                        co_return conflict(
                            QStringLiteral("A captured contact document is invalid."));
                    item.beforeDocumentJson = std::move(*rewritten);
                    item.currentCardId = std::nullopt;
                }
                HistoryEntry contactEntry;
                contactEntry.payload = ContactCardHistory{
                    .connectionId = history->connectionId,
                    .accountId = history->accountId,
                    .items = history->affectedCards,
                };
                ContactHistoryExecutor contactExecutor{m_contacts};
                auto restored = co_await contactExecutor.execute(std::move(contactEntry),
                                                                 HistoryExecutionDirection::Undo);
                if (restored.outcome != HistoryExecutionOutcome::Success)
                {
                    restored.outcome = HistoryExecutionOutcome::PartialFailure;
                    restored.updatedPayload = *history;
                    co_return restored;
                }
                history->affectedCards =
                    std::get<ContactCardHistory>(*restored.updatedPayload).items;
            }
        }
        else if (!desired.has_value())
            history->currentAddressBookId = std::nullopt;
        co_return success(*history);
    }
} // namespace javelin::app::undo
