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
        QString refreshToken;
        QString tokenEndpoint;
        QString oauthClientId;
        QString oauthIssuer = {};
        QString oauthResource = {};
        QString oauthScope = {};
        QString revocationEndpoint = {};
        qint64 tokenExpiresAtEpochSeconds = 0;
        QStringList cachedAccountIds;
    };
} // namespace javelin::gui::settings
