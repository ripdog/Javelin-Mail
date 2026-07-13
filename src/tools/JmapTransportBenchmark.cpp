#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/MailMethods.h"
#include "jmap/api/MethodCaller.h"
#include "jmap/api/ResponseReader.h"
#include "jmap/api/Transport.h"
#include "jmap/cache/Database.h"
#include "jmap/cache/SessionRepository.h"

#include <QCoroSignal>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QSettings>
#include <QStandardPaths>

#include <iostream>
#include <ranges>
#include <stdexcept>

namespace
{
    using namespace javelin::jmap::api;

    struct Account
    {
        QString displayName;
        std::string sessionUrl;
        std::string loginEmail;
        std::string apiKey;
        std::string accountId;
    };

    [[nodiscard]] Account configuredAccount(const QString& displayName)
    {
        QSettings settings;
        settings.beginGroup(QStringLiteral("accounts"));
        const int count = settings.beginReadArray(QStringLiteral("size"));
        for (int index = 0; index < count; ++index)
        {
            settings.setArrayIndex(index);
            if (settings.value(QStringLiteral("displayName")).toString() != displayName)
            {
                continue;
            }
            const auto ids = settings.value(QStringLiteral("cachedAccountIds")).toStringList();
            if (ids.empty())
            {
                throw std::runtime_error("The account has no cached JMAP account id");
            }
            return {
                .displayName = displayName,
                .sessionUrl = settings.value(QStringLiteral("sessionUrl")).toString().toStdString(),
                .loginEmail = settings.value(QStringLiteral("loginEmail")).toString().toStdString(),
                .apiKey = settings.value(QStringLiteral("apiKey")).toString().toStdString(),
                .accountId = ids.front().toStdString()};
        }
        throw std::runtime_error("No configured account has that display name");
    }

    template <typename Response>
    [[nodiscard]] Response read(const MethodCallerResult& result,
                                const CallHandle<Response>& handle)
    {
        const auto* envelope = std::get_if<ResponseEnvelope>(&result);
        if (envelope == nullptr)
        {
            throw std::runtime_error("JMAP transport or authentication request failed");
        }
        const auto parsed = ResponseReader{*envelope}.get(handle);
        if (const auto* error = std::get_if<ResponseReaderError>(&parsed))
        {
            throw std::runtime_error(error->message);
        }
        return std::get<Response>(parsed);
    }

