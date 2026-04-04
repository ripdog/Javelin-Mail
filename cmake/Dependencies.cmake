include_guard(GLOBAL)

include(FetchContent)

function(javelin_configure_dependencies)
    set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
    set(CATCH_INSTALL_EXTRAS ON CACHE BOOL "" FORCE)
    set(QCORO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(QCORO_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(QCORO_WITH_QTDBUS OFF CACHE BOOL "" FORCE)
    set(QCORO_WITH_QTNETWORK ON CACHE BOOL "" FORCE)
    set(QCORO_WITH_QTWEBSOCKETS OFF CACHE BOOL "" FORCE)
    set(USE_QT_VERSION 6 CACHE STRING "" FORCE)
    set(glaze_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(glaze_BUILD_TESTS OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.7.1
        GIT_SHALLOW TRUE
    )

    FetchContent_Declare(
        qcoro
        GIT_REPOSITORY https://github.com/qcoro/qcoro.git
        GIT_TAG v0.13.0
        GIT_SHALLOW TRUE
    )

    FetchContent_Declare(
        glaze
        GIT_REPOSITORY https://github.com/stephenberry/glaze.git
        GIT_TAG v2.9.5
        GIT_SHALLOW TRUE
    )

    FetchContent_MakeAvailable(Catch2 qcoro glaze)

    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
    set(CMAKE_MODULE_PATH "${CMAKE_MODULE_PATH}" PARENT_SCOPE)
endfunction()
