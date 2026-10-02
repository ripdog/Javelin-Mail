#include "desktop/notifications/DesktopAuthActionExecutor.h"

#include <KLocalizedString>

#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QUrl>
#include <QVariantMap>

#include <utility>

namespace javelin::app
{
    DesktopAuthActionExecutor::DesktopAuthActionExecutor(QObject* parent)
        : DesktopAuthActionExecutor(QDBusConnection::sessionBus(), parent)
    {
    }

    DesktopAuthActionExecutor::DesktopAuthActionExecutor(QDBusConnection connection,
                                                         QObject* parent)
        : QObject(parent), m_connection(std::move(connection))
    {
    }

    void DesktopAuthActionExecutor::copyCode(const QString& code)
    {
        if (code.isEmpty())
            return;
        auto message = QDBusMessage::createMethodCall(
            QStringLiteral("org.kde.klipper"), QStringLiteral("/klipper"),
            QStringLiteral("org.kde.klipper.klipper"), QStringLiteral("setClipboardContents"));
        message.setArguments({code});
        dispatch(message, i18n("Unable to copy authentication code"));
    }

    void DesktopAuthActionExecutor::openLink(const QString& url, const QString& activationToken)
    {
        const auto parsed = QUrl::fromEncoded(url.toUtf8(), QUrl::StrictMode);
        if (!parsed.isValid() || parsed.host().isEmpty() ||
            (parsed.scheme() != QStringLiteral("https") &&
             parsed.scheme() != QStringLiteral("http")))
        {
            Q_EMIT actionFailed(i18n("Unable to open authentication link"),
                                i18n("The authentication link is invalid."));
            return;
        }
        QVariantMap options;
        if (!activationToken.isEmpty())
            options.insert(QStringLiteral("activation_token"), activationToken);
        auto message = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.portal.Desktop"),
            QStringLiteral("/org/freedesktop/portal/desktop"),
            QStringLiteral("org.freedesktop.portal.OpenURI"), QStringLiteral("OpenURI"));
        message.setArguments({QString{}, QString::fromUtf8(parsed.toEncoded()), options});
        dispatch(message, i18n("Unable to open authentication link"));
    }

    void DesktopAuthActionExecutor::dispatch(const QDBusMessage& message,
                                             const QString& failureTitle)
    {
        auto* watcher = new QDBusPendingCallWatcher(m_connection.asyncCall(message), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this,
                [this, failureTitle](QDBusPendingCallWatcher* finished)
                {
                    const auto reply = finished->reply();
                    finished->deleteLater();
                    if (reply.type() == QDBusMessage::ErrorMessage)
                        Q_EMIT actionFailed(failureTitle, reply.errorMessage());
                });
    }
} // namespace javelin::app
