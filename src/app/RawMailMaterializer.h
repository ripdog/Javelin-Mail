#pragma once

#include "jmap/OperationError.h"
#include "jmap/api/LiveConnectionSettings.h"
#include "jmap/cache/MailVault.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <QCoroTask>

#include <QString>

#include <string>
#include <variant>

namespace javelin::jmap
{
    class MessageContentClient;
}

namespace javelin::app
{
    struct MaterializedRawMail
    {
        std::string contentHash;
        QString filePath;
        javelin::jmap::cache::MailVaultLease lease;
    };

    using RawMailMaterializationResult =
        std::variant<MaterializedRawMail, javelin::jmap::OperationError>;

    class RawMailMaterializer
    {
      public:
        RawMailMaterializer(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                            javelin::jmap::MessageContentClient& messageContentClient);

        [[nodiscard]] QCoro::Task<RawMailMaterializationResult>
        materialize(javelin::jmap::LiveConnectionSettings settings, std::string accountId,
                    std::string emailId, std::string expectedBlobId);

      private:
        javelin::jmap::cache::DatabaseConnection& m_databaseConnection;
        javelin::jmap::MessageContentClient& m_messageContentClient;
    };
} // namespace javelin::app
