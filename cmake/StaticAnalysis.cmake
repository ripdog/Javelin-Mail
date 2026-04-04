include_guard(GLOBAL)

option(JAVELIN_ENABLE_CLANG_TIDY "Run clang-tidy during compilation" OFF)
option(JAVELIN_ENABLE_CLAZY "Add per-target clazy targets when clazy-standalone is available" OFF)

function(javelin_enable_static_analysis target)
    if (JAVELIN_ENABLE_CLANG_TIDY)
        find_program(JAVELIN_CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
        set_target_properties(${target} PROPERTIES
            CXX_CLANG_TIDY
            "${JAVELIN_CLANG_TIDY_EXE};--config-file=${PROJECT_SOURCE_DIR}/.clang-tidy"
        )
    endif()

    if (NOT JAVELIN_ENABLE_CLAZY)
        return()
    endif()

    find_program(JAVELIN_CLAZY_EXE NAMES clazy-standalone REQUIRED)

    get_target_property(target_sources ${target} SOURCES)
    if (NOT target_sources)
        return()
    endif()

    list(FILTER target_sources INCLUDE REGEX [[\.(cc|cpp|cxx)$]])
    if (NOT target_sources)
        return()
    endif()

    set(clazy_compile_commands_dir "${CMAKE_BINARY_DIR}/clazy/${target}")

    add_custom_target(${target}_clazy
        COMMAND ${CMAKE_COMMAND}
            -DINPUT_FILE=${CMAKE_BINARY_DIR}/compile_commands.json
            -DOUTPUT_FILE=${clazy_compile_commands_dir}/compile_commands.json
            -P ${PROJECT_SOURCE_DIR}/cmake/PrepareClazyCompileCommands.cmake
        COMMAND ${JAVELIN_CLAZY_EXE}
            -p ${clazy_compile_commands_dir}
            ${target_sources}
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        COMMENT "Running clazy for ${target}"
        VERBATIM
        COMMAND_EXPAND_LISTS
    )
endfunction()

function(javelin_add_format_targets)
    find_program(JAVELIN_CLANG_FORMAT_EXE NAMES clang-format)
    if (NOT JAVELIN_CLANG_FORMAT_EXE)
        message(STATUS "clang-format not found; format targets will not be generated")
        return()
    endif()

    set(format_files ${ARGN})
    if (NOT format_files)
        return()
    endif()

    add_custom_target(format
        COMMAND ${JAVELIN_CLANG_FORMAT_EXE} -i ${format_files}
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        COMMENT "Formatting Javelin Mail C++ sources"
        VERBATIM
    )

    add_custom_target(format-check
        COMMAND ${JAVELIN_CLANG_FORMAT_EXE} --dry-run --Werror ${format_files}
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        COMMENT "Checking Javelin Mail C++ formatting"
        VERBATIM
    )
endfunction()
