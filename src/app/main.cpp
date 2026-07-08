#include "app/ApplicationBootstrap.h"
#include "app/WebEngineSetup.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QLocale>
#include <QTimer>
#include <QTranslator>

#include <memory>

#ifndef JAVELIN_DATA_DIR
#define JAVELIN_DATA_DIR ""
#endif

namespace
{
    constexpr auto uiProfilingEnvVar = "JAVELIN_UI_PROFILING";

    [[nodiscard]] bool uiProfilingEnabled()
    {
        bool ok = false;
        const int value = qEnvironmentVariableIntValue(uiProfilingEnvVar, &ok);
        return ok && value != 0;
    }

    class UiStallProbe final : public QObject
    {
      public:
        explicit UiStallProbe(QObject* parent = nullptr) : QObject(parent)
        {
            m_timer.setTimerType(Qt::PreciseTimer);
            m_timer.setInterval(50);
            m_last.start();

            connect(&m_timer, &QTimer::timeout, this,
                    [this]
                    {
                        const qint64 elapsed = m_last.restart();
                        if (elapsed > 200)
                        {
                            qWarning().noquote() << "UI event loop stall:" << elapsed << "ms";
                        }
                    });

            m_timer.start();
        }

      private:
        QTimer m_timer;
        QElapsedTimer m_last;
    };

    class ProfilingApplication final : public QApplication
    {
      public:
        ProfilingApplication(int& argc, char** argv, const bool profilingEnabled)
            : QApplication(argc, argv), m_profilingEnabled(profilingEnabled)
        {
            if (m_profilingEnabled)
            {
                qInfo().noquote() << "UI profiling enabled via" << uiProfilingEnvVar;
            }
        }

        bool notify(QObject* receiver, QEvent* event) override
        {
            if (!m_profilingEnabled)
            {
                return QApplication::notify(receiver, event);
            }

            const char* receiverClass =
                receiver != nullptr ? receiver->metaObject()->className() : "<null>";
            const QString objectName =
                receiver != nullptr ? receiver->objectName() : QStringLiteral("<null>");
            const auto eventType = event != nullptr ? event->type() : QEvent::None;

            QElapsedTimer timer;
            timer.start();

            const bool result = QApplication::notify(receiver, event);

            const qint64 elapsed = timer.elapsed();
            if (elapsed > 50)
            {
                qWarning().noquote() << "Slow Qt event:" << elapsed << "ms"
                                     << "receiver=" << receiverClass << "objectName=" << objectName
                                     << "event=" << eventType;
            }

            return result;
        }

      private:
        bool m_profilingEnabled = false;
    };
} // namespace

int main(int argc, char* argv[])
{
    // Prepend the local install data directory to XDG_DATA_DIRS so KXMLGUI
    // can find .rc files during development without a system-wide install.
    constexpr auto localDataDir = JAVELIN_DATA_DIR;
    if constexpr (localDataDir[0] != '\0')
    {
        const char* existingDirs = getenv("XDG_DATA_DIRS");
        const QString dataDirs = existingDirs
                                     ? QString::fromLatin1(localDataDir) + QLatin1Char(':') +
                                           QString::fromLocal8Bit(existingDirs)
                                     : QString::fromLatin1(localDataDir);
        qputenv("XDG_DATA_DIRS", dataDirs.toUtf8());
    }

    javelin::app::registerInlineMessageUrlScheme();
    const bool profileUi = uiProfilingEnabled();
    ProfilingApplication application(argc, argv, profileUi);
    const auto stallProbe = profileUi ? std::make_unique<UiStallProbe>() : nullptr;
    application.setApplicationName(QStringLiteral("Javelin Mail"));
    application.setOrganizationName(QStringLiteral("Javelin Mail"));

    QTranslator translator;
    const QStringList uiLanguages = QLocale::system().uiLanguages();
    for (const QString& locale : uiLanguages)
    {
        const QString baseName = QStringLiteral("Javelin-Mail_") + QLocale(locale).name();
        if (translator.load(QStringLiteral(":/i18n/") + baseName))
        {
            application.installTranslator(&translator);
            break;
        }
    }

    javelin::app::ApplicationBootstrap bootstrap(application);
    return bootstrap.run();
}
