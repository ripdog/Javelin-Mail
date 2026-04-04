add_executable(javelin_jmap_tests
    FixtureReader.cpp
    FixtureReader.h
    jmap/AuthTest.cpp
    jmap/JmapCoreTest.cpp
    jmap/MethodCallerTest.cpp
    jmap/MethodEnvelopeTest.cpp
    jmap/MailEntityParsersTest.cpp
    jmap/SessionClientTest.cpp
    jmap/SessionTest.cpp
)

target_include_directories(javelin_jmap_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/tests
)

target_link_libraries(javelin_jmap_tests
    PRIVATE
        Catch2::Catch2WithMain
        javelin_jmap
)
target_compile_definitions(javelin_jmap_tests
    PRIVATE
        JAVELIN_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
)

javelin_set_project_warnings(javelin_jmap_tests)
javelin_enable_static_analysis(javelin_jmap_tests)
javelin_enable_sanitizers(javelin_jmap_tests)

include(Catch)
catch_discover_tests(javelin_jmap_tests)
