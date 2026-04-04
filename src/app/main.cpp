#include "app/ApplicationBootstrap.h"
#include "app/WebEngineSetup.h"

#include <QApplication>
#include <QLocale>
#include <QTranslator>

int main(int argc, char* argv[])
{
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
