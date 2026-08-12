#pragma once

#include "jmap/cache/MailboxMessageReader.h"
#include "jmap/cache/ThreadReader.h"
#include "jmap/cache/ThreadRepository.h"

#include <QString>

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace javelin::app
{

    struct SelectedEmail
    {
        std::string emailId;
    };

    struct SelectedCollapsedThread
    {
        std::string threadId;
    };

    using MessageSelectionItem = std::variant<SelectedEmail, SelectedCollapsedThread>;
    using MessageSelection = std::vector<MessageSelectionItem>;
    using ResolvedMessageSelection = std::variant<std::vector<std::string>, QString>;

    [[nodiscard]] ResolvedMessageSelection
    resolveMessageSelection(const javelin::jmap::cache::MailboxMessageReader& mailboxMessageReader,
                            const javelin::jmap::cache::ThreadReader& threadReader,
                            const javelin::jmap::cache::ThreadRepository& threadRepository,
                            std::string_view accountId, std::optional<std::string_view> mailboxId,
                            const MessageSelection& selection);

} // namespace javelin::app
