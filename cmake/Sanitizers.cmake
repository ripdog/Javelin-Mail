include_guard(GLOBAL)

option(JAVELIN_ENABLE_ASAN "Enable AddressSanitizer for supported toolchains" OFF)
option(JAVELIN_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer for supported toolchains" OFF)

function(javelin_enable_sanitizers target)
    if (MSVC)
        return()
    endif()

    set(sanitizer_flags)

    if (JAVELIN_ENABLE_ASAN)
        list(APPEND sanitizer_flags -fsanitize=address -fno-omit-frame-pointer)
    endif()

    if (JAVELIN_ENABLE_UBSAN)
        list(APPEND sanitizer_flags -fsanitize=undefined -fno-omit-frame-pointer)
    endif()

    if (NOT sanitizer_flags)
        return()
    endif()

    target_compile_options(${target} PRIVATE ${sanitizer_flags})
    target_link_options(${target} PRIVATE ${sanitizer_flags})
endfunction()
