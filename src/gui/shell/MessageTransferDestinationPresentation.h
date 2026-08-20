#pragma once

#include "jmap/cache/AccountReadRepository.h"
#include "jmap/cache/MailboxReadRepository.h"

#include <QString>
#include <QStringView>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace javelin::gui::shell
{
    struct MessageTransferDestinationRow
    {
        std::string accountId;
        std::string mailboxId;
        std::string mailboxName;
        std::optional<std::string> role;
        QString mailboxPath;
        QString accountLabel;
        std::size_t depth = 0;
        bool separatorBefore = false;
    };

    struct MessageTransferDestinationAccount
    {
        std::string accountId;
        QString label;
        std::vector<MessageTransferDestinationRow> rows;
    };

    struct MessageTransferDestinationPresentation
    {
        QString currentAccountLabel;
        std::vector<MessageTransferDestinationRow> currentAccountRows;
        std::vector<MessageTransferDestinationAccount> otherAccounts;
    };

    struct MessageTransferDestinationSearchResult
    {
        MessageTransferDestinationRow destination;
        int score = 0;
    };

    using MessageTransferAccountDisplayName = std::function<QString(QStringView)>;

    [[nodiscard]] MessageTransferDestinationPresentation
    buildMessageTransferDestinationPresentation(
        std::string currentAccountId,
        const std::vector<javelin::jmap::cache::CachedAccount>& accounts,
        const std::unordered_map<std::string, std::vector<javelin::jmap::cache::MailboxTreeItem>>&
            mailboxesByAccount,
        const MessageTransferAccountDisplayName& configuredDisplayName = {});

    [[nodiscard]] std::vector<MessageTransferDestinationSearchResult>
    searchMessageTransferDestinations(const MessageTransferDestinationPresentation& presentation,
                                      QStringView query);

} // namespace javelin::gui::shell
