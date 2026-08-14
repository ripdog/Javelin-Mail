#pragma once

#include <QString>

#include <chrono>
#include <optional>

namespace javelin::app
{
    class PerformanceMetrics final
    {
      public:
        [[nodiscard]] static bool enabled();

        static void recordDuration(QString process, QString operation,
                                   std::chrono::microseconds duration, QString outcome = {},
                                   QString details = {});
        static void recordEvent(QString process, QString operation, QString outcome = {},
                                QString details = {});
        static void recordProcessResources(QString process, QString databasePath = {});

        [[nodiscard]] static QString formatMetric(QString process, QString operation,
                                                  std::optional<std::chrono::microseconds> duration,
                                                  QString outcome, QString details);
        [[nodiscard]] static QString remoteActionName(QString actionName);
    };

    class PerformanceSpan final
    {
      public:
        PerformanceSpan(QString process, QString operation, QString details = {});
        ~PerformanceSpan();

        PerformanceSpan(const PerformanceSpan&) = delete;
        PerformanceSpan& operator=(const PerformanceSpan&) = delete;
        PerformanceSpan(PerformanceSpan&&) = delete;
        PerformanceSpan& operator=(PerformanceSpan&&) = delete;

        void finish(QString outcome = QStringLiteral("finished"), QString details = {});

      private:
        QString m_process;
        QString m_operation;
        QString m_details;
        std::chrono::steady_clock::time_point m_startedAt;
        bool m_enabled = false;
        bool m_finished = false;
    };
} // namespace javelin::app
