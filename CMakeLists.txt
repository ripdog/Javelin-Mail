cmake_minimum_required(VERSION 3.19)
project(Javelin-Mail LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/Warnings.cmake)
include(cmake/Dependencies.cmake)
include(cmake/StaticAnalysis.cmake)
include(cmake/Sanitizers.cmake)

# 1. Find Qt6 FIRST
find_package(Qt6 6.6 REQUIRED COMPONENTS Core Network Sql Widgets Concurrent LinguistTools WebEngineCore WebEngineWidgets Svg)

# 2. Fetch Dependencies BEFORE KDE is allowed to load
javelin_configure_dependencies()
qcoro_enable_coroutines()

# 3. NOW it is safe to load ECM and KDE Frameworks
set(KF_MIN_VERSION "6.0.0")
find_package(ECM ${KF_MIN_VERSION} REQUIRED NO_MODULE)
list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(FeatureSummary)

find_package(KF6 REQUIRED COMPONENTS XmlGui CoreAddons)

qt_standard_project_setup()
enable_testing()

set(JAVELIN_FORMAT_FILES
    src/app/ApplicationBootstrap.cpp
    src/app/ApplicationBootstrap.h
    src/app/InlineMessageSchemeHandler.cpp
    src/app/InlineMessageSchemeHandler.h
    src/app/WebEngineSetup.h
    src/app/main.cpp
    src/app/ProcessServices.cpp
    src/app/ProcessServices.h
    src/gui/mailboxes/MailboxTreeModel.cpp
    src/gui/mailboxes/MailboxTreeModel.h
    src/gui/messages/MessageListModel.cpp
    src/gui/messages/MessageListModel.h
    src/gui/messages/MessageListDelegate.cpp
    src/gui/messages/MessageListDelegate.h
    src/gui/messages/MessageListDelegate.cpp
    src/gui/messages/MessageListDelegate.h
    src/gui/messageview/HtmlMessageView.cpp
    src/gui/messageview/HtmlMessageView.h
    src/gui/messageview/MessageViewContainer.cpp
    src/gui/messageview/MessageViewContainer.h
    src/gui/settings/PreferencesDialog.cpp
    src/gui/settings/PreferencesDialog.h
    src/gui/shell/MainWindow.cpp
    src/gui/shell/MainWindow.h
    src/jmap/api/Error.cpp
    src/jmap/api/Error.h
    src/jmap/api/MailMethods.cpp
    src/jmap/api/MailMethods.h
    src/jmap/api/MethodCaller.cpp
    src/jmap/api/MethodCaller.h
    src/jmap/api/MethodEnvelope.cpp
    src/jmap/api/MethodEnvelope.h
    src/jmap/api/Session.cpp
    src/jmap/api/Session.h
    src/jmap/api/SessionClient.cpp
    src/jmap/api/SessionClient.h
    src/jmap/api/SessionParser.cpp
    src/jmap/api/SessionParser.h
    src/jmap/api/Transport.cpp
    src/jmap/api/Transport.h
    src/jmap/cache/AccountRepository.cpp
    src/jmap/cache/AccountRepository.h
    src/jmap/cache/Database.cpp
    src/jmap/cache/Database.h
    src/jmap/cache/EmailRepository.cpp
    src/jmap/cache/EmailRepository.h
    src/jmap/cache/InlinePartPayloadRepository.cpp
    src/jmap/cache/InlinePartPayloadRepository.h
    src/jmap/cache/MailboxRepository.cpp
    src/jmap/cache/MailboxRepository.h
    src/jmap/cache/MessageContentRepository.cpp
    src/jmap/cache/MessageContentRepository.h
    src/jmap/cache/MessageViewService.cpp
    src/jmap/cache/MessageViewService.h
    src/jmap/cache/QueryService.cpp
    src/jmap/cache/QueryService.h
    src/jmap/cache/SessionRepository.cpp
    src/jmap/cache/SessionRepository.h
    src/jmap/cache/SyncStateRepository.cpp
    src/jmap/cache/SyncStateRepository.h
    src/jmap/cache/ThreadRepository.cpp
    src/jmap/cache/ThreadRepository.h
    src/jmap/domain/MailEntities.h
    src/jmap/domain/MailEntityParsers.cpp
    src/jmap/domain/MailEntityParsers.h
    src/jmap/query/MessageGrouping.cpp
    src/jmap/query/MessageGrouping.h
    src/jmap/query/QueryDiff.cpp
    src/jmap/query/QueryDiff.h
    src/jmap/render/HtmlMessageDocumentBuilder.cpp
    src/jmap/render/HtmlMessageDocumentBuilder.h
    src/jmap/render/InlineMessageUrl.cpp
    src/jmap/render/InlineMessageUrl.h
    src/jmap/sync/LongPollWorker.cpp
    src/jmap/sync/LongPollWorker.h
    src/jmap/sync/PendingActions.cpp
    src/jmap/sync/PendingActions.h
    src/jmap/sync/SyncPlanner.cpp
    src/jmap/sync/SyncPlanner.h
    src/jmap/sync/SyncReconciler.cpp
    src/jmap/sync/SyncReconciler.h
    src/jmap/auth/AccessTokenResolver.cpp
    src/jmap/auth/AccessTokenResolver.h
    src/jmap/auth/Auth.cpp
    src/jmap/auth/Auth.h
    src/jmap/JmapCore.cpp
    src/jmap/JmapCore.h
    tests/FixtureReader.cpp
    tests/FixtureReader.h
    tests/jmap/AccountRepositoryTest.cpp
    tests/jmap/AuthTest.cpp
    tests/jmap/DatabaseTest.cpp
    tests/jmap/EmailRepositoryTest.cpp
    tests/jmap/HtmlMessageDocumentBuilderTest.cpp
    tests/jmap/InlinePartPayloadRepositoryTest.cpp
    tests/jmap/InlineMessageUrlTest.cpp
    tests/jmap/JmapCoreMessageContentTest.cpp
    tests/jmap/JmapCoreTest.cpp
    tests/jmap/LongPollWorkerTest.cpp
    tests/jmap/MailMethodsTest.cpp
    tests/jmap/MailboxRepositoryTest.cpp
    tests/jmap/MessageGroupingTest.cpp
    tests/jmap/MessageContentRepositoryTest.cpp
    tests/jmap/MessageViewServiceTest.cpp
    tests/jmap/MethodCallerTest.cpp
    tests/jmap/MethodEnvelopeTest.cpp
    tests/jmap/PendingActionsTest.cpp
    tests/jmap/QueryDiffTest.cpp
    tests/jmap/QueryServiceTest.cpp
    tests/jmap/MailEntityParsersTest.cpp
    tests/jmap/SessionClientTest.cpp
    tests/jmap/SessionRepositoryTest.cpp
    tests/jmap/SessionTest.cpp
    tests/jmap/SyncStateRepositoryTest.cpp
    tests/jmap/SyncPlannerTest.cpp
    tests/jmap/SyncReconcilerTest.cpp
    tests/jmap/ThreadRepositoryTest.cpp
)

