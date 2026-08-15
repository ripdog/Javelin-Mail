include(Catch)

function(javelin_configure_catch_test target)
    target_include_directories(${target}
        PRIVATE
            ${PROJECT_SOURCE_DIR}/tests
    )
    target_link_libraries(${target} PRIVATE Catch2::Catch2WithMain)
    target_compile_options(${target} PRIVATE -fexceptions)
    javelin_set_project_warnings(${target})
    javelin_enable_static_analysis(${target})
    javelin_enable_sanitizers(${target})
    catch_discover_tests(${target})
endfunction()

function(javelin_configure_gui_catch_test target)
    target_include_directories(${target}
        PRIVATE
            ${PROJECT_SOURCE_DIR}/tests
    )
    target_link_libraries(${target} PRIVATE Catch2::Catch2)
    target_compile_options(${target} PRIVATE -fexceptions)
    javelin_set_project_warnings(${target})
    javelin_enable_static_analysis(${target})
    javelin_enable_sanitizers(${target})
    catch_discover_tests(${target} DISCOVERY_MODE PRE_TEST)
endfunction()

function(javelin_enable_test_localization target)
    target_sources(${target} PRIVATE TestLocalization.cpp)
    target_link_libraries(${target} PRIVATE KF6::I18n)
endfunction()

