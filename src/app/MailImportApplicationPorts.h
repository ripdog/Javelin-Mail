#pragma once

#include "jmap/OperationError.h"

#include <QCoroTask>

#include <QString>

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace javelin::app
{
    struct MailImportIntent
    {
        std::string accountId;
        std::optional<std::string> mailboxId;
        std::vector<QString> sourcePaths;
        bool recreateHierarchy = false;
    };

    struct MailImportAdmission
    {
        std::string operationId;
        std::string jobId;
    };

    using MailImportStartResult = std::variant<MailImportAdmission, javelin::jmap::OperationError>;

    class MailImportPort
    {
      public:
        virtual ~MailImportPort() = default;

        [[nodiscard]] virtual QCoro::Task<MailImportStartResult>
        startImport(MailImportIntent intent) = 0;
    };
} // namespace javelin::app