add_library(javelin_jmap STATIC
    src/jmap/api/Error.cpp
    src/jmap/api/Error.h
    src/jmap/api/MailMethods.cpp
    src/jmap/api/MailMethods.h
    src/jmap/api/MethodCaller.cpp
    src/jmap/api/MethodCaller.h
    src/jmap/api/MethodEnvelope.cpp
    src/jmap/api/MethodEnvelope.h
    src/jmap/api/Session.cpp
    src/jmap/api/Session.h
    src/jmap/api/SessionClient.cpp
    src/jmap/api/SessionClient.h
    src/jmap/api/SessionParser.cpp
    src/jmap/api/SessionParser.h
    src/jmap/api/Transport.cpp
    src/jmap/api/Transport.h
    src/jmap/cache/AccountRepository.cpp
    src/jmap/cache/AccountRepository.h
    src/jmap/cache/Database.cpp
    src/jmap/cache/Database.h
    src/jmap/cache/EmailRepository.cpp
    src/jmap/cache/EmailRepository.h
    src/jmap/cache/InlinePartPayloadRepository.cpp
    src/jmap/cache/InlinePartPayloadRepository.h
    src/jmap/cache/MailboxRepository.cpp
    src/jmap/cache/MailboxRepository.h
    src/jmap/cache/MessageContentRepository.cpp
    src/jmap/cache/MessageContentRepository.h
    src/jmap/cache/MessageViewService.cpp
    src/jmap/cache/MessageViewService.h
    src/jmap/cache/QueryService.cpp
    src/jmap/cache/QueryService.h
    src/jmap/cache/SessionRepository.cpp
    src/jmap/cache/SessionRepository.h
    src/jmap/cache/SyncStateRepository.cpp
    src/jmap/cache/SyncStateRepository.h
    src/jmap/cache/ThreadRepository.cpp
    src/jmap/cache/ThreadRepository.h
    src/jmap/domain/MailEntities.h
    src/jmap/domain/MailEntityParsers.cpp
    src/jmap/domain/MailEntityParsers.h
    src/jmap/query/MessageGrouping.cpp
    src/jmap/query/MessageGrouping.h
    src/jmap/query/QueryDiff.cpp
    src/jmap/query/QueryDiff.h
    src/jmap/render/HtmlMessageDocumentBuilder.cpp
    src/jmap/render/HtmlMessageDocumentBuilder.h
    src/jmap/render/InlineMessageUrl.cpp
    src/jmap/render/InlineMessageUrl.h
    src/jmap/sync/LongPollWorker.cpp
    src/jmap/sync/LongPollWorker.h
    src/jmap/sync/PendingActions.cpp
    src/jmap/sync/PendingActions.h
    src/jmap/sync/SyncPlanner.cpp
    src/jmap/sync/SyncPlanner.h
    src/jmap/sync/SyncReconciler.cpp
    src/jmap/sync/SyncReconciler.h
    src/jmap/auth/AccessTokenResolver.cpp
    src/jmap/auth/AccessTokenResolver.h
    src/jmap/auth/Auth.cpp
    src/jmap/auth/Auth.h
    src/jmap/JmapCore.cpp
    src/jmap/JmapCore.h
)
target_include_directories(javelin_jmap PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)
target_link_libraries(javelin_jmap
    PUBLIC
        QCoro6::Coro
        Qt::Core
        Qt::Network
        Qt::Sql
        Qt::WebEngineCore
    PRIVATE
        QCoro6::Network
        glaze::glaze
)
javelin_set_project_warnings(javelin_jmap)
javelin_enable_static_analysis(javelin_jmap)
javelin_enable_sanitizers(javelin_jmap)

