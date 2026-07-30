#include <QApplication>

#include <catch2/catch_session.hpp>

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QPA_PLATFORMTHEME", "");
    qputenv("QT_STYLE_OVERRIDE", "Fusion");
    QApplication application{argc, argv};
    return Catch::Session{}.run(argc, argv);
}
