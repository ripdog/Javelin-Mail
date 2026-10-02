#pragma once

#include <QDBusConnection>
#include <QObject>
#include <QString>

namespace javelin::app
{
    class DesktopAuthActionExecutor final : public QObject
    {
        Q_OBJECT

      public:
        explicit DesktopAuthActionExecutor(QObject* parent = nullptr);
        DesktopAuthActionExecutor(QDBusConnection connection, QObject* parent = nullptr);

        void copyCode(const QString& code);
        void openLink(const QString& url, const QString& activationToken);

      Q_SIGNALS:
        void actionFailed(const QString& title, const QString& detail);

      private:
        void dispatch(const QDBusMessage& message, const QString& failureTitle);

        QDBusConnection m_connection;
    };
} // namespace javelin::app
