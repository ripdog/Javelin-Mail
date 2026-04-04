cmake_minimum_required(VERSION 3.19)
project(Javelin-Mail LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/Warnings.cmake)
include(cmake/Dependencies.cmake)
include(cmake/StaticAnalysis.cmake)
include(cmake/Sanitizers.cmake)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets LinguistTools)

qt_standard_project_setup()
javelin_configure_dependencies()
enable_testing()

set(JAVELIN_FORMAT_FILES
    src/app/ApplicationBootstrap.cpp
    src/app/ApplicationBootstrap.h
    src/app/main.cpp
    src/app/ProcessServices.cpp
    src/app/ProcessServices.h
    src/gui/shell/MainWindow.cpp
    src/gui/shell/MainWindow.h
    src/jmap/api/Error.cpp
    src/jmap/api/Error.h
    src/jmap/api/Session.cpp
    src/jmap/api/Session.h
    src/jmap/api/SessionParser.cpp
    src/jmap/api/SessionParser.h
    src/jmap/auth/Auth.cpp
    src/jmap/auth/Auth.h
    src/jmap/JmapCore.cpp
    src/jmap/JmapCore.h
    tests/FixtureReader.cpp
    tests/FixtureReader.h
    tests/jmap/AuthTest.cpp
    tests/jmap/JmapCoreTest.cpp
    tests/jmap/SessionTest.cpp
)

add_library(javelin_jmap STATIC
    src/jmap/api/Error.cpp
    src/jmap/api/Error.h
    src/jmap/api/Session.cpp
    src/jmap/api/Session.h
    src/jmap/api/SessionParser.cpp
    src/jmap/api/SessionParser.h
    src/jmap/auth/Auth.cpp
    src/jmap/auth/Auth.h
    src/jmap/JmapCore.cpp
    src/jmap/JmapCore.h
)
target_include_directories(javelin_jmap PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)
target_link_libraries(javelin_jmap
    PUBLIC
        Qt::Core
    PRIVATE
        glaze::glaze
)
javelin_set_project_warnings(javelin_jmap)
javelin_enable_static_analysis(javelin_jmap)
javelin_enable_sanitizers(javelin_jmap)

add_library(javelin_gui STATIC
    src/gui/shell/MainWindow.cpp
    src/gui/shell/MainWindow.h
)
target_include_directories(javelin_gui PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)
target_link_libraries(javelin_gui
    PUBLIC
        Qt::Core
        Qt::Widgets
    PRIVATE
        javelin_jmap
)
javelin_set_project_warnings(javelin_gui)
javelin_enable_static_analysis(javelin_gui)
javelin_enable_sanitizers(javelin_gui)

add_library(javelin_app STATIC
    src/app/ApplicationBootstrap.cpp
    src/app/ApplicationBootstrap.h
    src/app/ProcessServices.cpp
    src/app/ProcessServices.h
)
target_include_directories(javelin_app PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)
target_link_libraries(javelin_app
    PUBLIC
        Qt::Core
        Qt::Widgets
    PRIVATE
        javelin_gui
        javelin_jmap
)
javelin_set_project_warnings(javelin_app)
javelin_enable_static_analysis(javelin_app)
javelin_enable_sanitizers(javelin_app)

qt_add_executable(Javelin-Mail
    WIN32 MACOSX_BUNDLE
    src/app/main.cpp
)

qt_add_translations(
    TARGETS Javelin-Mail
    TS_FILES Javelin-Mail_en_NZ.ts
)

target_link_libraries(Javelin-Mail
    PRIVATE
        javelin_app
)
javelin_set_project_warnings(Javelin-Mail)
javelin_enable_static_analysis(Javelin-Mail)
javelin_enable_sanitizers(Javelin-Mail)

javelin_add_format_targets(${JAVELIN_FORMAT_FILES})

add_subdirectory(tests)

install(TARGETS Javelin-Mail
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_generate_deploy_app_script(
    TARGET Javelin-Mail
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})
