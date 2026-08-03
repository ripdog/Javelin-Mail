#include_guard(GLOBAL)

function(javelin_set_project_warnings target)
    if (MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8)
        return()
    endif()

    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Wformat=2
        -Wundef
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Woverloaded-virtual
        -Wnull-dereference
        -Wimplicit-fallthrough
        -Werror
        -Wno-missing-include-dirs
    )

    # GCC 14 reports false maybe-uninitialized diagnostics while moving Qt values
    # through std::optional/std::variant in optimized builds. Keep the warning
    # visible, but do not let that compiler version turn it into a release blocker.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 15)
        target_compile_options(${target} PRIVATE -Wno-error=maybe-uninitialized)
    endif()
endfunction()
