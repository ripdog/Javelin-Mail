if(NOT DEFINED JAVELIN_SOURCE_DIR)
    message(FATAL_ERROR "JAVELIN_SOURCE_DIR is required")
endif()
if(NOT DEFINED JAVELIN_TRANSLATION_DOMAIN)
    message(FATAL_ERROR "JAVELIN_TRANSLATION_DOMAIN is required")
endif()
if(NOT DEFINED JAVELIN_TRANSLATION_OUTPUT)
    message(FATAL_ERROR "JAVELIN_TRANSLATION_OUTPUT is required")
endif()

find_program(XGETTEXT_EXECUTABLE xgettext REQUIRED)

file(GLOB_RECURSE javelin_translation_sources LIST_DIRECTORIES false
    "${JAVELIN_SOURCE_DIR}/src/*.cpp"
    "${JAVELIN_SOURCE_DIR}/src/*.h"
)
list(SORT javelin_translation_sources)

# KXMLGUI resources are loaded at runtime, so expose their text to xgettext in
# the same dummy-call form produced by KDE's extractrc utility.
file(READ "${JAVELIN_SOURCE_DIR}/res/javelinmailui.rc" javelin_xmlgui)
# Protect XML entities containing semicolons before CMake turns regex matches
# into a list, then restore the mnemonic marker in each extracted string.
string(REPLACE "&amp;" "__JAVELIN_AMP__" javelin_xmlgui "${javelin_xmlgui}")
string(REGEX MATCHALL "<text>[^<]*</text>" javelin_xmlgui_messages "${javelin_xmlgui}")
set(javelin_rc_cpp
    "i18nc(\"NAME OF TRANSLATORS\", \"Your names\");\n"
    "i18nc(\"EMAIL OF TRANSLATORS\", \"Your emails\");\n")
foreach(javelin_xmlgui_message IN LISTS javelin_xmlgui_messages)
    string(REGEX REPLACE "^<text>|</text>$" "" javelin_xmlgui_message
           "${javelin_xmlgui_message}")
    string(REPLACE "__JAVELIN_AMP__" "&" javelin_xmlgui_message
           "${javelin_xmlgui_message}")
    string(REPLACE "\\" "\\\\" javelin_xmlgui_message "${javelin_xmlgui_message}")
    string(REPLACE "\"" "\\\"" javelin_xmlgui_message "${javelin_xmlgui_message}")
    string(APPEND javelin_rc_cpp "i18n(\"${javelin_xmlgui_message}\");\n")
endforeach()

set(javelin_rc_cpp_path "${CMAKE_CURRENT_BINARY_DIR}/javelin-i18n-rc.cpp")
file(WRITE "${javelin_rc_cpp_path}" "${javelin_rc_cpp}")
file(MAKE_DIRECTORY "${JAVELIN_SOURCE_DIR}/po")

execute_process(
    COMMAND "${XGETTEXT_EXECUTABLE}"
        --c++
        --kde
        --from-code=UTF-8
        --add-comments=i18n
        --keyword=i18n:1
        --keyword=i18nc:1c,2
        --keyword=i18np:1,2
        --keyword=i18ncp:1c,2,3
        --keyword=ki18n:1
        --keyword=ki18nc:1c,2
        --keyword=ki18np:1,2
        --keyword=ki18ncp:1c,2,3
        --keyword=kli18n:1
        --keyword=kli18nc:1c,2
        --keyword=kli18np:1,2
        --keyword=kli18ncp:1c,2,3
        --keyword=I18N_NOOP:1
        --keyword=I18NC_NOOP:1c,2
        --package-name=Javelin-Mail
        --msgid-bugs-address=https://github.com/ripdog/Javelin-Mail/issues
        --sort-by-file
        --output
        "${JAVELIN_TRANSLATION_OUTPUT}"
        ${javelin_translation_sources}
        "${javelin_rc_cpp_path}"
    RESULT_VARIABLE javelin_xgettext_result
    ERROR_VARIABLE javelin_xgettext_error
)
file(REMOVE "${javelin_rc_cpp_path}")

if(NOT javelin_xgettext_result EQUAL 0)
    message(FATAL_ERROR "xgettext failed: ${javelin_xgettext_error}")
endif()

message(STATUS
    "Extracted ${JAVELIN_TRANSLATION_DOMAIN} messages to ${JAVELIN_TRANSLATION_OUTPUT}")
