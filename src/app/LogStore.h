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
        static void install();

        [[nodiscard]] QVector<LogEntry> entries() const;
        void clear();
        void append(LogEntry entry);

      Q_SIGNALS:
        void entryAdded(const javelin::app::LogEntry& entry);
        void cleared();

      private:
        explicit LogStore(QObject* parent = nullptr);
        mutable QMutex m_mutex;
        QVector<LogEntry> m_entries;
    };
} // namespace javelin::app

Q_DECLARE_METATYPE(javelin::app::LogEntry)
