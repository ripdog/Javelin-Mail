include_guard(GLOBAL)

include(FetchContent)

function(javelin_fetch_catch2)
    set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
    set(CATCH_INSTALL_EXTRAS ON CACHE BOOL "" FORCE)

    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.7.1
        GIT_SHALLOW TRUE
    )

    FetchContent_MakeAvailable(Catch2)

    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
    set(CMAKE_MODULE_PATH "${CMAKE_MODULE_PATH}" PARENT_SCOPE)
endfunction()

function(javelin_fetch_qcoro)
    set(QCORO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(QCORO_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(QCORO_WITH_QTDBUS OFF CACHE BOOL "" FORCE)
    set(QCORO_WITH_QTNETWORK ON CACHE BOOL "" FORCE)
    set(QCORO_WITH_QTWEBSOCKETS OFF CACHE BOOL "" FORCE)
    set(QCORO_GENERATE_PRI_FILES OFF CACHE BOOL "" FORCE)
    set(USE_QT_VERSION 6 CACHE STRING "" FORCE)

    # Hide Extra CMake Modules from QCoro to prevent buggy PRI generation
    set(CMAKE_DISABLE_FIND_PACKAGE_ECM ON)
    set(ECM_FOUND OFF)

    FetchContent_Declare(
        qcoro
        GIT_REPOSITORY https://github.com/qcoro/qcoro.git
        GIT_TAG v0.13.0
        GIT_SHALLOW TRUE
    )

    FetchContent_MakeAvailable(qcoro)
endfunction()

function(javelin_fetch_glaze)
    set(glaze_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(glaze_BUILD_TESTS OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        glaze
        GIT_REPOSITORY https://github.com/stephenberry/glaze.git
        GIT_TAG v2.9.5
        GIT_SHALLOW TRUE
    )

    FetchContent_MakeAvailable(glaze)
endfunction()

function(javelin_configure_dependencies)
    find_package(QCoro6 CONFIG QUIET COMPONENTS Core Coro Network)
    if(NOT QCoro6_FOUND)
        javelin_fetch_qcoro()
    endif()

    find_package(glaze CONFIG QUIET)
    if(NOT glaze_FOUND)
        javelin_fetch_glaze()
    endif()

    if(BUILD_TESTING)
        find_package(Catch2 3 CONFIG QUIET)
        if(Catch2_FOUND)
            list(APPEND CMAKE_MODULE_PATH "${Catch2_DIR}")
            set(CMAKE_MODULE_PATH "${CMAKE_MODULE_PATH}" PARENT_SCOPE)
        else()
            javelin_fetch_catch2()
        endif()
    endif()
endfunction()
