cmake_minimum_required(VERSION 3.19)
project(Javelin-Mail LANGUAGES CXX)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets LinguistTools)

qt_standard_project_setup()

qt_add_executable(Javelin-Mail
    WIN32 MACOSX_BUNDLE
    main.cpp
    emailview.cpp
    emailview.h
    emailview.ui
)

qt_add_translations(
    TARGETS Javelin-Mail
    TS_FILES Javelin-Mail_en_NZ.ts
)

target_link_libraries(Javelin-Mail
    PRIVATE
        Qt::Core
        Qt::Widgets
)

install(TARGETS Javelin-Mail
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_generate_deploy_app_script(
    TARGET Javelin-Mail
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})
