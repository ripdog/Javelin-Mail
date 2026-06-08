#pragma once

#include "jmap/cache/MessageContentTypes.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace javelin::jmap::render
{

    struct HtmlRenderDocument
    {
        std::string html;
        std::size_t inlineResourceCount = 0;
        std::size_t blockedRemoteResourceCount = 0;
    };

    class HtmlMessageDocumentBuilder
    {
      public:
        [[nodiscard]] HtmlRenderDocument
        build(std::string_view accountId, std::string_view emailId, std::string_view sourceHtml,
              const std::vector<javelin::jmap::cache::EmailPart>& parts) const;

      private:
        [[nodiscard]] static std::optional<std::string>
        makeInlinePartUrl(std::string_view accountId, std::string_view emailId,
                          const javelin::jmap::cache::EmailPart& part);
    };

} // namespace javelin::jmap::render
