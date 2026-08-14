add_library(javelin_protocol STATIC
    ProtocolValidation.cpp
    ProtocolValidation.h
    ProtocolTypes.h
    ActionContract.h
    ActivationContract.h
    BoundaryEventContract.h
    CacheContract.h
    DaemonRequestHandler.h
    HandshakeContract.h
    SettingsContract.h
    LocalActivationClient.cpp
    LocalActivationClient.h
    LocalActivationServer.cpp
    LocalActivationServer.h
    LocalDaemonClient.cpp
    LocalDaemonClient.h
    LocalDaemonServer.cpp
    LocalDaemonServer.h
    SocketFrameCodec.cpp
    SocketFrameCodec.h
    SocketSecurity.cpp
    SocketSecurity.h
    SocketWireCodec.cpp
    SocketWireCodecInternal.h
)
target_include_directories(javelin_protocol PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)
target_link_libraries(javelin_protocol PUBLIC Qt::Core Qt::Network)
javelin_set_project_warnings(javelin_protocol)
javelin_enable_static_analysis(javelin_protocol)
javelin_enable_sanitizers(javelin_protocol)
