# Coroutine parameters are stored in the coroutine frame. Borrowed immutable
# inputs can outlive their caller as soon as the coroutine suspends, so public
# task APIs must take request data by value.
file(GLOB_RECURSE coroutine_headers
    "${PROJECT_SOURCE_DIR}/src/*.h"
)

foreach(header IN LISTS coroutine_headers)
    file(READ "${header}" contents)
    string(REGEX MATCHALL "QCoro::Task[^;]*;" task_declarations "${contents}")
    foreach(declaration IN LISTS task_declarations)
        if(declaration MATCHES
           "std::string_view|const[ \n\t]+[A-Za-z0-9_:<>]+&[ \n\t]+[A-Za-z_]")
            file(RELATIVE_PATH relative_header "${PROJECT_SOURCE_DIR}" "${header}")
            message(FATAL_ERROR
                "Borrowed immutable parameter in coroutine API ${relative_header}:\n"
                "${declaration}\n"
                "Pass suspend-lived request data by value. Mutable references are reserved "
                "for explicit lifetime/control services."
            )
        endif()
    endforeach()
endforeach()
