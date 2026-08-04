#include "jmap/submission/DraftInlineImageStorage.h"

#include "jmap/cache/MailVault.h"

#include <QDir>

namespace javelin::jmap::submission
{

    std::variant<DraftAttachment, javelin::jmap::OperationError>
    materializeDraftInlineImage(const javelin::jmap::cache::MailVault& vault,
                                DraftAttachment attachment,
                                const javelin::jmap::AttachmentDownload& download)
    {
        const auto installed = vault.install(download.payload);
        if (const auto* error = std::get_if<javelin::jmap::cache::MailVaultError>(&installed))
        {
            return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::LocalStorageFailure,
                .message = error->message,
            };
        }

        const auto& object = std::get<javelin::jmap::cache::MailVaultObject>(installed);
        attachment.localFilePath =
            QDir{vault.rootPath()}.filePath(object.relativePath).toStdString();
        attachment.contentHash = object.contentHash;
        attachment.size = object.size;
        if (attachment.displayName.empty() && download.name.has_value())
        {
            attachment.displayName = *download.name;
        }
        if (attachment.mediaType.empty())
        {
            attachment.mediaType = download.mediaType;
        }
        return attachment;
    }

} // namespace javelin::jmap::submission
