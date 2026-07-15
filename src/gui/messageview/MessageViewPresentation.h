#pragma once

#include <cstddef>

namespace javelin::gui::messageview
{

    enum class MessageViewPresentation
    {
        NoAccount,
        MultipleSelection,
        NoMailbox,
        NoMessage,
        Message,
    };

    [[nodiscard]] constexpr MessageViewPresentation
    messageViewPresentation(const bool hasAccount, const bool hasMailbox, const bool hasEmail,
                            const std::size_t selectedMessageCount)
    {
        if (!hasAccount)
        {
            return MessageViewPresentation::NoAccount;
        }
        if (selectedMessageCount > 0)
        {
            return MessageViewPresentation::MultipleSelection;
        }
        if (!hasMailbox && !hasEmail)
        {
            return MessageViewPresentation::NoMailbox;
        }
        if (!hasEmail)
        {
            return MessageViewPresentation::NoMessage;
        }
        return MessageViewPresentation::Message;
    }

} // namespace javelin::gui::messageview
