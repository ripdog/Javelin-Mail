add_library(javelin_protocol STATIC
    ProcessBoundary.cpp
    ProcessBoundary.h
    SocketTransport.cpp
    SocketTransport.h
)
target_include_directories(javelin_protocol PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)
target_link_libraries(javelin_protocol PUBLIC Qt::Core Qt::Network)
javelin_set_project_warnings(javelin_protocol)
javelin_enable_static_analysis(javelin_protocol)
javelin_enable_sanitizers(javelin_protocol)