    [[nodiscard]] QCoro::Task<void> runBenchmark(const Account account,
                                                 javelin::jmap::cache::DatabaseConnection& database,
                                                 JmapMethodTransport& transport,
                                                 const JmapTransportPolicy policy,
                                                 const QString label)
    {
        javelin::jmap::cache::SessionRepository sessions{database};
        const auto sessionResult = sessions.load(account.accountId);
        const auto* session = std::get_if<std::optional<Session>>(&sessionResult);
        if (session == nullptr || !session->has_value())
        {
            throw std::runtime_error("No cached JMAP session is available for the account");
        }
        const ApiRequestContext context{.credentials = {.accountId = account.accountId,
                                                        .emailAddress = account.loginEmail,
                                                        .sessionUrl = account.sessionUrl,
                                                        .token = {.accessToken = account.apiKey,
                                                                  .refreshToken = std::nullopt,
                                                                  .expiry = std::nullopt}},
                                        .apiUrl = session->value().apiUrl,
                                        .transportPolicy = policy};
        MethodCaller caller{transport};
        QElapsedTimer total;
        total.start();

        QElapsedTimer timer;
        timer.start();
        RequestBuilder mailboxesBuilder;
        mailboxesBuilder.useCore().useMail();
        const auto mailboxRequest = mailboxGet({.accountId = account.accountId,
                                                .ids = std::nullopt,
                                                .idsReference = std::nullopt,
                                                .properties = std::nullopt});
        const auto mailboxHandle = mailboxesBuilder.call(*mailboxRequest);
        const auto mailboxResponse =
            read(co_await caller.call(context, mailboxesBuilder), mailboxHandle);
        std::cout << label.toStdString() << " sequential Mailbox/get: " << timer.restart()
                  << " ms\n";
        const auto inbox =
            std::ranges::find_if(mailboxResponse.list, [](const auto& mailbox)
                                 { return mailbox.role.has_value() && *mailbox.role == "inbox"; });
        if (inbox == mailboxResponse.list.end())
        {
            throw std::runtime_error("The account has no Inbox mailbox");
        }

        RequestBuilder queryBuilder;
        queryBuilder.useCore().useMail();
        EmailQueryFilter inboxFilter;
        inboxFilter.inMailbox = inbox->id;
        const auto queryRequest = emailQuery({.accountId = account.accountId,
                                              .filter = std::move(inboxFilter),
                                              .sort = {},
                                              .position = std::nullopt,
                                              .limit = 1,
                                              .collapseThreads = false,
                                              .calculateTotal = false});
        const auto queryHandle = queryBuilder.call(*queryRequest);
        const auto queryResponse = read(co_await caller.call(context, queryBuilder), queryHandle);
        std::cout << label.toStdString() << " sequential Email/query: " << timer.restart()
                  << " ms\n";
        if (queryResponse.ids.empty())
        {
            throw std::runtime_error("Inbox contains no email");
        }

        RequestBuilder contentBuilder;
        contentBuilder.useCore().useMail();
        const auto contentRequest =
            emailContentGet({.accountId = account.accountId,
                             .ids = {queryResponse.ids.front()},
                             .properties = {"id", "subject", "textBody", "htmlBody", "bodyValues"},
                             .bodyProperties = {"partId", "type"},
                             .fetchTextBodyValues = true,
                             .fetchHTMLBodyValues = true,
                             .fetchAllBodyValues = false,
                             .maxBodyValueBytes = std::nullopt});
        const auto contentHandle = contentBuilder.call(*contentRequest);
        static_cast<void>(read(co_await caller.call(context, contentBuilder), contentHandle));
        std::cout << label.toStdString() << " sequential Email/get content: " << timer.restart()
                  << " ms\n"
                  << label.toStdString() << " sequential total: " << total.elapsed() << " ms\n";

        RequestBuilder batch;
        batch.useCore().useMail();
        static_cast<void>(batch.call(*mailboxRequest, "mailboxes"));
        static_cast<void>(batch.call(*queryRequest, "inbox-mails"));
        static_cast<void>(batch.call(*contentRequest, "content"));
        timer.restart();
        const auto batchResult = co_await caller.call(context, batch);
        if (!std::holds_alternative<ResponseEnvelope>(batchResult))
        {
            throw std::runtime_error("Batched JMAP request failed");
        }
        std::cout << label.toStdString() << " batched (3 calls): " << timer.elapsed() << " ms\n";
    }
} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application{argc, argv};
    application.setApplicationName(QStringLiteral("Javelin Mail"));
    application.setOrganizationName(QStringLiteral("Javelin Mail"));
    if (application.arguments().size() != 2)
    {
        std::cerr << "Usage: jmap-transport-benchmark <account-display-name>\n";
        return 2;
    }
    try
    {
        const auto account = configuredAccount(application.arguments().at(1));
        const auto path =
            QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                .filePath(QStringLiteral("cache.sqlite3"));
        auto opened = javelin::jmap::cache::DatabaseConnection::open(
            {.connectionName = QStringLiteral("jmap-transport-benchmark"), .databasePath = path});
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&opened))
        {
            throw std::runtime_error(error->message.toStdString());
        }
        auto database = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
        QNetworkAccessManager network;
        javelin::jmap::api::QtNetworkTransport networkTransport{network};
        javelin::jmap::api::HttpJmapMethodTransport http{networkTransport};
        javelin::jmap::api::PreferredJmapMethodTransport preferred{database, http};
        auto task = [&]() -> QCoro::Task<void>
        {
            co_await runBenchmark(account, database, preferred, JmapTransportPolicy::ForceWebSocket,
                                  QStringLiteral("WebSocket"));
            co_await runBenchmark(account, database, http, JmapTransportPolicy::Preferred,
                                  QStringLiteral("HTTP"));
        }();
        QCoro::connect(std::move(task), &application, &QCoreApplication::quit);
        return application.exec();
    }
    catch (const std::exception& error)
    {
        std::cerr << "Benchmark setup failed: " << error.what() << '\n';
        return 1;
    }
}
