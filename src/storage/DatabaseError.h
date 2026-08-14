#pragma once

#include <QString>

class QSqlError;

namespace javelin::jmap::cache
{
    enum class DatabaseErrorCode
    {
        DriverUnavailable,
        OpenFailed,
        QueryFailed,
        MigrationFailed,
        TransientContention,
        ThreadAffinityViolation,
    };

    struct DatabaseError
    {
        DatabaseErrorCode code;
        QString message;
    };

    [[nodiscard]] DatabaseError
    databaseError(const QString& operation, const QSqlError& error,
                  DatabaseErrorCode fallback = DatabaseErrorCode::QueryFailed);

} // namespace javelin::jmap::cache
