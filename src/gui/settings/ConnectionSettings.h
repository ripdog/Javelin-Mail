#pragma once

#include <QString>
#include <QStringList>

#include <cstdint>

namespace javelin::gui::settings
{
    struct ConnectionSettings
    {
        QString id;
        std::uint64_t revision = 0;
        QString displayName;
        QString sessionUrl;
        QString loginEmail;
        QString apiKey;
        QStringList cachedAccountIds;
    };
} // namespace javelin::gui::settings
