#include "app/MailboxSyncCoverage.h"

#include "jmap/cache/MailboxWindowRepository.h"
#include "jmap/sync/MailboxQueryDescriptor.h"

#include <optional>

namespace javelin::app
{

    std::variant<bool, javelin::jmap::cache::DatabaseError>
    hasAuthoritativeCanonicalMailboxCoverage(
        javelin::jmap::cache::DatabaseConnection& databaseConnection,
        const std::string_view accountId, const std::span<const std::string> mailboxIds)
    {
        constexpr std::size_t canonicalWindowLimit = 100;
        javelin::jmap::cache::MailboxWindowRepository windows{databaseConnection};
        for (const auto& mailboxId : mailboxIds)
        {
            const auto queryKey = javelin::jmap::sync::mailboxQueryKey({
                .mailboxId = mailboxId,
                .sortProperty = "receivedAt",
                .isAscending = false,
                .collapseThreads = true,
            });
            const auto result = windows.find(accountId, queryKey, 0, canonicalWindowLimit);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
            {
                return *error;
            }
            const auto& window =
                std::get<std::optional<javelin::jmap::cache::MailboxWindowRecord>>(result);
            if (!window.has_value() ||
                !javelin::jmap::cache::isDisplayCurrent(window->coverage, window->materialization))
            {
                return false;
            }
        }
        return true;
    }

} // namespace javelin::app
