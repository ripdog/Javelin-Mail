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

#include <array>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <ranges>
#include <stdexcept>
#include <vector>

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

    struct Measurements
    {
        qint64 mailboxGetMs = 0;
        qint64 emailQueryMs = 0;
        qint64 emailContentGetMs = 0;
        qint64 sequentialTotalMs = 0;
        qint64 batchedMs = 0;
    };

    struct Summary
    {
        double averageMs = 0.0;
        qint64 bestMs = 0;
        qint64 worstMs = 0;
    };

    [[nodiscard]] Account configuredAccount(const QString& displayName)
    {
        QSettings settings{QStringLiteral("Javelin Mail"), QStringLiteral("javelinmail")};
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

    [[nodiscard]] QCoro::Task<Measurements>
    runBenchmark(const Account account, javelin::jmap::cache::DatabaseConnection& database,
                 JmapMethodTransport& transport, const JmapTransportPolicy policy)
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
        Measurements measurements;
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
        measurements.mailboxGetMs = timer.restart();
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
                                              .anchor = std::nullopt,
                                              .anchorOffset = 0,
                                              .limit = 1,
                                              .collapseThreads = false,
                                              .calculateTotal = false});
        const auto queryHandle = queryBuilder.call(*queryRequest);
        const auto queryResponse = read(co_await caller.call(context, queryBuilder), queryHandle);
        measurements.emailQueryMs = timer.restart();
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
        measurements.emailContentGetMs = timer.restart();
        measurements.sequentialTotalMs = total.elapsed();

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
        measurements.batchedMs = timer.elapsed();
        co_return measurements;
    }

    [[nodiscard]] Summary summarize(const std::vector<Measurements>& runs,
                                    const qint64 Measurements::* member)
    {
        const auto [best, worst] = std::ranges::minmax_element(
            runs, {}, [member](const Measurements& measurements) { return measurements.*member; });
        const auto total =
            std::accumulate(runs.begin(), runs.end(), qint64{0},
                            [member](const qint64 sum, const Measurements& measurements)
                            { return sum + measurements.*member; });
        return {.averageMs = static_cast<double>(total) / static_cast<double>(runs.size()),
                .bestMs = (*best).*member,
                .worstMs = (*worst).*member};
    }

    void printTransportReport(const std::string_view name, const std::vector<Measurements>& runs)
    {
        struct Row
        {
            std::string_view name;
            qint64 Measurements::* member;
        };
        constexpr std::array rows{
            Row{"Mailbox/get", &Measurements::mailboxGetMs},
            Row{"Email/query", &Measurements::emailQueryMs},
            Row{"Email/get content", &Measurements::emailContentGetMs},
            Row{"Sequential total", &Measurements::sequentialTotalMs},
            Row{"Batched (3 calls)", &Measurements::batchedMs},
        };

        std::cout << '\n' << name << "\n";
        std::cout << std::left << std::setw(22) << "Operation" << std::right << std::setw(12)
                  << "Average" << std::setw(10) << "Best" << std::setw(10) << "Worst" << '\n';
        for (const auto& row : rows)
        {
            const auto summary = summarize(runs, row.member);
            std::cout << std::left << std::setw(22) << row.name << std::right << std::fixed
                      << std::setprecision(1) << std::setw(9) << summary.averageMs << " ms"
                      << std::setw(7) << summary.bestMs << " ms" << std::setw(7) << summary.worstMs
                      << " ms\n";
        }
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
            constexpr int measuredRunCount = 3;
            static_cast<void>(co_await runBenchmark(account, database, preferred,
                                                    JmapTransportPolicy::ForceWebSocket));
            static_cast<void>(
                co_await runBenchmark(account, database, http, JmapTransportPolicy::Preferred));

            std::vector<Measurements> webSocketRuns;
            std::vector<Measurements> httpRuns;
            webSocketRuns.reserve(measuredRunCount);
            httpRuns.reserve(measuredRunCount);
            for (int run = 0; run < measuredRunCount; ++run)
            {
                webSocketRuns.push_back(co_await runBenchmark(account, database, preferred,
                                                              JmapTransportPolicy::ForceWebSocket));
                httpRuns.push_back(
                    co_await runBenchmark(account, database, http, JmapTransportPolicy::Preferred));
            }

            std::cout << "JMAP transport benchmark: " << account.displayName.toStdString()
                      << "\n1 warmup run (discarded), " << measuredRunCount << " measured runs\n";
            printTransportReport("WebSocket", webSocketRuns);
            printTransportReport("HTTP", httpRuns);
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
