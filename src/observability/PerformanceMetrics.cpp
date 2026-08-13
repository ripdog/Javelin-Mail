#include "app/PerformanceMetrics.h"

#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QStringList>

#include <algorithm>
#include <chrono>

#if defined(Q_OS_UNIX)
#include <sys/resource.h>
#endif

Q_LOGGING_CATEGORY(logPerformance, "javelin.performance")

namespace javelin::app
{
    namespace
    {
        [[nodiscard]] QString metricToken(QString value)
        {
            value.replace(QLatin1String(" "), QStringLiteral("_"));
            value.replace(QLatin1String("\t"), QStringLiteral("_"));
            value.replace(QLatin1String("\r"), QStringLiteral("_"));
            value.replace(QLatin1String("\n"), QStringLiteral("_"));
            value.replace(QLatin1String("="), QStringLiteral("_"));
            return value;
        }

        [[nodiscard]] QString quotedMetricValue(QString value)
        {
            value.replace(QLatin1String("\\"), QStringLiteral("\\\\"));
            value.replace(QLatin1String("\""), QStringLiteral("\\\""));
            value.replace(QLatin1String("\r"), QStringLiteral("\\r"));
            value.replace(QLatin1String("\n"), QStringLiteral("\\n"));
            return QStringLiteral("\"") + value + QStringLiteral("\"");
        }

        [[nodiscard]] std::optional<qint64> currentResidentSetKiB()
        {
            QFile statusFile{QStringLiteral("/proc/self/status")};
            if (!statusFile.open(QIODevice::ReadOnly))
                return std::nullopt;

            while (!statusFile.atEnd())
            {
                const auto line = statusFile.readLine().simplified();
                if (!line.startsWith(QByteArrayLiteral("VmRSS:")))
                    continue;
                const auto fields = line.mid(line.indexOf(':') + 1).trimmed().split(' ');
                if (fields.isEmpty())
                    return std::nullopt;
                bool ok = false;
                const auto value = fields.front().toLongLong(&ok);
                return ok ? std::optional<qint64>{value} : std::nullopt;
            }
            return std::nullopt;
        }

#if defined(Q_OS_UNIX)
        [[nodiscard]] qint64 timeValueMicroseconds(const timeval& value)
        {
            return static_cast<qint64>(value.tv_sec) * 1'000'000 +
                   static_cast<qint64>(value.tv_usec);
        }
#endif
    } // namespace

    bool PerformanceMetrics::enabled()
    {
        static const bool value = []
        {
            bool ok = false;
            const int configured = qEnvironmentVariableIntValue("JAVELIN_UI_PROFILING", &ok);
            return ok && configured > 0;
        }();
        return value;
    }

    void PerformanceMetrics::recordDuration(QString process, QString operation,
                                            const std::chrono::microseconds duration,
                                            QString outcome, QString details)
    {
        if (!enabled())
            return;
        qCInfo(logPerformance).noquote()
            << formatMetric(std::move(process), std::move(operation), duration, std::move(outcome),
                            std::move(details));
    }

    void PerformanceMetrics::recordEvent(QString process, QString operation, QString outcome,
                                         QString details)
    {
        if (!enabled())
            return;
        qCInfo(logPerformance).noquote()
            << formatMetric(std::move(process), std::move(operation), std::nullopt,
                            std::move(outcome), std::move(details));
    }

    void PerformanceMetrics::recordProcessResources(QString process, QString databasePath)
    {
        if (!enabled())
            return;

        QStringList details;
        if (const auto residentSet = currentResidentSetKiB(); residentSet.has_value())
            details.push_back(QStringLiteral("rss_kib=%1").arg(*residentSet));

        const auto maximumResidentSet = [&]() -> std::optional<qint64>
        {
#if defined(Q_OS_UNIX)
            struct rusage usage{};
            if (getrusage(RUSAGE_SELF, &usage) != 0)
                return std::nullopt;
#if defined(Q_OS_MACOS)
            return static_cast<qint64>(usage.ru_maxrss) / 1024;
#else
            return static_cast<qint64>(usage.ru_maxrss);
#endif
#else
            return std::nullopt;
#endif
        }();
        if (maximumResidentSet.has_value())
            details.push_back(QStringLiteral("max_rss_kib=%1").arg(*maximumResidentSet));

#if defined(Q_OS_UNIX)
        struct rusage usage{};
        if (getrusage(RUSAGE_SELF, &usage) == 0)
        {
            details.push_back(
                QStringLiteral("user_cpu_us=%1").arg(timeValueMicroseconds(usage.ru_utime)));
            details.push_back(
                QStringLiteral("system_cpu_us=%1").arg(timeValueMicroseconds(usage.ru_stime)));
        }
#endif

        if (!databasePath.isEmpty())
        {
            const auto walPath = databasePath + QStringLiteral("-wal");
            const auto walBytes = QFileInfo{walPath}.exists() ? QFileInfo{walPath}.size() : 0;
            details.push_back(QStringLiteral("wal_bytes=%1").arg(walBytes));
        }
        if (details.isEmpty())
            details.push_back(QStringLiteral("sample_unavailable=true"));

        recordEvent(std::move(process), QStringLiteral("process_resources"),
                    QStringLiteral("sample"), details.join(QLatin1Char(' ')));
    }

    QString
    PerformanceMetrics::formatMetric(QString process, QString operation,
                                     const std::optional<std::chrono::microseconds> duration,
                                     QString outcome, QString details)
    {
        QString result =
            QStringLiteral("metric process=%1 operation=%2")
                .arg(metricToken(std::move(process)), metricToken(std::move(operation)));
        if (!outcome.isEmpty())
            result += QStringLiteral(" outcome=%1").arg(metricToken(std::move(outcome)));
        if (duration.has_value())
        {
            const auto microseconds = std::max<qint64>(0, duration->count());
            result += QStringLiteral(" duration_us=%1").arg(microseconds);
        }
        if (!details.isEmpty())
            result += QStringLiteral(" details=%1").arg(quotedMetricValue(std::move(details)));
        return result;
    }

    QString PerformanceMetrics::remoteActionName(QString source)
    {
        if (source.isEmpty())
            return QStringLiteral("unknown");
        QString result;
        result.reserve(source.size() + 8);
        for (qsizetype index = 0; index < source.size(); ++index)
        {
            const auto character = source[index];
            if (character.isUpper() && index > 0 && source[index - 1].isLower())
                result += QLatin1Char('_');
            result += character.toLower();
        }
        result.replace(QStringLiteral("_o_auth"), QStringLiteral("_oauth"));
        return result;
    }

    PerformanceSpan::PerformanceSpan(QString process, QString operation, QString details)
        : m_process(std::move(process)), m_operation(std::move(operation)),
          m_details(std::move(details)), m_startedAt(std::chrono::steady_clock::now()),
          m_enabled(PerformanceMetrics::enabled())
    {
    }

    PerformanceSpan::~PerformanceSpan()
    {
        finish();
    }

    void PerformanceSpan::finish(QString outcome, QString details)
    {
        if (m_finished)
            return;
        m_finished = true;
        if (!m_enabled)
            return;

        if (!m_details.isEmpty() && !details.isEmpty())
            m_details += QLatin1Char(' ');
        m_details += std::move(details);
        PerformanceMetrics::recordDuration(std::move(m_process), std::move(m_operation),
                                           std::chrono::duration_cast<std::chrono::microseconds>(
                                               std::chrono::steady_clock::now() - m_startedAt),
                                           std::move(outcome), std::move(m_details));
    }
} // namespace javelin::app
