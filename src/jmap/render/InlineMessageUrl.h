#pragma once

#include <QUrl>

#include <optional>
#include <string>
#include <string_view>

namespace javelin::jmap::render
{

    struct InlineMessageUrlParts
    {
        std::string accountId;
        std::string emailId;
        std::string partId;
        std::string blobId;
    };

    [[nodiscard]] QString inlineMessageUrlScheme();
    [[nodiscard]] std::string buildInlineMessageUrl(std::string_view accountId,
                                                    std::string_view emailId,
                                                    std::string_view partId,
                                                    std::string_view blobId);
    [[nodiscard]] std::optional<InlineMessageUrlParts> parseInlineMessageUrl(const QUrl& url);

} // namespace javelin::jmap::render