add_library(javelin_gui STATIC
    src/gui/mailboxes/MailboxTreeModel.cpp
    src/gui/mailboxes/MailboxTreeModel.h
    src/gui/messages/MessageListModel.cpp
    src/gui/messages/MessageListModel.h
    src/gui/messages/MessageListDelegate.cpp
    src/gui/messages/MessageListDelegate.h
    src/gui/messageview/HtmlMessageView.cpp
    src/gui/messageview/HtmlMessageView.h
    src/gui/messageview/MessageViewContainer.cpp
    src/gui/messageview/MessageViewContainer.h
    src/gui/settings/PreferencesDialog.cpp
    src/gui/settings/PreferencesDialog.h
    src/gui/shell/MainWindow.cpp
    src/gui/shell/MainWindow.h
)
target_include_directories(javelin_gui PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)
target_link_libraries(javelin_gui
    PUBLIC
        Qt::Core
        Qt::Widgets
        Qt::WebEngineWidgets
        KF6::XmlGui
    PRIVATE
        Qt::Concurrent
        javelin_jmap
)
javelin_set_project_warnings(javelin_gui)
javelin_enable_static_analysis(javelin_gui)
javelin_enable_sanitizers(javelin_gui)
# QCoro uses exceptions internally
target_compile_options(javelin_gui PRIVATE -fexceptions)

add_library(javelin_app STATIC
    src/app/ApplicationBootstrap.cpp
    src/app/ApplicationBootstrap.h
    src/app/InlineMessageSchemeHandler.cpp
    src/app/InlineMessageSchemeHandler.h
    src/app/WebEngineSetup.h
    src/app/ProcessServices.cpp
    src/app/ProcessServices.h
)
qt_add_resources(javelin_app "javelin_icons"
    PREFIX "/icons"
    BASE "res"
    FILES "res/icon.svg"
)
target_include_directories(javelin_app PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)
target_link_libraries(javelin_app
    PUBLIC
        Qt::Core
        Qt::Widgets
        Qt::WebEngineCore
        Qt::Svg
        KF6::CoreAddons
    PRIVATE
        javelin_gui
        javelin_jmap
)
javelin_set_project_warnings(javelin_app)
javelin_enable_static_analysis(javelin_app)
javelin_enable_sanitizers(javelin_app)
# QCoro and ProcessServices use exceptions internally
target_compile_options(javelin_app PRIVATE -fexceptions)

qt_add_executable(Javelin-Mail
    WIN32 MACOSX_BUNDLE
    src/app/main.cpp
)

# Copy the KXMLGUI .rc file into the build directory so KXmlGuiWindow can
# locate it at runtime without a full install.
configure_file(
    ${PROJECT_SOURCE_DIR}/res/javelinmailui.rc
    ${CMAKE_CURRENT_BINARY_DIR}/javelinmailui.rc
    COPYONLY
)

qt_add_translations(
    TARGETS Javelin-Mail
    TS_FILES Javelin-Mail_en_NZ.ts
)

target_link_libraries(Javelin-Mail
    PRIVATE
        javelin_app
        Qt::Svg
)
javelin_set_project_warnings(Javelin-Mail)
javelin_enable_static_analysis(Javelin-Mail)
javelin_enable_sanitizers(Javelin-Mail)

javelin_add_format_targets(${JAVELIN_FORMAT_FILES})

add_subdirectory(tests)

install(TARGETS Javelin-Mail
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES res/javelinmailui.rc
    DESTINATION ${KDE_INSTALL_KXMLGUIDIR}/javelinmail
)

qt_generate_deploy_app_script(
    TARGET Javelin-Mail
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})
