#pragma once

#include "jmap/JmapCore.h"
#include "jmap/submission/ComposeTypes.h"

#include <variant>

namespace javelin::jmap::cache
{
    class MailVault;
}

namespace javelin::jmap::submission
{

    [[nodiscard]] std::variant<DraftAttachment, javelin::jmap::OperationError>
    materializeDraftInlineImage(const javelin::jmap::cache::MailVault& vault,
                                DraftAttachment attachment,
                                const javelin::jmap::AttachmentDownload& download);

} // namespace javelin::jmap::submission
