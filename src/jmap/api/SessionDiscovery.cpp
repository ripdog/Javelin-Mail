#include "jmap/api/SessionDiscovery.h"

#include <QCoroSignal>

#include <QDnsLookup>
#include <QRandomGenerator>

#include <algorithm>

namespace javelin::jmap::api
{
    namespace
    {
        [[nodiscard]] QUrl wellKnownUrl(QUrl base)
        {
            if (base.scheme().isEmpty())
            {
                base = QUrl{QStringLiteral("https://") + base.toString()};
            }
            if (base.path().isEmpty() || base.path() == QStringLiteral("/"))
            {
                base.setPath(QStringLiteral("/.well-known/jmap"));
            }
            return base;
        }
    } // namespace

    QCoro::Task<std::optional<QUrl>> discoverSessionUrl(std::string configuredServer,
                                                        std::string loginEmail)
    {
        if (!configuredServer.empty())
        {
            const auto url = wellKnownUrl(QUrl{QString::fromStdString(configuredServer)});
            co_return url.isValid() && url.scheme() == QStringLiteral("https") ? std::optional{url}
                                                                               : std::nullopt;
        }

        const auto email = QString::fromStdString(loginEmail);
        const auto separator = email.lastIndexOf(QLatin1Char{'@'});
        if (separator < 0 || separator + 1 >= email.size())
        {
            co_return std::nullopt;
        }
        const auto domain = email.mid(separator + 1);
        QDnsLookup lookup{QDnsLookup::SRV, QStringLiteral("_jmap._tcp.") + domain};
        lookup.lookup();
        co_await qCoro(&lookup, &QDnsLookup::finished);
        if (lookup.error() == QDnsLookup::NoError && !lookup.serviceRecords().empty())
        {
            auto records = lookup.serviceRecords();
            const auto minimumPriority =
                std::ranges::min(records, {}, &QDnsServiceRecord::priority).priority();
            records.erase(std::remove_if(records.begin(), records.end(),
                                         [minimumPriority](const auto& record)
                                         { return record.priority() != minimumPriority; }),
                          records.end());
            quint32 totalWeight = 0;
            for (const auto& record : records)
            {
                totalWeight += record.weight();
            }
            quint32 choice =
                totalWeight == 0 ? 0 : QRandomGenerator::global()->bounded(totalWeight);
            auto selected = records.cbegin();
            for (auto it = records.cbegin(); it != records.cend(); ++it)
            {
                if (choice < it->weight() || it + 1 == records.cend())
                {
                    selected = it;
                    break;
                }
                choice -= it->weight();
            }
            QUrl url;
            url.setScheme(QStringLiteral("https"));
            auto target = selected->target();
            if (target.endsWith(QLatin1Char{'.'}))
            {
                target.chop(1);
            }
            url.setHost(target);
            url.setPort(selected->port());
            url.setPath(QStringLiteral("/.well-known/jmap"));
            co_return url;
        }

        co_return wellKnownUrl(QUrl{QStringLiteral("https://") + domain});
    }
} // namespace javelin::jmap::api
