#pragma once

#include "jmap/cache/Database.h"

#include <QString>
#include <QStringList>

namespace javelin::app
{

    class AccountCommandPort
    {
      public:
        virtual ~AccountCommandPort() = default;

        [[nodiscard]] virtual std::optional<javelin::jmap::cache::DatabaseError>
        removeConfiguredAccount(const QString& loginEmail, const QString& sessionUrl,
                                const QStringList& knownAccountIds) = 0;
    };

} // namespace javelin::app
