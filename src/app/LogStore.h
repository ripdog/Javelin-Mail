#pragma once

#include <QDateTime>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QVector>

namespace javelin::app
{
    struct LogEntry
    {
        QDateTime timestamp;
        QtMsgType level = QtInfoMsg;
        QString subsystem;
        QString message;
    };

    class LogStore final : public QObject
    {
        Q_OBJECT

      public:
        static LogStore& instance();
        static void install(qsizetype maximumEntries = 10000);

        [[nodiscard]] QVector<LogEntry> entries() const;
        void setMaximumEntries(qsizetype maximumEntries);
        void clear();
        void append(LogEntry entry);

      Q_SIGNALS:
        void entryAdded(const javelin::app::LogEntry& entry);
        void cleared();

      private:
        explicit LogStore(QObject* parent = nullptr);
        mutable QMutex m_mutex;
        QVector<LogEntry> m_entries;
        qsizetype m_maximumEntries = 10000;
    };

    class DaemonLogPort : public QObject
    {
        Q_OBJECT

      public:
        explicit DaemonLogPort(QObject* parent = nullptr) : QObject(parent)
        {
        }
        ~DaemonLogPort() override = default;

        [[nodiscard]] virtual QVector<LogEntry> entries() const = 0;
        virtual void acquire() = 0;
        virtual void release() = 0;
        virtual void clear() = 0;

      Q_SIGNALS:
        void entryAdded(const javelin::app::LogEntry& entry);
        void cleared();
    };
} // namespace javelin::app

Q_DECLARE_METATYPE(javelin::app::LogEntry)
