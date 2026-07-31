#pragma once

#include "jmap/OperationError.h"
#include "jmap/sieve/SieveTypes.h"

#include <QByteArray>
#include <QString>

#include <variant>
#include <vector>

namespace javelin::jmap::sieve
{
    struct SieveValidation
    {
        bool valid = false;
        QString message;
    };

    using SieveListResult = std::variant<std::vector<SieveScript>, OperationError>;
    using SieveContentResult = std::variant<QByteArray, OperationError>;
    using SieveValidationResult = std::variant<SieveValidation, OperationError>;
    using SieveSaveResult = std::variant<SieveScript, OperationError>;
    using SieveDeleteResult = std::variant<std::monostate, OperationError>;
    using SieveActivationResult = std::variant<std::monostate, OperationError>;
} // namespace javelin::jmap::sieve