# Test-only protocol adapter. It deliberately is not part of the production protocol library.
add_library(javelin_protocol_test_support STATIC
    ${PROJECT_SOURCE_DIR}/src/protocol/InProcessEndpoint.cpp
    ${PROJECT_SOURCE_DIR}/src/protocol/InProcessEndpoint.h
)
target_include_directories(javelin_protocol_test_support
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(javelin_protocol_test_support PUBLIC javelin_protocol)
javelin_set_project_warnings(javelin_protocol_test_support)
javelin_enable_static_analysis(javelin_protocol_test_support)
javelin_enable_sanitizers(javelin_protocol_test_support)

add_library(javelin_test_support STATIC
    FixtureReader.cpp
    FixtureReader.h
)
target_include_directories(javelin_test_support
    PUBLIC
        ${PROJECT_SOURCE_DIR}/tests
)
target_link_libraries(javelin_test_support PUBLIC Qt::Core)
target_compile_definitions(javelin_test_support
    PRIVATE
        JAVELIN_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
)
target_compile_options(javelin_test_support PRIVATE -fexceptions)
javelin_set_project_warnings(javelin_test_support)
javelin_enable_static_analysis(javelin_test_support)
javelin_enable_sanitizers(javelin_test_support)

add_executable(javelin_protocol_tests
    protocol/ProcessBoundaryTest.cpp
)
target_link_libraries(javelin_protocol_tests
    PRIVATE
        javelin_protocol
        javelin_protocol_test_support
        Qt::Network
)
javelin_configure_catch_test(javelin_protocol_tests)

add_executable(javelin_translation_tests
    app/TranslationServiceTest.cpp
    gui/GoogleTranslationBackendTest.cpp
    jmap/TranslationCacheRepositoryTest.cpp
    jmap/LanguageDetectionTest.cpp
)
if(JAVELIN_ENABLE_BERGAMOT_TRANSLATION)
    target_sources(javelin_translation_tests PRIVATE
        gui/TranslationModelManifestTest.cpp
        gui/TranslationModelStoreTest.cpp
    )
    target_link_libraries(javelin_translation_tests PRIVATE PkgConfig::ZSTD)
    set_source_files_properties(
        "${JAVELIN_BERGAMOT_SOURCE_DIR}/LICENSE"
        PROPERTIES QT_RESOURCE_ALIAS "MPL-2.0.txt")
    qt_add_resources(javelin_translation_tests "translation_test_licenses"
        PREFIX "/javelin/translation/licenses"
        FILES
            "${JAVELIN_BERGAMOT_SOURCE_DIR}/LICENSE"
    )
endif()
target_link_libraries(javelin_translation_tests
    PRIVATE
        QCoro6::Core
        Qt::Network
        Qt::Sql
        javelin_translation
)
javelin_configure_catch_test(javelin_translation_tests)
javelin_enable_test_localization(javelin_translation_tests)

# Storage/cache tests exercise persistence contracts and migration behavior. The writable
# repositories still live in javelin_jmap, while read-only infrastructure is supplied by
# javelin_cache_read through that production dependency.
add_executable(javelin_storage_tests
    jmap/AccountReadRepositoryTest.cpp
    jmap/AccountRepositoryTest.cpp
    jmap/CalendarNotificationRepositoryTest.cpp
    jmap/CalendarRepositoryTest.cpp
    jmap/ContactRepositoryTest.cpp
    jmap/DatabaseTest.cpp
    jmap/EmailRepositoryTest.cpp
    jmap/IdentityRepositoryTest.cpp
    jmap/JmapTransportPreferenceRepositoryTest.cpp
    jmap/MailboxReadRepositoryTest.cpp
    jmap/MailboxRepositoryTest.cpp
    jmap/MailVaultTest.cpp
    jmap/MessageViewServiceTest.cpp
    jmap/ReadOnlyCacheSurfaceTest.cpp
    jmap/SearchWindowRepositoryTest.cpp
    jmap/SessionRepositoryTest.cpp
    jmap/SyncStateRepositoryTest.cpp
    jmap/ThreadRepositoryTest.cpp
)
target_link_libraries(javelin_storage_tests
    PRIVATE
        javelin_jmap
        javelin_test_support
        QCoro6::Network
        Qt::Concurrent
        Qt::Network
)
javelin_configure_catch_test(javelin_storage_tests)
javelin_enable_test_localization(javelin_storage_tests)

add_executable(javelin_jmap_protocol_tests
    jmap/AccountOnboardingServiceTest.cpp
    jmap/AuthTest.cpp
    jmap/CalendarMethodsTest.cpp
    jmap/CancellationTest.cpp
    jmap/ContactsMethodsTest.cpp
    jmap/JmapCoreMessageContentTest.cpp
    jmap/JmapCoreTest.cpp
    jmap/MailMethodsTest.cpp
    jmap/MethodCallerTest.cpp
    jmap/MethodEnvelopeTest.cpp
    jmap/OperationErrorTest.cpp
    jmap/PreferredJmapMethodTransportTest.cpp
    jmap/RequestBuilderTest.cpp
    jmap/SessionClientTest.cpp
    jmap/SessionTest.cpp
)
target_link_libraries(javelin_jmap_protocol_tests
    PRIVATE
        javelin_jmap
        javelin_test_support
        QCoro6::Network
        Qt::Concurrent
        Qt::Network
)
javelin_configure_catch_test(javelin_jmap_protocol_tests)
javelin_enable_test_localization(javelin_jmap_protocol_tests)

add_executable(javelin_jmap_sync_tests
    jmap/ConsistencyDomainTest.cpp
    jmap/EventSourceLongPollTest.cpp
    jmap/LongPollMailboxObserverTest.cpp
    jmap/LongPollWorkerTest.cpp
    jmap/MailboxQueryDescriptorTest.cpp
    jmap/MailboxRefreshExecutorTest.cpp
    jmap/MailboxStateRefreshExecutorTest.cpp
    jmap/MailDeltaRefreshExecutorTest.cpp
    jmap/MutationJournalTest.cpp
    jmap/PreferredStateChangeSourceTest.cpp
    jmap/RefreshNotificationPlannerTest.cpp
    jmap/SyncPlannerTest.cpp
    jmap/SyncReconcilerTest.cpp
    jmap/WebSocketPushChannelTest.cpp
)
target_link_libraries(javelin_jmap_sync_tests
    PRIVATE
        javelin_jmap
        javelin_test_support
        QCoro6::Network
        Qt::Concurrent
        Qt::Network
)
javelin_configure_catch_test(javelin_jmap_sync_tests)
javelin_enable_test_localization(javelin_jmap_sync_tests)

add_executable(javelin_jmap_domain_tests
    jmap/CalendarColorTest.cpp
    jmap/CalendarEventEditingTest.cpp
    jmap/CalendarServiceTest.cpp
    jmap/ComposeRevisionGateTest.cpp
    jmap/ComposeServiceTest.cpp
    jmap/ContactServiceTest.cpp
    jmap/DraftInlineImageStorageTest.cpp
    jmap/EmailSearchTest.cpp
    jmap/HtmlBodyEmbeddingTest.cpp
    jmap/HtmlMessageDocumentBuilderTest.cpp
    jmap/HtmlTextExtractorTest.cpp
    jmap/IdentityServiceTest.cpp
    jmap/InlineMessageUrlTest.cpp
    jmap/MailEntityParsersTest.cpp
    jmap/MessageGroupingTest.cpp
    jmap/QueryDiffTest.cpp
    jmap/SieveServiceTest.cpp
)
target_link_libraries(javelin_jmap_domain_tests
    PRIVATE
        javelin_jmap
        javelin_test_support
        QCoro6::Network
        Qt::Concurrent
        Qt::Gui
        Qt::Network
)
javelin_configure_catch_test(javelin_jmap_domain_tests)
javelin_enable_test_localization(javelin_jmap_domain_tests)

# Daemon application tests now link the production daemon module rather than rebuilding app .cpp
# files inside a JMAP test executable.
add_executable(javelin_daemon_app_tests
    app/AccountCredentialStoreTest.cpp
    app/ApplicationErrorCoordinatorTest.cpp
    app/CacheAccessBarrierTest.cpp
    app/CacheInvalidationPublisherTest.cpp
    app/CacheLocationProviderTest.cpp
    app/CalendarHistoryExecutorTest.cpp
    app/CalendarInvitationServiceTest.cpp
    app/CalendarPreferenceExecutorTest.cpp
    app/CommandDispatcherTest.cpp
    app/ContactCommandPreparationTest.cpp
    app/ContactHistoryExecutorTest.cpp
    app/DaemonTrayControllerTest.cpp
    app/DeferredSendRepositoryTest.cpp
    app/DeferredSendServiceTest.cpp
    app/DeveloperDiagnosticsServiceTest.cpp
    app/DeveloperMaintenanceServiceTest.cpp
    app/DraftHistoryExecutorTest.cpp
    app/EmailMutationBatchSubmitterTest.cpp
    app/FullMailSyncServiceTest.cpp
    app/HistoryRepositoryTest.cpp
    app/MailTransferRepositoryTest.cpp
    app/MailboxSelectionMutationTest.cpp
    app/MailboxSessionTest.cpp
    app/MailboxSyncCoverageTest.cpp
    app/MailHistoryExecutorTest.cpp
    app/MessageNavigationCoordinatorTest.cpp
    app/MessageSubjectTest.cpp
    app/OAuthRefreshSingleFlightTest.cpp
    app/PerformanceMetricsTest.cpp
    app/ProcessInstanceLockTest.cpp
    app/RemoteCodecTest.cpp
    app/SearchSessionTest.cpp
    app/SieveHistoryExecutorTest.cpp
    app/StateChangePolicyTest.cpp
    app/ThreadMaterializationCoordinatorTest.cpp
    app/ThreadMembershipMaterializationWorkerTest.cpp
    app/UndoManagerTest.cpp
    app/WorkSchedulerTest.cpp
)
target_link_libraries(javelin_daemon_app_tests
    PRIVATE
        javelin_daemon_core
        javelin_test_support
        Qt::DBus
        Qt::Network
        Qt::Sql
)
javelin_configure_catch_test(javelin_daemon_app_tests)
javelin_enable_test_localization(javelin_daemon_app_tests)

# Pure presentation/policy tests must stay independent of daemon and writable JMAP targets.
add_executable(javelin_gui_policy_tests
    gui/ConnectionSettingsAdapterTest.cpp
    gui/GuiSettingsTest.cpp
    gui/ComposeTabPolicyTest.cpp
    gui/ComposeBodyConverterTest.cpp
    gui/IdentityPresentationTest.cpp
    gui/SignatureTrackingPolicyTest.cpp
    gui/MailboxSelectionTest.cpp
    gui/MailboxSortTest.cpp
    gui/MessageActionPolicyTest.cpp
    gui/EmailContextMenuLayoutTest.cpp
    gui/CalendarEventContextMenuLayoutTest.cpp
    gui/MessageActionSelectionTest.cpp
    gui/MessageConfirmationPresentationTest.cpp
    gui/MessageContentPolicyTest.cpp
    gui/MessageSelectionRestorationTest.cpp
    gui/MessageListTabPolicyTest.cpp
    gui/MessageListTabPresenterTest.cpp
    gui/MessageNavigationPolicyTest.cpp
    gui/TabActivationPolicyTest.cpp
    gui/TabPersistenceTest.cpp
    gui/TabWorkspaceTest.cpp
    gui/MainWindowStateStoreTest.cpp
    gui/MessageAppearanceTest.cpp
    gui/MessageViewPresentationTest.cpp
    gui/PaginationTest.cpp
    gui/RefreshGenerationTest.cpp
    gui/PlainTextLinkifierTest.cpp
    gui/MonthCalendarWidgetTest.cpp
    gui/RecurrencePatternTest.cpp
)
target_link_libraries(javelin_gui_policy_tests
    PRIVATE
        javelin_gui
        Qt::Widgets
)
javelin_configure_catch_test(javelin_gui_policy_tests)
javelin_enable_test_localization(javelin_gui_policy_tests)

add_executable(javelin_incompatible_gui_helper
    app/IncompatibleGuiHelper.cpp
)
target_link_libraries(javelin_incompatible_gui_helper
    PRIVATE
        Qt::Core
        javelin_protocol
)
javelin_set_project_warnings(javelin_incompatible_gui_helper)
javelin_enable_static_analysis(javelin_incompatible_gui_helper)
javelin_enable_sanitizers(javelin_incompatible_gui_helper)

# Full daemon lifecycle and desktop-notification tests retain a separate target because they
# exercise process composition and externally visible recovery behavior.
add_executable(javelin_daemon_integration_tests
    app/DaemonProcessTest.cpp
    app/DesktopNotificationControllerTest.cpp
)
target_link_libraries(javelin_daemon_integration_tests
    PRIVATE
        javelin_daemon_core
        Qt::DBus
        Qt::Network
        Qt::Sql
)
target_compile_definitions(javelin_daemon_integration_tests
    PRIVATE
        JAVELIN_INCOMPATIBLE_GUI_HELPER_PATH="$<TARGET_FILE:javelin_incompatible_gui_helper>"
)
add_dependencies(javelin_daemon_integration_tests javelin_incompatible_gui_helper)
javelin_configure_catch_test(javelin_daemon_integration_tests)
javelin_enable_test_localization(javelin_daemon_integration_tests)

# Cross-layer tests are explicit. They validate integration contracts rather than making every
# layer-focused target depend on both process halves.
add_executable(javelin_integration_tests
    app/GuiMailApplicationEventsTest.cpp
    app/SettingsRepositoryTest.cpp
    jmap/QueryServiceTest.cpp
)
target_link_libraries(javelin_integration_tests
    PRIVATE
        javelin_daemon_core
        javelin_gui
        javelin_gui_session
        javelin_test_support
        Qt::Network
        Qt::Sql
        Qt::Widgets
)
javelin_configure_catch_test(javelin_integration_tests)
javelin_enable_test_localization(javelin_integration_tests)

add_executable(javelin_gui_widget_tests
    gui/ContactsGuiTestMain.cpp
    gui/DayAgendaDialogTest.cpp
    gui/MonthCalendarAccessibilityTest.cpp
    gui/NoRttiAccessibleObject.cpp
    gui/NoRttiAccessibleObject.h
    gui/DeveloperOptionsDialogTest.cpp
    gui/FontUtilsTest.cpp
    gui/IconUtilsTest.cpp
    gui/IndeterminateProgressBarTest.cpp
    gui/EmailContextMenuEditorDialogTest.cpp
    gui/CalendarEventContextMenuEditorDialogTest.cpp
    gui/MessageBannerWidgetTest.cpp
    gui/MessageDragListViewTest.cpp
    gui/MailboxPreferenceStateTest.cpp
    gui/MailboxPendingCreateTest.cpp
    gui/MailboxPropertiesDialogTest.cpp
    gui/SendingIdentitiesDialogTest.cpp
    gui/UndoSendDialogTest.cpp
)
set_source_files_properties(gui/NoRttiAccessibleObject.cpp PROPERTIES COMPILE_OPTIONS -fno-rtti)
target_link_libraries(javelin_gui_widget_tests
    PRIVATE
        javelin_gui
        Qt::Widgets
)
target_compile_definitions(javelin_gui_widget_tests
    PRIVATE
        JAVELIN_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
javelin_configure_gui_catch_test(javelin_gui_widget_tests)
javelin_enable_test_localization(javelin_gui_widget_tests)

# GUI/cache integration tests use writable repositories only as fixture setup. Keeping them out of
# javelin_gui_widget_tests makes the ordinary widget target enforce the production GUI dependency
# direction.
add_executable(javelin_gui_integration_tests
    gui/ContactsGuiTestMain.cpp
    gui/ContactsManagerWidgetTest.cpp
    gui/MessageListModelTest.cpp
)
target_link_libraries(javelin_gui_integration_tests
    PRIVATE
        javelin_gui
        javelin_jmap
        Qt::Widgets
)
javelin_configure_gui_catch_test(javelin_gui_integration_tests)
javelin_enable_test_localization(javelin_gui_integration_tests)

add_executable(javelin_composer_gui_tests
    gui/ContactsGuiTestMain.cpp
    gui/JavelinComposerEditTest.cpp
    gui/ComposerInlineImageCodecTest.cpp
    gui/MailtoParserTest.cpp
)
target_link_libraries(javelin_composer_gui_tests
    PRIVATE
        javelin_gui
        KF6::XmlGui
        KPim6::MessageComposer
        Qt::Widgets
)
target_include_directories(javelin_composer_gui_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/tests
)
target_link_libraries(javelin_composer_gui_tests PRIVATE Catch2::Catch2)
target_compile_options(javelin_composer_gui_tests PRIVATE -fexceptions)
javelin_set_project_warnings(javelin_composer_gui_tests)
javelin_enable_static_analysis(javelin_composer_gui_tests)
javelin_enable_sanitizers(javelin_composer_gui_tests)
javelin_enable_test_localization(javelin_composer_gui_tests)
# KDE PIM keeps its generated settings singleton and KConfig caches alive until process exit.
# Keep ASan/UBSan enabled for these tests, but do not treat those external process-lifetime
# allocations as Javelin leaks.
catch_discover_tests(javelin_composer_gui_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0:halt_on_error=1")

# Phase 9 architecture guard: test executables consume production libraries. Production .cpp files
# may not be smuggled into a test target to bypass a target dependency. Test-only support libraries
# above are the deliberate exception.
set(javelin_phase9_test_executables
    javelin_protocol_tests
    javelin_translation_tests
    javelin_storage_tests
    javelin_jmap_protocol_tests
    javelin_jmap_sync_tests
    javelin_jmap_domain_tests
    javelin_daemon_app_tests
    javelin_daemon_integration_tests
    javelin_gui_policy_tests
    javelin_gui_widget_tests
    javelin_gui_integration_tests
    javelin_composer_gui_tests
    javelin_integration_tests
)
foreach(test_target IN LISTS javelin_phase9_test_executables)
    get_target_property(test_sources ${test_target} SOURCES)
    foreach(test_source IN LISTS test_sources)
        if(IS_ABSOLUTE "${test_source}")
            set(test_source_path "${test_source}")
        else()
            get_filename_component(test_source_path
                "${CMAKE_CURRENT_SOURCE_DIR}/${test_source}" ABSOLUTE)
        endif()
        string(FIND "${test_source_path}" "${PROJECT_SOURCE_DIR}/src/" production_source_prefix)
        if(production_source_prefix EQUAL 0)
            message(FATAL_ERROR
                "${test_target} compiles production source directly: ${test_source_path}. "
                "Link the owning production target instead.")
        endif()
    endforeach()
endforeach()

get_target_property(gui_policy_links javelin_gui_policy_tests LINK_LIBRARIES)
if(gui_policy_links MATCHES "javelin_(daemon_core|jmap)")
    message(FATAL_ERROR "GUI policy tests must not depend on daemon or writable JMAP targets")
endif()
get_target_property(gui_widget_links javelin_gui_widget_tests LINK_LIBRARIES)
if(gui_widget_links MATCHES "javelin_(daemon_core|jmap)")
    message(FATAL_ERROR "GUI widget tests must not depend on daemon or writable JMAP targets")
endif()
