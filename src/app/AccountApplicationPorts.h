#pragma once

#include "storage/DatabaseError.h"

#include <QCoroTask>

#include <QString>
#include <QStringList>

namespace javelin::app
{

    class AccountCommandPort
    {
      public:
        virtual ~AccountCommandPort() = default;

        [[nodiscard]] virtual QCoro::Task<std::optional<javelin::jmap::cache::DatabaseError>>
        removeConfiguredAccount(QString loginEmail, QString sessionUrl,
                                QStringList knownAccountIds) = 0;
    };

} // namespace javelin::app
