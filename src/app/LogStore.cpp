#include "app/LogStore.h"

#include <QCoreApplication>
#include <QDebug>
#include <QMutexLocker>
#include <QRegularExpression>
#include <cstdio>

namespace javelin::app
{
    namespace
    {
        constexpr qsizetype maximumEntries = 10000;
        QtMessageHandler previousHandler = nullptr;

        QString inferredSubsystem(const QMessageLogContext& context, const QString& message)
        {
            const QString category = QString::fromUtf8(context.category);
            if (category != QStringLiteral("default"))
            {
                return category;
            }
            const auto firstWord = message.section(QLatin1Char(' '), 0, 0).toLower();
            if (firstWord == QStringLiteral("jmap") || firstWord == QStringLiteral("mailbox"))
            {
                return QStringLiteral("jmap");
            }
            if (firstWord == QStringLiteral("long"))
            {
                return QStringLiteral("websocket");
            }
            if (firstWord == QStringLiteral("gui") || firstWord == QStringLiteral("ui"))
            {
                return QStringLiteral("gui");
            }
            if (firstWord == QStringLiteral("compose"))
            {
                return QStringLiteral("user");
            }
            return QStringLiteral("application");
        }

        void messageHandler(const QtMsgType type, const QMessageLogContext& context,
                            const QString& message)
        {
            const LogEntry entry{.timestamp = QDateTime::currentDateTime(),
                                 .level = type,
                                 .subsystem = inferredSubsystem(context, message),
                                 .message = message.simplified()};
            LogStore::instance().append(entry);

            const auto line = QStringLiteral("%1 [%2] [%3] %4\n")
                                  .arg(entry.timestamp.toString(Qt::ISODateWithMs),
                                       type == QtDebugMsg      ? QStringLiteral("DEBUG")
                                       : type == QtInfoMsg     ? QStringLiteral("INFO")
                                       : type == QtWarningMsg  ? QStringLiteral("WARN")
                                       : type == QtCriticalMsg ? QStringLiteral("ERROR")
                                                               : QStringLiteral("FATAL"),
                                       entry.subsystem, entry.message)
                                  .toLocal8Bit();
            std::fwrite(line.constData(), 1, static_cast<std::size_t>(line.size()), stderr);
            std::fflush(stderr);
            if (type == QtFatalMsg)
            {
                std::abort();
            }
        }
    } // namespace

    LogStore::LogStore(QObject* parent) : QObject(parent)
    {
    }

    LogStore& LogStore::instance()
    {
        static LogStore store;
        return store;
    }

    void LogStore::install()
    {
        qRegisterMetaType<LogEntry>();
        previousHandler = qInstallMessageHandler(messageHandler);
        Q_UNUSED(previousHandler)
    }

    QVector<LogEntry> LogStore::entries() const
    {
        const QMutexLocker lock{&m_mutex};
        return m_entries;
    }

    void LogStore::append(LogEntry entry)
    {
        {
            const QMutexLocker lock{&m_mutex};
            m_entries.push_back(entry);
            if (m_entries.size() > maximumEntries)
            {
                m_entries.remove(0, m_entries.size() - maximumEntries);
            }
        }
        Q_EMIT entryAdded(entry);
    }

    void LogStore::clear()
    {
        {
            const QMutexLocker lock{&m_mutex};
            m_entries.clear();
        }
        Q_EMIT cleared();
    }
} // namespace javelin::app
