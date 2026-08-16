#include "app/AccountCredentialStore.h"
#include "app/SettingsRepository.h"
#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/MethodCaller.h"
#include "jmap/api/MethodEnvelope.h"
#include "jmap/api/Transport.h"
#include "jmap/cache/SessionRepository.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <QCoroSignal>
#include <QCoroTask>

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QNetworkAccessManager>
#include <QSettings>
#include <QStandardPaths>

#include <glaze/glaze.hpp>

#include <algorithm>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    struct ConfiguredAccount
    {
        QString connectionId;
        QString displayName;
        QString loginEmail;
        std::string sessionUrl;
        std::vector<std::string> cachedAccountIds;
    };

    [[nodiscard]] std::vector<ConfiguredAccount>
    configuredAccounts(javelin::app::SettingsRepository& settings)
    {
        const auto loaded = settings.load();
        if (const auto* error = std::get_if<javelin::app::SettingsRepositoryError>(&loaded))
            throw std::runtime_error(error->detail.toStdString());
        const auto& snapshot = std::get<javelin::protocol::SettingsSnapshot>(loaded);
        std::vector<ConfiguredAccount> result;
        result.reserve(snapshot.accounts.size());
        for (const auto& account : snapshot.accounts)
        {
            std::vector<std::string> cachedAccountIds;
            cachedAccountIds.reserve(account.cachedAccountIds.size());
            for (const auto& id : account.cachedAccountIds)
                cachedAccountIds.push_back(id.toStdString());
            result.push_back({
                .connectionId = account.id,
                .displayName = account.displayName,
                .loginEmail = account.loginEmail,
                .sessionUrl = account.sessionUrl.toStdString(),
                .cachedAccountIds = std::move(cachedAccountIds),
            });
        }
        return result;
    }

    [[nodiscard]] std::unique_ptr<QSettings> daemonSettings()
    {
        return std::make_unique<QSettings>(QSettings::NativeFormat, QSettings::UserScope,
                                           QStringLiteral("Javelin Mail"),
                                           QStringLiteral("javelinmail"));
    }

    [[nodiscard]] javelin::app::AccountCredentialSecrets
    accountCredentials(javelin::app::AccountCredentialStore& store,
                       const ConfiguredAccount& account)
    {
        const auto loaded = store.load(account.connectionId);
        if (const auto* error = std::get_if<javelin::app::AccountCredentialStoreError>(&loaded))
            throw std::runtime_error(error->detail.toStdString());
        const auto& credentials =
            std::get<std::optional<javelin::app::AccountCredentialSecrets>>(loaded);
        if (!credentials.has_value() || !credentials->hasAccessToken())
            throw std::runtime_error("The configured account needs to be signed in again");
        return *credentials;
    }

    [[nodiscard]] const ConfiguredAccount&
    selectAccount(const std::vector<ConfiguredAccount>& accounts, const QString& name,
                  const bool requireCachedAccountId)
    {
        const auto account = std::ranges::find_if(
            accounts,
            [&name](const auto& candidate)
            {
                return candidate.displayName.compare(name, Qt::CaseInsensitive) == 0 ||
                       candidate.loginEmail.compare(name, Qt::CaseInsensitive) == 0;
            });
        if (account == accounts.end())
            throw std::runtime_error("No configured account matches --account");
        if (requireCachedAccountId && account->cachedAccountIds.empty())
            throw std::runtime_error("The configured account has no cached JMAP account id");
        if (account->sessionUrl.empty())
            throw std::runtime_error("The configured account has incomplete connection settings");
        return *account;
    }

    [[nodiscard]] std::string requestJson(const QCommandLineParser& parser)
    {
        const auto inlineJson = parser.value(QStringLiteral("json"));
        const auto requestPath = parser.value(QStringLiteral("request"));
        if (!inlineJson.isEmpty() && !requestPath.isEmpty())
            throw std::runtime_error("Use only one of --json or --request");
        if (!inlineJson.isEmpty())
            return inlineJson.toStdString();

        QFile input;
        if (!requestPath.isEmpty())
        {
            input.setFileName(requestPath);
            if (!input.open(QIODevice::ReadOnly))
                throw std::runtime_error("Unable to open the JMAP request file");
        }
        else if (!input.open(stdin, QIODevice::ReadOnly))
        {
            throw std::runtime_error("Unable to read the JMAP request from stdin");
        }
        constexpr qint64 maximumRequestBytes = 4 * 1024 * 1024;
        const auto body = input.read(maximumRequestBytes + 1);
        if (body.size() > maximumRequestBytes)
            throw std::runtime_error("The JMAP request exceeds 4 MiB");
        if (body.trimmed().isEmpty())
            throw std::runtime_error("No JMAP request was provided");
        return body.toStdString();
    }

    void printFailure(const javelin::jmap::api::MethodCallerResult& result)
    {
        if (const auto* transportError = std::get_if<javelin::jmap::api::TransportError>(&result))
            std::cerr << "Transport error: " << transportError->message << '\n';
        else if (const auto* authError = std::get_if<javelin::jmap::api::AuthError>(&result))
            std::cerr << "Authentication error: " << authError->message << '\n';
        else if (const auto* protocolError =
                     std::get_if<javelin::jmap::api::ProtocolError>(&result))
            std::cerr << "Protocol error: " << protocolError->message << '\n';
        else
            std::cerr << "Unknown JMAP failure\n";
    }

    int printLiveSession(QCoreApplication& application, const ConfiguredAccount& account,
                         const javelin::app::AccountCredentialSecrets& credentials)
    {
        QNetworkAccessManager network;
        javelin::jmap::api::QtNetworkTransport transport{network};
        int exitCode = 1;
        auto task = [&]() -> QCoro::Task<void>
        {
            const auto result = co_await transport.send({
                .method = javelin::jmap::api::HttpMethod::Get,
                .url = QUrl{QString::fromStdString(account.sessionUrl)},
                .headers =
                    {
                        {.name = "Authorization",
                         .value = QByteArrayLiteral("Bearer ") + credentials.accessToken.toUtf8()},
                        {.name = "Accept", .value = "application/json"},
                    },
                .body = {},
                .authentication =
                    javelin::jmap::api::BearerAuthentication{
                        .accountId = account.cachedAccountIds.empty()
                                         ? std::string{}
                                         : account.cachedAccountIds.front(),
                        .accessToken = credentials.accessToken.toStdString(),
                    },
                .cancellation = {},
                .dispatched = {},
            });
            if (const auto* error = std::get_if<javelin::jmap::api::TransportError>(&result))
            {
                std::cerr << "Transport error: " << error->message << '\n';
            }
            else
            {
                const auto& response = std::get<javelin::jmap::api::HttpResponse>(result);
                if (response.statusCode < 200 || response.statusCode >= 300)
                {
                    std::cerr << "Session request failed with HTTP " << response.statusCode << '\n';
                }
                else
                {
                    const auto json = response.body.toStdString();
                    std::cout << glz::prettify_json(json) << '\n';
                    exitCode = 0;
                }
            }
        }();
        QCoro::connect(std::move(task), &application, &QCoreApplication::quit);
        static_cast<void>(application.exec());
        return exitCode;
    }
} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application{argc, argv};
    application.setApplicationName(QStringLiteral("Javelin Mail"));
    application.setOrganizationName(QStringLiteral("Javelin Mail"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Send a raw JMAP request using a configured Javelin account."));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("list-accounts"),
                      QStringLiteral("List configured account names and cached JMAP ids.")});
    parser.addOption({QStringLiteral("account"),
                      QStringLiteral("Configured display name or login."), QStringLiteral("name")});
    parser.addOption(
        {QStringLiteral("session"), QStringLiteral("Print the live JMAP Session document.")});
    parser.addOption({QStringLiteral("request"),
                      QStringLiteral("Read the complete JMAP request envelope from a file."),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("json"),
                      QStringLiteral("Use an inline complete JMAP request envelope."),
                      QStringLiteral("json")});
    parser.addOption({QStringLiteral("verbose"),
                      QStringLiteral("Print the selected endpoint and request to stderr.")});
    parser.addOption({QStringLiteral("websocket"),
                      QStringLiteral("Require the advertised JMAP WebSocket transport.")});
    parser.process(application);

    try
    {
        if (parser.isSet(QStringLiteral("session")) &&
            (parser.isSet(QStringLiteral("json")) || parser.isSet(QStringLiteral("request"))))
            throw std::runtime_error("--session cannot be combined with --json or --request");

        auto credentialStore = javelin::app::makeKWalletAccountCredentialStore();
        javelin::app::SettingsRepository settings{daemonSettings(), credentialStore.get()};
        const auto accounts = configuredAccounts(settings);
        if (parser.isSet(QStringLiteral("list-accounts")))
        {
            for (const auto& account : accounts)
            {
                std::cout << account.displayName.toStdString() << '\t'
                          << account.loginEmail.toStdString();
                for (const auto& id : account.cachedAccountIds)
                    std::cout << '\t' << id;
                std::cout << '\n';
            }
            return 0;
        }
        const auto accountName = parser.value(QStringLiteral("account"));
        if (accountName.isEmpty())
            throw std::runtime_error("--account is required unless --list-accounts is used");
        const auto& account =
            selectAccount(accounts, accountName, !parser.isSet(QStringLiteral("session")));
        const auto credentials = accountCredentials(*credentialStore, account);
        if (parser.isSet(QStringLiteral("session")))
            return printLiveSession(application, account, credentials);
        const auto parsedRequest = javelin::jmap::api::parseRequestEnvelope(requestJson(parser));
        if (!parsedRequest.ok())
            throw std::runtime_error(parsedRequest.error.value_or("Invalid JMAP request envelope"));

        const auto databasePath =
            QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                .filePath(QStringLiteral("cache.sqlite3"));
        auto opened = javelin::jmap::cache::DatabaseConnection::open(
            {.connectionName = QStringLiteral("jmap-query"), .databasePath = databasePath});
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&opened))
            throw std::runtime_error(error->message.toStdString());
        auto database = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
        javelin::jmap::cache::SessionRepository sessions{database};
        const auto ownerAccountId = account.cachedAccountIds.front();
        const auto loaded = sessions.load(ownerAccountId);
        const auto* session = std::get_if<std::optional<javelin::jmap::api::Session>>(&loaded);
        if (session == nullptr || !session->has_value())
            throw std::runtime_error("No cached JMAP session is available");

        auto request = *parsedRequest.value;
        if (parser.isSet(QStringLiteral("verbose")))
        {
            const auto serialized = javelin::jmap::api::serializeRequestEnvelope(request);
            std::cerr << "Account: " << account.displayName.toStdString() << " (" << ownerAccountId
                      << ")\nEndpoint: " << session->value().apiUrl << '\n';
            if (serialized)
                std::cerr << glz::prettify_json(*serialized) << '\n';
        }

        QNetworkAccessManager network;
        javelin::jmap::api::QtNetworkTransport networkTransport{network};
        javelin::jmap::api::HttpJmapMethodTransport httpTransport{networkTransport};
        javelin::jmap::api::WebSocketFailureCooldowns cooldowns;
        javelin::jmap::api::PreferredJmapMethodTransport preferredTransport{database, httpTransport,
                                                                            cooldowns};
        auto& transport =
            parser.isSet(QStringLiteral("websocket"))
                ? static_cast<javelin::jmap::api::JmapMethodTransport&>(preferredTransport)
                : static_cast<javelin::jmap::api::JmapMethodTransport&>(httpTransport);
        javelin::jmap::api::MethodCaller caller{transport};
        const javelin::jmap::api::ApiRequestContext context{
            .credentials = {.accountId = ownerAccountId,
                            .emailAddress = account.loginEmail.toStdString(),
                            .sessionUrl = account.sessionUrl,
                            .token = {.accessToken = credentials.accessToken.toStdString(),
                                      .refreshToken = credentials.refreshToken.isEmpty()
                                                          ? std::nullopt
                                                          : std::optional{credentials.refreshToken
                                                                              .toStdString()},
                                      .expiry = std::nullopt}},
            .apiUrl = session->value().apiUrl,
            .transportPolicy = parser.isSet(QStringLiteral("websocket"))
                                   ? javelin::jmap::api::JmapTransportPolicy::ForceWebSocket
                                   : javelin::jmap::api::JmapTransportPolicy::Preferred,
            .requestLimits = javelin::jmap::api::coreRequestLimits(session->value()),
        };
        int exitCode = 1;
        auto task = [&]() -> QCoro::Task<void>
        {
            const auto result = co_await caller.call(context, std::move(request));
            if (const auto* response = std::get_if<javelin::jmap::api::ResponseEnvelope>(&result))
            {
                const auto serialized = javelin::jmap::api::serializeResponseEnvelope(*response);
                if (!serialized)
                {
                    std::cerr << "Unable to serialize the parsed JMAP response\n";
                }
                else
                {
                    std::cout << glz::prettify_json(*serialized) << '\n';
                    exitCode = 0;
                }
            }
            else
            {
                printFailure(result);
            }
        }();
        QCoro::connect(std::move(task), &application, &QCoreApplication::quit);
        static_cast<void>(application.exec());
        return exitCode;
    }
    catch (const std::exception& error)
    {
        std::cerr << "jmap-query: " << error.what() << '\n';
        return 1;
    }
}
