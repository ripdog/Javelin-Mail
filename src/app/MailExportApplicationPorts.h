#pragma once

#include "jmap/OperationError.h"

#include <QCoroTask>

#include <QString>

#include <optional>
#include <string>
#include <variant>

namespace javelin::app
{
    enum class MailExportFormat
    {
        Eml,
        MboxRd,
    };

    enum class MailExportScopeKind
    {
        Mailbox,
        Account,
    };

    struct MailExportIntent
    {
        std::string accountId;
        MailExportScopeKind scopeKind = MailExportScopeKind::Mailbox;
        std::optional<std::string> mailboxId;
        MailExportFormat format = MailExportFormat::Eml;
        QString destinationDirectory;
    };

    struct MailExportAdmission
    {
        std::string operationId;
        std::string jobId;
    };

    using MailExportStartResult = std::variant<MailExportAdmission, javelin::jmap::OperationError>;

    class MailExportPort
    {
      public:
        virtual ~MailExportPort() = default;

        [[nodiscard]] virtual QCoro::Task<MailExportStartResult>
        startExport(MailExportIntent intent) = 0;
    };
} // namespace javelin::app
