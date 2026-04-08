#include "app/ApplicationBootstrap.h"
#include "app/WebEngineSetup.h"

#include <QApplication>
#include <QLocale>
#include <QTranslator>

#ifndef JAVELIN_DATA_DIR
#define JAVELIN_DATA_DIR ""
#endif

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
    QApplication application(argc, argv);
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
