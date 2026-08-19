#include "app/RawMailMaterializer.h"

#include "jmap/MessageContentClient.h"
#include "jmap/cache/RawMessageSourceRepository.h"

#include <KLocalizedString>

#include <utility>

namespace javelin::app
{
    RawMailMaterializer::RawMailMaterializer(
        javelin::jmap::cache::DatabaseConnection& databaseConnection,
        javelin::jmap::MessageContentClient& messageContentClient)
        : m_databaseConnection(databaseConnection), m_messageContentClient(messageContentClient)
    {
    }

    QCoro::Task<RawMailMaterializationResult>
    RawMailMaterializer::materialize(javelin::jmap::LiveConnectionSettings settings,
                                     std::string accountId, std::string emailId,
                                     std::string expectedBlobId)
    {
        auto refreshed =
            co_await m_messageContentClient.refresh(std::move(settings), accountId, emailId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&refreshed))
            co_return *error;
        if (const auto* unavailable =
                std::get_if<javelin::jmap::MessageContentUnavailable>(&refreshed))
        {
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::NotFound,
                .message = unavailable->message,
            };
        }

        javelin::jmap::cache::RawMessageSourceRepository repository{m_databaseConnection};
        auto referenceResult = repository.findReference(accountId, emailId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&referenceResult))
            co_return javelin::jmap::operationError(*error);
        auto reference = std::get<std::optional<javelin::jmap::cache::RawMessageSourceReference>>(
            std::move(referenceResult));
        if (!reference.has_value())
        {
            const auto migration = repository.migrateLegacySources(25);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&migration))
                co_return javelin::jmap::operationError(*error);
            referenceResult = repository.findReference(accountId, emailId);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&referenceResult))
                co_return javelin::jmap::operationError(*error);
            reference = std::get<std::optional<javelin::jmap::cache::RawMessageSourceReference>>(
                std::move(referenceResult));
        }
        if (!reference.has_value() || reference->blobId != expectedBlobId)
        {
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::NotFound,
                .message = i18n("The exact raw source for this message is no longer available."),
            };
        }

        const auto vault = javelin::jmap::cache::MailVault::forDatabase(m_databaseConnection);
        auto leaseResult = vault.acquireLease(reference->object);
        if (const auto* error = std::get_if<javelin::jmap::cache::MailVaultError>(&leaseResult))
        {
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::LocalStorageFailure,
                .message = error->message,
            };
        }
        auto lease = std::get<javelin::jmap::cache::MailVaultLease>(std::move(leaseResult));
        const auto pathResult = lease.filePath();
        if (const auto* error = std::get_if<javelin::jmap::cache::MailVaultError>(&pathResult))
        {
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::LocalStorageFailure,
                .message = error->message,
            };
        }

        co_return MaterializedRawMail{
            .contentHash = reference->object.contentHash,
            .filePath = std::get<QString>(pathResult),
            .lease = std::move(lease),
        };
    }
} // namespace javelin::app
