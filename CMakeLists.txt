cmake_minimum_required(VERSION 3.24)
project(Javelin-Mail VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(JAVELIN_ENABLE_LOCAL_DATA_INSTALL "Copy app data into the active install prefix during builds" ON)
option(JAVELIN_ENABLE_FASTTEXT_LANGUAGE_DETECTION "Use fastText for local mail language detection" ON)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    set(JAVELIN_BERGAMOT_DEFAULT ON)
else()
    set(JAVELIN_BERGAMOT_DEFAULT OFF)
endif()
option(JAVELIN_ENABLE_BERGAMOT_TRANSLATION
       "Build native local message translation"
       ${JAVELIN_BERGAMOT_DEFAULT})
if(JAVELIN_ENABLE_BERGAMOT_TRANSLATION AND
   NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    message(FATAL_ERROR "Native Bergamot translation is currently supported only on x86-64")
endif()
option(JAVELIN_INSTALL_SYSTEMD_USER_SERVICE "Install the javelind systemd user unit" ON)

include(cmake/Warnings.cmake)
include(cmake/Dependencies.cmake)
include(cmake/StaticAnalysis.cmake)
include(cmake/Sanitizers.cmake)
include(CTest)

# 1. Find Qt6 FIRST
find_package(Qt6 6.6 REQUIRED COMPONENTS Core DBus Network Sql Widgets Concurrent WebEngineCore WebEngineWidgets Svg WebSockets)

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

get_filename_component(JAVELIN_INSTALL_BINDIR_ABSOLUTE
                       "${KDE_INSTALL_BINDIR}" ABSOLUTE
                       BASE_DIR "${CMAKE_INSTALL_PREFIX}")
set(JAVELIN_DAEMON_EXECUTABLE "${JAVELIN_INSTALL_BINDIR_ABSOLUTE}/javelind")
configure_file(
    "${PROJECT_SOURCE_DIR}/res/javelind.service.in"
    "${CMAKE_CURRENT_BINARY_DIR}/javelind.service"
    @ONLY
)

find_package(KF6 REQUIRED COMPONENTS Codecs ConfigWidgets XmlGui CoreAddons I18n TextEditor Wallet WidgetsAddons)
find_package(KPim6Mime REQUIRED)
find_package(KPim6MessageComposer REQUIRED)

qt_standard_project_setup()

set(JAVELIN_FASTTEXT_SOURCE_MODEL_PATH
    "${PROJECT_SOURCE_DIR}/res/models/fasttext/lid.176.ftz")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${JAVELIN_FASTTEXT_SOURCE_MODEL_PATH}")
if(JAVELIN_ENABLE_FASTTEXT_LANGUAGE_DETECTION AND
   NOT EXISTS "${JAVELIN_FASTTEXT_SOURCE_MODEL_PATH}")
    message(FATAL_ERROR
            "JAVELIN_ENABLE_FASTTEXT_LANGUAGE_DETECTION is enabled, but the fastText model is missing: "
            "${JAVELIN_FASTTEXT_SOURCE_MODEL_PATH}")
endif()

set(JAVELIN_FASTTEXT_LANGUAGE_MODEL_PATH
    "${CMAKE_INSTALL_PREFIX}/${KDE_INSTALL_DATADIR}/javelinmail/models/fasttext/lid.176.ftz")

add_subdirectory(src/protocol)
add_subdirectory(src/storage)

add_library(javelin_observability STATIC
    src/observability/LogStore.cpp
    src/app/LogStore.h
    src/observability/PerformanceMetrics.cpp
    src/app/PerformanceMetrics.h
)
target_include_directories(javelin_observability PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)
target_link_libraries(javelin_observability PUBLIC Qt::Core)
javelin_set_project_warnings(javelin_observability)
javelin_enable_static_analysis(javelin_observability)
javelin_enable_sanitizers(javelin_observability)

# QObject application contracts are shared by the daemon and GUI process. Keeping their
# metaobjects in one target avoids each executable/test graph regenerating the same contract code.
add_library(javelin_app_contracts STATIC
    src/app/CalendarApplicationPorts.h
    src/app/MailApplicationEventsPorts.h
    src/app/MessageListSession.h
    src/app/MessageNavigationPort.h
    src/app/UndoApplicationPorts.h
)
target_include_directories(javelin_app_contracts PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)
target_link_libraries(javelin_app_contracts
    PUBLIC
        QCoro6::Core
        Qt::Core
        Qt::Sql
)
javelin_set_project_warnings(javelin_app_contracts)
javelin_enable_static_analysis(javelin_app_contracts)
javelin_enable_sanitizers(javelin_app_contracts)

# Server-independent mail/cache model shared by the protocol stack and the GUI process. It may
# touch SQLite, but it has no transport or Widgets/WebEngine dependency.
add_library(javelin_mail_model STATIC
    src/jmap/api/PatchObject.cpp
    src/jmap/api/PatchObject.h
    src/storage/account/AccountReadRepository.cpp
    src/jmap/cache/AccountReadRepository.h
    src/storage/contacts/ContactRepository.cpp
    src/jmap/cache/ContactRepository.h
    src/storage/identity/IdentityRepository.cpp
    src/jmap/cache/IdentityRepository.h
    src/jmap/cache/EmailListSortSql.h
    src/storage/messages/MailSearchIndex.cpp
    src/jmap/cache/MailSearchIndex.h
    src/storage/messages/MailSearchReadRepository.cpp
    src/jmap/cache/MailSearchReadRepository.h
    src/jmap/cache/MailSearchReader.h
    src/storage/messages/MailTagReadRepository.cpp
    src/jmap/cache/MailTagReadRepository.h
    src/jmap/cache/MailTagReader.h
    src/storage/messages/MailboxStatisticsReadRepository.cpp
    src/jmap/cache/MailboxStatisticsReadRepository.h
    src/jmap/cache/MailboxStatisticsReader.h
    src/storage/messages/MailboxMessageReadRepository.cpp
    src/jmap/cache/MailboxMessageReadRepository.h
    src/jmap/cache/MailboxMessageReader.h
    src/storage/messages/MailboxFilterReadRepository.cpp
    src/jmap/cache/MailboxFilterReadRepository.h
    src/jmap/cache/MailboxFilterReader.h
    src/storage/messages/MailboxReadRepository.cpp
    src/jmap/cache/MailboxReadRepository.h
    src/storage/messages/MailboxRepository.cpp
    src/jmap/cache/MailboxRepository.h
    src/storage/messages/MailboxWindowRepository.cpp
    src/jmap/cache/MailboxWindowRepository.h
    src/storage/messages/MessageListReadSupport.cpp
    src/jmap/cache/MessageListReadSupport.h
    src/jmap/cache/MessageListReadTypes.h
    src/storage/messages/MessageSummaryReadRepository.cpp
    src/jmap/cache/MessageSummaryReadRepository.h
    src/jmap/cache/MessageSummaryReader.h
    src/storage/messages/SearchWindowReadRepository.cpp
    src/jmap/cache/SearchWindowReadRepository.h
    src/storage/messages/MailboxWindowReadRepository.cpp
    src/jmap/cache/MailboxWindowReadRepository.h
    src/storage/messages/QueryWindowReadRepository.cpp
    src/jmap/cache/QueryWindowReadRepository.h
    src/jmap/cache/QueryWindowReader.h
    src/storage/messages/ThreadReadRepository.cpp
    src/jmap/cache/ThreadReadRepository.h
    src/jmap/cache/ThreadReader.h
    src/storage/messages/SearchWindowRepository.cpp
    src/jmap/cache/SearchWindowRepository.h
    src/storage/sync/SyncStateRepository.cpp
    src/jmap/cache/SyncStateRepository.h
    src/jmap/calendar/CalendarEventEditing.cpp
    src/jmap/calendar/CalendarEventEditing.h
    src/jmap/contacts/ContactIdentityLookup.cpp
    src/jmap/contacts/ContactIdentityLookup.h
    src/jmap/contacts/ContactInterchange.cpp
    src/jmap/contacts/ContactInterchange.h
    src/jmap/contacts/ContactTypes.cpp
    src/jmap/contacts/ContactTypes.h
    src/jmap/domain/MailEntityParsers.cpp
    src/jmap/domain/MailEntityParsers.h
    src/jmap/domain/MailEntities.h
    src/jmap/search/EmailSearch.cpp
    src/jmap/search/EmailSearch.h
    src/jmap/sync/MailboxQueryDescriptor.cpp
    src/jmap/sync/MailboxQueryDescriptor.h
)
target_include_directories(javelin_mail_model PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)
target_link_libraries(javelin_mail_model
    PUBLIC
        javelin_cache_read
        Qt::Core
    PRIVATE
        glaze::glaze
        Qt::Concurrent
        Qt::Sql
)
target_compile_options(javelin_mail_model PRIVATE -fexceptions)
javelin_set_project_warnings(javelin_mail_model)
javelin_enable_static_analysis(javelin_mail_model)
javelin_enable_sanitizers(javelin_mail_model)

# Application-side list/navigation services are used by both process compositions. They depend on
# the local mail model and typed application contracts, but not on daemon transports or GUI widgets.
add_library(javelin_app_shared STATIC
    src/app/messages/MailboxSession.cpp
    src/app/MailboxSession.h
    src/app/messages/MessageListSessionFactoryService.cpp
    src/app/MessageListSessionFactoryService.h
    src/app/messages/MessageNavigationCoordinator.cpp
    src/app/MessageNavigationCoordinator.h
    src/app/runtime/ProcessInstanceLock.cpp
    src/app/ProcessInstanceLock.h
    src/app/messages/SearchSession.cpp
    src/app/SearchSession.h
)
target_include_directories(javelin_app_shared PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)
target_link_libraries(javelin_app_shared
    PUBLIC
        javelin_app_contracts
        javelin_cache_read
        javelin_mail_model
        Qt::Core
    PRIVATE
        KF6::I18n
        QCoro6::Core
        Qt::Concurrent
)
target_compile_options(javelin_app_shared PRIVATE -fexceptions)
javelin_set_project_warnings(javelin_app_shared)
javelin_enable_static_analysis(javelin_app_shared)
javelin_enable_sanitizers(javelin_app_shared)

set(JAVELIN_FORMAT_FILES
    src/gui/widgets/IndeterminateProgressBar.h
    src/gui/developer/DeveloperOptionsDialog.cpp
    src/gui/developer/DeveloperOptionsDialog.h
    tests/app/DeveloperDiagnosticsServiceTest.cpp
    tests/app/DeveloperMaintenanceServiceTest.cpp
    tests/gui/DeveloperOptionsDialogTest.cpp
    tests/gui/IndeterminateProgressBarTest.cpp
    src/gui/messages/MessageConfirmationPresentation.cpp
    src/gui/messages/MessageConfirmationPresentation.h
    tests/gui/MessageConfirmationPresentationTest.cpp
    tests/app/RemoteCodecTest.cpp
    src/daemon/actions/DaemonRemoteActionAccountHandler.cpp
    src/daemon/actions/DaemonRemoteActionCalendarHandler.cpp
    src/daemon/actions/DaemonRemoteActionComposeHandler.cpp
    src/daemon/actions/DaemonRemoteActionContactHandler.cpp
    src/daemon/actions/DaemonRemoteActionDeveloperHandler.cpp
    src/daemon/actions/DaemonRemoteActionDispatcher.cpp
    src/daemon/actions/DaemonRemoteActionDispatcher.h
    src/daemon/actions/DaemonRemoteActionHistoryHandler.cpp
    src/daemon/actions/DaemonRemoteActionIdentityHandler.cpp
    src/daemon/actions/DaemonRemoteActionMailHandler.cpp
    src/daemon/actions/DaemonRemoteActionSieveHandler.cpp
    src/daemon/actions/DaemonRemoteActionWorkHandler.cpp
    src/client/RemoteActionClient.cpp
    src/client/RemoteActionClient.h
    src/app/RemoteCodec.h
    src/protocol/ActionContract.h
    src/protocol/ActionNames.h
    src/protocol/ActivationContract.h
    src/protocol/BoundaryEventContract.h
    src/protocol/CacheContract.h
    src/protocol/DaemonRequestHandler.h
    src/protocol/HandshakeContract.h
    src/protocol/ProtocolTypes.h
    src/protocol/ProtocolValidation.cpp
    src/protocol/ProtocolValidation.h
    src/protocol/SettingsContract.h
    src/protocol/actions/AccountActions.h
    src/protocol/actions/ActionCatalog.h
    src/protocol/actions/ActionDescriptor.h
    src/protocol/actions/CalendarActions.h
    src/protocol/actions/ComposeActions.h
    src/protocol/actions/ContactActions.h
    src/protocol/actions/DeveloperActions.h
    src/protocol/actions/HistoryActions.h
    src/protocol/actions/IdentityActions.h
    src/protocol/actions/MailActions.h
    src/protocol/actions/SieveActions.h
    src/protocol/actions/WorkActions.h
    src/protocol/SocketFrameCodec.cpp
    src/protocol/SocketFrameCodec.h
    src/protocol/SocketSecurity.cpp
    src/protocol/SocketSecurity.h
    src/protocol/SocketWireCodec.cpp
    src/protocol/SocketWireCodecInternal.h
    src/protocol/LocalDaemonServer.cpp
    src/protocol/LocalDaemonServer.h
    src/protocol/LocalDaemonClient.cpp
    src/protocol/LocalDaemonClient.h
    src/protocol/LocalActivationServer.cpp
    src/protocol/LocalActivationServer.h
    src/protocol/LocalActivationClient.cpp
    src/protocol/LocalActivationClient.h
    src/jmap/render/HtmlBodyEmbedding.cpp
    src/jmap/submission/DraftInlineImageStorage.cpp
    src/jmap/submission/DraftInlineImageStorage.h
    src/jmap/render/HtmlBodyEmbedding.h
    tests/gui/ComposerInlineImageCodecTest.cpp
    tests/gui/JavelinComposerEditTest.cpp
    tests/jmap/DraftInlineImageStorageTest.cpp
    tests/jmap/HtmlBodyEmbeddingTest.cpp
    tests/protocol/ProcessBoundaryTest.cpp
    src/jmap/sync/PreferredStateChangeSource.cpp
    src/jmap/sync/PreferredStateChangeSource.h
    src/jmap/sync/EmailMutationQueue.cpp
    src/jmap/sync/EmailMutationQueue.h
    tests/jmap/PreferredStateChangeSourceTest.cpp
    src/jmap/api/Cancellation.cpp
    src/jmap/api/Cancellation.h
    src/storage/messages/SearchWindowRepository.cpp
    src/jmap/cache/SearchWindowRepository.h
    src/storage/messages/MailboxWindowRepository.cpp
    src/jmap/cache/MailboxWindowRepository.h
    src/storage/sync/NotificationRepository.cpp
    src/jmap/cache/NotificationRepository.h
    src/storage/sync/NotificationDispatchRepository.cpp
    src/jmap/cache/NotificationDispatchRepository.h
    src/jmap/cache/ContactReader.h
    src/storage/messages/EmailReadRepository.cpp
    src/jmap/cache/EmailReadRepository.h
    src/jmap/cache/IdentityReader.h
    src/jmap/cache/MessageContentTypes.h
    src/storage/messages/MessageViewService.cpp
    src/jmap/cache/MessageViewService.h
    src/jmap/cache/MessageViewReader.h
    src/storage/messages/MimeMessageParser.cpp
    src/jmap/cache/MimeMessageParser.h
    src/storage/messages/RawMessageSourceReadRepository.cpp
    src/jmap/cache/RawMessageSourceReadRepository.h
    src/jmap/calendar/CalendarReader.h
    src/storage/calendar/CalendarNotificationRepository.cpp
    src/jmap/cache/CalendarNotificationRepository.h
    src/storage/calendar/CalendarInvitationRepository.cpp
    src/jmap/cache/CalendarInvitationRepository.h
    src/jmap/sync/StateChangeSource.h
    src/jmap/sync/MailboxInterestRegistry.cpp
    src/jmap/sync/MailboxInterestRegistry.h
    src/app/AccountConnectionProvider.h
    src/app/AccountConnectionSettings.h
    src/app/AccountApplicationPorts.h
    src/app/account/AccountCommandService.cpp
    src/app/AccountCommandService.h
    src/app/AccountRefreshApplicationPorts.h
    src/app/account/AccountRefreshCommandService.cpp
    src/app/AccountRefreshCommandService.h
    src/app/runtime/CommandDispatcher.cpp
    src/app/CommandDispatcher.h
    src/app/runtime/CacheInvalidationPublisher.cpp
    src/app/CacheInvalidationPublisher.h
    src/app/RefreshGeneration.h
    src/app/MailApplicationEventsPorts.h
    src/app/messages/MailApplicationEventsService.cpp
    src/app/MailApplicationEventsService.h
    src/app/runtime/CacheAccessBarrier.cpp
    src/app/CacheAccessBarrier.h
    src/app/runtime/ApplicationErrorCoordinator.cpp
    src/app/ApplicationErrorCoordinator.h
    src/app/calendar/CalendarNotificationService.cpp
    src/app/CalendarNotificationService.h
    src/app/calendar/CalendarInvitationService.cpp
    src/app/CalendarInvitationService.h
    src/app/CalendarApplicationPorts.h
    src/app/calendar/CalendarCommandService.cpp
    src/app/CalendarCommandService.h
    src/app/ContactApplicationPorts.h
    src/app/MailApplicationTypes.h
    src/app/MailApplicationPorts.h
    src/app/messages/MailCommandService.cpp
    src/app/MailCommandService.h
    src/app/MessageContentApplicationPorts.h
    src/app/messages/MessageContentCommandService.cpp
    src/app/MessageContentCommandService.h
    src/app/MessageListCacheRead.h
    src/app/MessageSubject.h
    src/app/MailboxTreeCacheRead.h
    src/app/MessageListSessionFactory.h
    src/app/messages/MessageListSessionFactoryService.cpp
    src/app/MessageListSessionFactoryService.h
    src/app/SieveApplicationPorts.h
    src/app/sieve/SieveCommandService.cpp
    src/app/SieveCommandService.h
    src/app/IdentityApplicationPorts.h
    src/app/identity/IdentityCommandService.cpp
    src/app/IdentityCommandService.h
    src/app/UndoApplicationPorts.h
    src/app/undo/UndoCommandService.cpp
    src/app/UndoCommandService.h
    src/app/contacts/ContactCommandPreparation.cpp
    src/app/ContactCommandPreparation.h
    src/app/ContactCommands.h
    src/app/contacts/ContactCommandService.cpp
    src/app/ContactCommandService.h
    src/gui/compose/AddressSuggestionStore.cpp
    src/gui/compose/AddressSuggestionStore.h
    src/app/compose/ComposeService.cpp
    src/app/ComposeService.h
    src/app/ComposeApplicationPorts.h
    src/app/compose/ComposeCommandService.cpp
    src/app/ComposeCommandService.h
    src/app/compose/ComposePreferences.cpp
    src/app/ComposePreferences.h
    src/app/compose/DeferredSendRepository.cpp
    src/app/DeferredSendRepository.h
    src/app/messages/MailTransferRepository.cpp
    src/app/MailTransferRepository.h
    src/app/messages/MailTransferPlanning.cpp
    src/app/MailTransferPlanning.h
    src/app/messages/MailTransferApplicationService.cpp
    src/app/MailTransferApplicationService.h
    src/app/messages/MailTransferCommandService.cpp
    src/app/MailTransferCommandService.h
    src/app/messages/MailTransferExecutor.cpp
    src/app/MailTransferExecutor.h
    src/app/compose/DeferredSendService.cpp
    src/app/DeferredSendService.h
    src/app/DeveloperDiagnostics.h
    src/app/developer/DeveloperDiagnosticsService.cpp
    src/app/DeveloperDiagnosticsService.h
    src/app/DeveloperMaintenance.h
    src/app/developer/DeveloperMaintenanceService.cpp
    src/app/DeveloperMaintenanceService.h
    src/app/messages/MailboxMaintenanceRegistry.cpp
    src/app/MailboxMaintenanceRegistry.h
    src/desktop/notifications/DesktopNotificationController.cpp
    src/desktop/notifications/DesktopNotificationController.h
    src/app/messages/FullMailSyncService.cpp
    src/app/FullMailSyncService.h
    src/app/undo/HistoryRepository.cpp
    src/app/undo/HistoryRepository.h
    src/app/undo/HistorySerialization.cpp
    src/app/undo/HistorySerialization.h
    src/app/undo/HistoryTypes.cpp
    src/app/undo/HistoryTypes.h
    src/app/undo/HistoryCommandExecutor.h
    src/app/undo/DraftHistoryExecutor.cpp
    src/app/undo/DraftHistoryExecutor.h
    src/app/undo/DraftHistoryPort.h
    src/app/undo/CalendarHistoryExecutor.cpp
    src/app/undo/CalendarHistoryExecutor.h
    src/app/undo/CalendarHistoryPort.h
    src/app/undo/CalendarPreferenceExecutor.cpp
    src/app/undo/CalendarPreferenceExecutor.h
    src/app/undo/CalendarPreferencePort.h
    src/app/undo/AddressBookHistoryExecutor.cpp
    src/app/undo/AddressBookHistoryExecutor.h
    src/app/undo/AddressBookHistoryPort.h
    src/app/undo/ContactHistoryExecutor.cpp
    src/app/undo/ContactHistoryExecutor.h
    src/app/undo/ContactHistoryPort.h
    src/app/undo/MailHistoryExecutor.cpp
    src/app/undo/MailHistoryExecutor.h
    src/app/undo/MailHistoryPort.h
    src/app/undo/MailTransferHistoryCoordinator.cpp
    src/app/undo/MailTransferHistoryCoordinator.h
    src/app/undo/MailTransferHistoryExecutor.cpp
    src/app/undo/MailTransferHistoryExecutor.h
    src/app/undo/MailTransferHistoryPort.h
    src/app/undo/MailTransferHistoryService.cpp
    src/app/undo/MailTransferHistoryService.h
    src/app/undo/SieveHistoryExecutor.cpp
    src/app/undo/SieveHistoryExecutor.h
    src/app/undo/SieveHistoryPort.h
    src/app/undo/UndoManager.cpp
    src/app/undo/UndoManager.h
    src/app/messages/MailIndexService.cpp
    src/app/MailIndexService.h
    src/app/runtime/LocalMaintenanceService.cpp
    src/app/LocalMaintenanceService.h
    src/gui/messageview/InlineMessageSchemeHandler.cpp
    src/gui/messageview/InlineMessageSchemeHandler.h
    src/observability/LogStore.cpp
    src/app/LogStore.h
    src/app/account/AccountSyncCoordinator.cpp
    src/app/account/AccountSyncCoordinator.h
    src/app/account/OAuthRefreshSingleFlight.cpp
    src/app/OAuthRefreshSingleFlight.h
    src/app/messages/MailApplicationService.cpp
    src/app/AccountRuntimeManager.h
    src/app/CalendarApplicationService.h
    src/app/ContactApplicationService.h
    src/app/MailMutationApplicationService.h
    src/app/MailNotificationService.h
    src/app/MailQueryApplicationService.h
    src/app/MessageContentApplicationService.h
    src/app/SieveApplicationService.h
    src/app/messages/EmailMutationBatchSubmitter.cpp
    src/app/EmailMutationBatchSubmitter.h
    src/app/messages/ThreadMaterializationCoordinator.cpp
    src/app/ThreadMaterializationCoordinator.h
    src/app/messages/ThreadMembershipMaterializationWorker.cpp
    src/app/ThreadMembershipMaterializationWorker.h
    src/app/messages/MailboxSelectionMutation.cpp
    src/app/MailboxSelectionMutation.h
    src/app/messages/MessageSelection.cpp
    src/app/MessageSelection.h
    src/app/messages/MailboxSyncCoverage.cpp
    src/app/MailboxSyncCoverage.h
    src/app/messages/MessageNavigationCoordinator.cpp
    src/app/MessageNavigationCoordinator.h
    src/daemon/DaemonServices.cpp
    src/daemon/DaemonServices.h
    src/app/MessageNavigationPort.h
    src/gui/tasks/WorkTaskModel.cpp
    src/gui/tasks/WorkTaskModel.h
    src/app/WorkTaskPort.h
    src/gui/IconUtils.cpp
    src/gui/IconUtils.h
    src/gui/logging/LogViewerDialog.cpp
    src/gui/logging/LogViewerDialog.h
    src/gui/mailboxes/MailboxPresentation.cpp
    src/gui/mailboxes/MailboxPresentation.h
    src/gui/mailboxes/MailboxPropertiesDialog.cpp
    src/gui/mailboxes/MailboxPropertiesDialog.h
    src/gui/mailboxes/MailboxSelection.cpp
    src/gui/mailboxes/MailboxSelection.h
    src/gui/mailboxes/MailboxTreeDelegate.cpp
    src/gui/mailboxes/MailboxTreeDelegate.h
    src/gui/mailboxes/MailboxTreeModel.cpp
    src/gui/mailboxes/MailboxTreeModel.h
    src/gui/mailboxes/MailboxTreeView.cpp
    src/gui/mailboxes/MailboxTreeView.h
    src/gui/compose/ComposeTabWidget.cpp
    src/gui/compose/ComposeTabWidget.h
    src/gui/compose/AttachmentController.cpp
    src/gui/compose/AttachmentController.h
    src/gui/compose/ComposeAutosaveController.cpp
    src/gui/compose/ComposeAutosaveController.h
    src/gui/compose/ComposeIdentityController.cpp
    src/gui/compose/ComposeIdentityController.h
    src/gui/compose/InlineImageController.cpp
    src/gui/compose/InlineImageController.h
    src/gui/compose/ComposeRecipientController.cpp
    src/gui/compose/ComposeRecipientController.h
    src/gui/compose/IdentityPresentation.cpp
    src/gui/compose/IdentityPresentation.h
    src/gui/compose/SignatureController.cpp
    src/gui/compose/SignatureController.h
    src/gui/compose/SignatureTrackingPolicy.cpp
    src/gui/compose/SignatureTrackingPolicy.h
    src/gui/compose/SendingIdentitiesDialog.cpp
    src/gui/compose/SendingIdentitiesDialog.h
    src/gui/compose/UndoSendDialog.cpp
    src/gui/compose/UndoSendDialog.h
    src/gui/identity/IdentityManagerDialog.cpp
    src/gui/identity/IdentityManagerDialog.h
    src/gui/compose/ComposeUiPreferences.cpp
    src/gui/compose/ComposeUiPreferences.h
    src/gui/compose/ComposerInlineImageCodec.cpp
    src/gui/compose/ComposerInlineImageCodec.h
    src/gui/compose/JavelinComposerEdit.cpp
    src/gui/compose/JavelinComposerEdit.h
    src/gui/compose/ComposeBodyConverter.cpp
    src/gui/compose/ComposeBodyConverter.h
    src/gui/contacts/AddressBookController.cpp
    src/gui/contacts/AddressBookController.h
    src/gui/contacts/ContactDetailsView.cpp
    src/gui/contacts/ContactDetailsView.h
    src/gui/contacts/ContactEditor.cpp
    src/gui/contacts/ContactEditor.h
    src/gui/contacts/ContactGroupController.cpp
    src/gui/contacts/ContactGroupController.h
    src/gui/contacts/ContactPhotoController.cpp
    src/gui/contacts/ContactPhotoController.h
    src/gui/contacts/ContactsBrowser.cpp
    src/gui/contacts/ContactsBrowser.h
    src/gui/contacts/ContactsManagerWidget.cpp
    src/gui/contacts/ContactsManagerWidget.h
    src/gui/contacts/ContactsViewState.h
    src/gui/widgets/EmailAddressLineEdit.cpp
    src/gui/widgets/EmailAddressLineEdit.h
    src/gui/calendar/MonthCalendarWidget.cpp
    src/gui/calendar/MonthCalendarWidget.h
    src/gui/calendar/MonthCalendarLayout.cpp
    src/gui/calendar/MonthCalendarLayout.h
    src/gui/calendar/DayAgendaDialog.cpp
    src/gui/calendar/DayAgendaDialog.h
    src/gui/calendar/CalendarEventContextMenuEditorDialog.cpp
    src/gui/calendar/CalendarEventContextMenuEditorDialog.h
    src/gui/calendar/CalendarEventContextMenuLayout.cpp
    src/gui/calendar/CalendarEventContextMenuLayout.h
    src/gui/calendar/EventDialog.cpp
    src/gui/calendar/EventDialog.h
    src/gui/calendar/CalendarPresentation.cpp
    src/gui/calendar/CalendarPresentation.h
    src/gui/calendar/RecurrenceDialog.cpp
    src/gui/calendar/RecurrenceDialog.h
    src/gui/calendar/RecurrencePattern.cpp
    src/gui/calendar/RecurrencePattern.h
    src/gui/messages/InfiniteScroll.h
    src/gui/messages/MessageListWindowPersistence.h
    src/gui/messages/MessageListModel.cpp
    src/gui/messages/MessageListModel.h
    src/gui/messages/MessageListPanePresenter.cpp
    src/gui/messages/MessageListPanePresenter.h
    src/gui/messages/MessageListDelegate.cpp
    src/gui/messages/MessageListDelegate.h
    src/gui/messages/MessageDragListView.cpp
    src/gui/messages/MessageDragListView.h
    src/gui/messages/MessageActionSelection.cpp
    src/gui/messages/MessageActionSelection.h
    src/gui/messages/MessageSelectionRestoration.cpp
    src/gui/messages/MessageSelectionRestoration.h
    src/gui/messageview/HtmlMessageView.cpp
    src/gui/messageview/HtmlMessageView.h
    src/gui/messageview/MessageAppearance.cpp
    src/gui/messageview/MessageAppearance.h
    src/gui/messageview/MessageBannerWidget.cpp
    src/gui/messageview/MessageBannerWidget.h
    src/gui/messageview/MessageAttachmentPanel.cpp
    src/gui/messageview/MessageAttachmentPanel.h
    src/gui/messageview/MessageBannerCoordinator.cpp
    src/gui/messageview/MessageBannerCoordinator.h
    src/gui/messageview/MessageBodyPresenter.cpp
    src/gui/messageview/MessageBodyPresenter.h
    src/gui/messageview/MessageTranslationController.cpp
    src/gui/messageview/MessageTranslationController.h
    src/gui/messageview/RemoteContentController.cpp
    src/gui/messageview/RemoteContentController.h
    src/gui/messageview/MessageViewContainer.cpp
    src/gui/messageview/MessageViewContainer.h
    src/gui/messageview/MessageViewPresentation.h
    src/gui/messageview/PlainTextLinkifier.cpp
    src/gui/messageview/PlainTextLinkifier.h
    src/gui/search/AdvancedSearchDialog.cpp
    src/gui/search/AdvancedSearchDialog.h
    src/app/messages/SearchSession.cpp
    src/app/SearchSession.h
    src/app/MessageListSession.h
    src/app/messages/MailboxSession.cpp
    src/app/MailboxSession.h
    src/gui/search/SearchSessionPersistence.cpp
    src/gui/search/SearchSessionPersistence.h
    src/gui/settings/PreferencesDialog.cpp
    src/gui/settings/PreferencesDialog.h
    src/gui/settings/PreferencesPages.cpp
    src/gui/settings/PreferencesPages.h
    src/gui/onboarding/FirstRunWizard.cpp
    src/gui/onboarding/FirstRunWizard.h
    src/gui/shell/ElidingLabel.cpp
    src/gui/shell/ElidingLabel.h
    src/gui/shell/FocusedCommandRouter.cpp
    src/gui/shell/FocusedCommandRouter.h
    src/gui/shell/MessageFileUtils.cpp
    src/gui/shell/MessageFileUtils.h
    src/gui/shell/MessageFileController.cpp
    src/gui/shell/MessageFileController.h
    src/gui/shell/MessageCommandController.cpp
    src/gui/shell/MessageCommandController.h
    src/gui/shell/MessageListTabController.cpp
    src/gui/shell/MessageListTabController.h
    src/gui/shell/MessageListTabPolicy.cpp
    src/gui/shell/MessageListTabPolicy.h
    src/gui/shell/TabActivationPolicy.cpp
    src/gui/shell/TabActivationPolicy.h
    src/gui/shell/TabPersistence.cpp
    src/gui/shell/TabPersistencePolicy.cpp
    src/gui/shell/TabPersistence.h
    src/gui/shell/TabWorkspace.cpp
    src/gui/shell/TabWorkspacePolicy.cpp
    src/gui/shell/TabWorkspace.h
    src/gui/shell/TabBarPresenter.cpp
    src/gui/shell/TabBarPresenter.h
    src/gui/shell/MainWindowStateStore.cpp
    src/gui/shell/MainWindowStateStore.h
    src/gui/shell/LayeredStatusBar.cpp
    src/gui/shell/LayeredStatusBar.h
    src/gui/shell/MainWindow.cpp
    src/gui/shell/MainWindow.h
    src/gui/shell/CalendarTabController.cpp
    src/gui/shell/CalendarTabController.h
    src/gui/shell/ContactsTabController.cpp
    src/gui/shell/ContactsTabController.h
    src/gui/shell/ComposeTabController.cpp
    src/gui/shell/ComposeTabController.h
    src/gui/shell/ComposeTabPolicy.cpp
    src/gui/shell/ComposeTabPolicy.h
    src/gui/shell/MessageActionPolicy.cpp
    src/gui/shell/MessageActionPolicy.h
    src/gui/shell/EmailContextMenuLayout.cpp
    src/gui/shell/EmailContextMenuLayout.h
    src/gui/shell/EmailContextMenuEditorDialog.cpp
    src/gui/shell/EmailContextMenuEditorDialog.h
    src/gui/shell/MessageListPresentationPolicy.cpp
    src/gui/shell/MessageListPresentationPolicy.h
    src/gui/shell/MessageListTabBindingPresenter.cpp
    src/gui/shell/MessageListTabBindingPresenter.h
    src/gui/shell/MessageListTabPresenter.cpp
    src/gui/shell/MessageListTabPresenter.h
    src/gui/shell/MessageNavigationController.cpp
    src/gui/shell/MessageNavigationController.h
    src/gui/shell/MessageNavigationPolicy.cpp
    src/gui/shell/MessageNavigationPolicy.h
    src/gui/settings/ConnectionSettings.h
    src/gui/settings/ConnectionSettingsAdapter.cpp
    src/gui/settings/ConnectionSettingsAdapter.h
    src/gui/shell/AccountRefreshController.cpp
    src/gui/shell/AccountRefreshController.h
    src/gui/shell/AuthenticationPromptCoordinator.cpp
    src/gui/shell/AuthenticationPromptCoordinator.h
    src/gui/shell/MessageContentController.cpp
    src/gui/shell/MessageContentController.h
    src/gui/shell/MessageContentPolicy.cpp
    src/gui/shell/MessageContentPolicy.h
    src/gui/shell/MessageSelectionController.cpp
    src/gui/shell/MessageSelectionController.h
    src/gui/shell/QuickFilterController.cpp
    src/gui/shell/QuickFilterController.h
    src/gui/shell/MailActionController.cpp
    src/gui/shell/MailActionController.h
    src/gui/shell/MailWorkspaceController.cpp
    src/gui/shell/MailWorkspaceController.h
    src/gui/shell/ThemeController.cpp
    src/gui/shell/ThemeController.h
    src/jmap/api/Error.cpp
    src/jmap/api/Error.h
    src/jmap/api/BlobUpload.cpp
    src/jmap/api/BlobUpload.h
    src/jmap/api/ContactsMethods.cpp
    src/jmap/api/ContactsMethods.h
    src/jmap/api/CalendarMethods.cpp
    src/jmap/api/CalendarMethods.h
    src/jmap/calendar/CalendarTypes.h
    src/jmap/calendar/CalendarCommandTypes.h
    src/jmap/calendar/CalendarColor.cpp
    src/jmap/calendar/CalendarColor.h
    src/jmap/calendar/CalendarEventEditing.cpp
    src/jmap/calendar/CalendarEventEditing.h
    src/jmap/calendar/CalendarService.cpp
    src/jmap/calendar/CalendarCacheReader.h
    src/jmap/calendar/CalendarMutationEngine.h
    src/jmap/calendar/CalendarProtocolClient.h
    src/jmap/calendar/CalendarReader.h
    src/jmap/calendar/CalendarSyncEngine.h
    src/jmap/sieve/SieveCommandTypes.h
    src/jmap/api/MailMethods.cpp
    src/jmap/api/MailMethods.h
    src/jmap/api/MethodCaller.cpp
    src/jmap/api/MethodCaller.h
    src/jmap/api/JmapMethodTransport.cpp
    src/jmap/api/JmapMethodTransport.h
    src/jmap/api/MethodEnvelope.cpp
    src/jmap/api/MethodEnvelope.h
    src/jmap/api/PatchObject.cpp
    src/jmap/api/PatchObject.h
    src/jmap/api/RequestBuilder.h
    src/jmap/api/ResponseReader.h
    src/jmap/api/Session.cpp
    src/jmap/api/Session.h
    src/jmap/api/SessionClient.cpp
    src/jmap/api/SessionClient.h
    src/jmap/api/SessionDiscovery.cpp
    src/jmap/api/SessionDiscovery.h
    src/jmap/api/SessionParser.cpp
    src/jmap/api/SessionParser.h
    src/jmap/api/Transport.cpp
    src/jmap/api/Transport.h
    src/storage/account/AccountRepository.cpp
    src/jmap/cache/AccountRepository.h
    src/storage/account/AccountReadRepository.cpp
    src/jmap/cache/AccountReadRepository.h
    src/storage/messages/MailTagReadRepository.cpp
    src/jmap/cache/MailTagReadRepository.h
    src/jmap/cache/MailTagReader.h
    src/storage/messages/MailboxStatisticsReadRepository.cpp
    src/jmap/cache/MailboxStatisticsReadRepository.h
    src/jmap/cache/MailboxStatisticsReader.h
    src/storage/messages/MailboxMessageReadRepository.cpp
    src/jmap/cache/MailboxMessageReadRepository.h
    src/jmap/cache/MailboxMessageReader.h
    src/storage/messages/MailboxFilterReadRepository.cpp
    src/jmap/cache/MailboxFilterReadRepository.h
    src/jmap/cache/MailboxFilterReader.h
    src/storage/messages/MailboxReadRepository.cpp
    src/jmap/cache/MailboxReadRepository.h
    src/storage/compose/ComposeSessionRepository.cpp
    src/jmap/cache/ComposeSessionRepository.h
    src/storage/contacts/ContactRepository.cpp
    src/jmap/cache/ContactRepository.h
    src/jmap/cache/ContactReader.h
    src/storage/calendar/CalendarRepository.cpp
    src/jmap/cache/CalendarRepository.h
    src/storage/messages/MessageListReadSupport.cpp
    src/jmap/cache/MessageListReadSupport.h
    src/jmap/cache/MessageListReadTypes.h
    src/storage/messages/MessageSummaryReadRepository.cpp
    src/jmap/cache/MessageSummaryReadRepository.h
    src/jmap/cache/MessageSummaryReader.h
    src/storage/messages/SearchWindowReadRepository.cpp
    src/jmap/cache/SearchWindowReadRepository.h
    src/storage/messages/MailboxWindowReadRepository.cpp
    src/jmap/cache/MailboxWindowReadRepository.h
    src/storage/messages/QueryWindowReadRepository.cpp
    src/jmap/cache/QueryWindowReadRepository.h
    src/jmap/cache/QueryWindowReader.h
    src/storage/messages/ThreadReadRepository.cpp
    src/jmap/cache/ThreadReadRepository.h
    src/jmap/cache/ThreadReader.h
    src/storage/messages/EmailRepository.cpp
    src/jmap/cache/EmailRepository.h
    src/jmap/cache/MailVault.h
    src/jmap/cache/EmailListSortSql.h
    src/storage/messages/MailSearchIndex.cpp
    src/jmap/cache/MailSearchIndex.h
    src/storage/messages/MailSearchReadRepository.cpp
    src/jmap/cache/MailSearchReadRepository.h
    src/jmap/cache/MailSearchReader.h
    src/storage/identity/IdentityRepository.cpp
    src/jmap/cache/IdentityRepository.h
    src/jmap/cache/IdentityReader.h
    src/storage/messages/MailboxRepository.cpp
    src/jmap/cache/MailboxRepository.h
    src/jmap/cache/MessageContentTypes.h
    src/storage/messages/MessageViewService.cpp
    src/jmap/cache/MessageViewService.h
    src/jmap/cache/MessageViewReader.h
    src/storage/messages/MimeMessageParser.cpp
    src/jmap/cache/MimeMessageParser.h
    src/storage/messages/RawMessageSourceRepository.cpp
    src/jmap/cache/RawMessageSourceRepository.h
    src/storage/account/SessionRepository.cpp
    src/jmap/cache/SessionRepository.h
    src/storage/sieve/SieveRepository.cpp
    src/jmap/cache/SieveRepository.h
    src/storage/sync/JmapTransportPreferenceRepository.cpp
    src/jmap/cache/JmapTransportPreferenceRepository.h
    src/storage/compose/SubmissionRepository.cpp
    src/jmap/cache/SubmissionRepository.h
    src/storage/sync/SyncStateRepository.cpp
    src/jmap/cache/SyncStateRepository.h
    src/storage/messages/ThreadRepository.cpp
    src/jmap/cache/ThreadRepository.h
    src/jmap/domain/MailEntities.h
    src/jmap/contacts/ContactTypes.cpp
    src/jmap/contacts/ContactTypes.h
    src/jmap/contacts/ContactInterchange.cpp
    src/jmap/contacts/ContactInterchange.h
    src/jmap/contacts/ContactResults.h
    src/jmap/contacts/ContactService.cpp
    src/jmap/contacts/ContactMediaService.h
    src/jmap/contacts/ContactMutationEngine.h
    src/jmap/contacts/ContactProtocolClient.h
    src/jmap/contacts/ContactSyncEngine.h
    src/jmap/contacts/ContactIdentityLookup.cpp
    src/jmap/contacts/ContactIdentityLookup.h
    src/jmap/domain/MailEntityParsers.cpp
    src/jmap/domain/MailEntityParsers.h
    src/jmap/query/MessageGrouping.cpp
    src/jmap/query/MessageGrouping.h
    src/jmap/query/EmailListSort.h
    src/jmap/query/QueryDiff.cpp
    src/jmap/query/QueryDiff.h
    src/jmap/render/HtmlMessageDocumentBuilder.cpp
    src/jmap/render/HtmlMessageDocumentBuilder.h
    src/jmap/render/HtmlTextExtractor.cpp
    src/jmap/render/HtmlTextExtractor.h
    src/jmap/render/InlineMessageUrl.cpp
    src/jmap/render/InlineMessageUrl.h
    src/jmap/search/EmailSearch.cpp
    src/jmap/search/EmailSearch.h
    src/jmap/sync/LongPollWorker.cpp
    src/jmap/sync/LongPollWorker.h
    src/jmap/sync/EventSourceLongPoll.cpp
    src/jmap/sync/EventSourceLongPoll.h
    src/jmap/sync/PushProtocol.cpp
    src/jmap/sync/PushProtocol.h
    src/jmap/sync/PushActivityTracker.cpp
    src/jmap/sync/PushActivityTracker.h
    src/jmap/sync/PushStreamSession.cpp
    src/jmap/sync/PushStreamSession.h
    src/jmap/sync/WebSocketPushChannel.cpp
    src/jmap/sync/WebSocketPushChannel.h
    src/jmap/sync/ConsistencyDomain.cpp
    src/jmap/sync/ConsistencyDomain.h
    src/jmap/sync/MailDeltaRefreshExecutor.cpp
    src/jmap/sync/MailDeltaRefreshExecutor.h
    src/jmap/sync/MailboxRefreshExecutor.cpp
    src/jmap/sync/MailboxRefreshExecutor.h
    src/jmap/sync/MailboxStateRefreshExecutor.cpp
    src/jmap/sync/MailboxStateRefreshExecutor.h
    src/jmap/sync/MailboxQueryDescriptor.cpp
    src/jmap/sync/MailboxQueryDescriptor.h
    src/jmap/sync/MutationJournal.cpp
    src/jmap/sync/MutationJournal.h
    src/jmap/sync/EmailMutationJournal.cpp
    src/jmap/sync/EmailMutationJournal.h
    src/jmap/sync/MailboxMutationJournal.cpp
    src/jmap/sync/MailboxMutationJournal.h
    src/jmap/contacts/ContactMutationJournal.cpp
    src/jmap/contacts/ContactMutationJournal.h
    src/jmap/calendar/CalendarMutationJournal.cpp
    src/jmap/calendar/CalendarMutationJournal.h
    src/jmap/contacts/AddressBookMutationJournal.cpp
    src/jmap/contacts/AddressBookMutationJournal.h
    src/jmap/sync/RefreshNotificationPlanner.cpp
    src/jmap/sync/RefreshNotificationPlanner.h
    src/jmap/sync/SyncPlanner.cpp
    src/jmap/sync/SyncPlanner.h
    src/jmap/sync/SyncReconciler.cpp
    src/jmap/sync/SyncReconciler.h
    src/jmap/auth/AccessTokenResolver.cpp
    src/jmap/auth/AccessTokenResolver.h
    src/jmap/auth/Auth.cpp
    src/jmap/auth/Auth.h
    src/jmap/JmapCore.cpp
    src/jmap/AccountBootstrapClient.h
    src/jmap/MessageContentClient.h
    src/jmap/api/LiveConnectionSettings.h
    src/jmap/api/SessionRefreshClient.h
    src/jmap/query/MailQueryClient.h
    src/jmap/query/MailQueryMaterializer.h
    src/jmap/sync/EmailMutationEngine.h
    src/jmap/sync/MailboxMutationEngine.h
    src/jmap/OperationError.cpp
    src/jmap/OperationError.h
    src/jmap/EmailMutation.h
    src/jmap/submission/ComposeService.cpp
    src/jmap/submission/ComposeService.h
    src/jmap/submission/ComposeRevisionGate.h
    src/jmap/submission/ComposeTypes.h
    src/jmap/submission/DraftSnapshotSerialization.h
    src/jmap/submission/DraftMutationJournal.cpp
    src/jmap/submission/DraftMutationJournal.h
    src/jmap/sieve/SieveService.cpp
    src/jmap/sieve/SieveMutationEngine.h
    src/jmap/sieve/SieveProtocolClient.h
    src/jmap/sieve/SieveCommandTypes.h
    src/jmap/sieve/SieveMutationJournal.cpp
    src/jmap/sieve/SieveMutationJournal.h
    src/jmap/sieve/SieveTypes.h
    src/gui/sieve/SieveEditorDialog.cpp
    src/gui/sieve/SieveEditorDialog.h
    tests/FixtureReader.cpp
    tests/FixtureReader.h
    tests/jmap/AccountRepositoryTest.cpp
    tests/jmap/AccountOnboardingServiceTest.cpp
    tests/jmap/AuthTest.cpp
    tests/jmap/ContactsMethodsTest.cpp
    tests/jmap/ContactRepositoryTest.cpp
    tests/jmap/ContactServiceTest.cpp
    tests/jmap/CalendarMethodsTest.cpp
    tests/jmap/CalendarColorTest.cpp
    tests/jmap/CalendarRepositoryTest.cpp
    tests/gui/MonthCalendarWidgetTest.cpp
    tests/gui/RecurrencePatternTest.cpp
    tests/gui/MessageViewPresentationTest.cpp
    tests/gui/PaginationTest.cpp
    tests/gui/PlainTextLinkifierTest.cpp
    tests/jmap/ComposeServiceTest.cpp
    tests/jmap/ConsistencyDomainTest.cpp
    tests/jmap/DatabaseTest.cpp
    tests/jmap/EmailRepositoryTest.cpp
    tests/jmap/HtmlMessageDocumentBuilderTest.cpp
    tests/jmap/HtmlTextExtractorTest.cpp
    tests/jmap/IdentityRepositoryTest.cpp
    tests/jmap/InlineMessageUrlTest.cpp
    tests/jmap/JmapCoreMessageContentTest.cpp
    tests/jmap/JmapCoreTest.cpp
    tests/jmap/JmapTransportPreferenceRepositoryTest.cpp
    tests/jmap/LongPollMailboxObserverTest.cpp
    tests/jmap/LongPollWorkerTest.cpp
    tests/jmap/MailMethodsTest.cpp
    tests/jmap/MailboxRepositoryTest.cpp
    tests/jmap/MailboxRefreshExecutorTest.cpp
    tests/jmap/MessageGroupingTest.cpp
    tests/jmap/MessageViewServiceTest.cpp
    tests/jmap/MethodCallerTest.cpp
    tests/jmap/MethodEnvelopeTest.cpp
    tests/jmap/MutationJournalTest.cpp
    tests/jmap/PreferredJmapMethodTransportTest.cpp
    tests/jmap/QueryDiffTest.cpp
    tests/jmap/QueryServiceTest.cpp
    tests/jmap/RefreshNotificationPlannerTest.cpp
    tests/jmap/MailEntityParsersTest.cpp
    tests/jmap/SessionClientTest.cpp
    tests/jmap/SessionRepositoryTest.cpp
    tests/jmap/SessionTest.cpp
    tests/jmap/SieveServiceTest.cpp
    tests/jmap/SyncStateRepositoryTest.cpp
    tests/jmap/SyncPlannerTest.cpp
    tests/jmap/SyncReconcilerTest.cpp
    tests/jmap/ThreadRepositoryTest.cpp
    tests/app/CacheAccessBarrierTest.cpp
    tests/app/OAuthRefreshSingleFlightTest.cpp
    tests/jmap/ReadOnlyCacheSurfaceTest.cpp
)

add_library(javelin_jmap STATIC
    src/jmap/EmailMutation.h
    src/jmap/render/HtmlBodyEmbedding.cpp
    src/jmap/submission/DraftInlineImageStorage.cpp
    src/jmap/submission/DraftInlineImageStorage.h
    src/jmap/render/HtmlBodyEmbedding.h
    src/jmap/api/CalendarMethods.cpp
    src/jmap/api/CalendarMethods.h
    src/jmap/calendar/CalendarTypes.h
    src/jmap/calendar/CalendarCommandTypes.h
    src/jmap/calendar/CalendarColor.cpp
    src/jmap/calendar/CalendarColor.h
    src/jmap/calendar/CalendarService.cpp
    src/jmap/calendar/CalendarCacheReader.h
    src/jmap/calendar/CalendarMutationEngine.h
    src/jmap/calendar/CalendarProtocolClient.h
    src/jmap/calendar/CalendarReader.h
    src/jmap/calendar/CalendarSyncEngine.h
    src/storage/calendar/CalendarRepository.cpp
    src/jmap/cache/CalendarRepository.h
    src/jmap/sync/PreferredStateChangeSource.cpp
    src/jmap/sync/PreferredStateChangeSource.h
    src/jmap/sync/EmailMutationQueue.cpp
    src/jmap/sync/EmailMutationQueue.h
    src/jmap/api/Cancellation.cpp
    src/jmap/api/Cancellation.h
    src/storage/messages/SearchWindowRepository.cpp
    src/jmap/cache/SearchWindowRepository.h
    src/storage/messages/MailboxWindowRepository.cpp
    src/jmap/cache/MailboxWindowRepository.h
    src/storage/sync/NotificationRepository.cpp
    src/jmap/cache/NotificationRepository.h
    src/storage/sync/NotificationDispatchRepository.cpp
    src/jmap/cache/NotificationDispatchRepository.h
    src/storage/calendar/CalendarNotificationRepository.cpp
    src/jmap/cache/CalendarNotificationRepository.h
    src/storage/calendar/CalendarInvitationRepository.cpp
    src/jmap/cache/CalendarInvitationRepository.h
    src/jmap/sync/StateChangeSource.h
    src/jmap/sync/MailboxInterestRegistry.cpp
    src/jmap/sync/MailboxInterestRegistry.h
    src/jmap/api/SessionDiscovery.cpp
    src/jmap/api/SessionDiscovery.h
    src/jmap/api/ContactsMethods.cpp
    src/jmap/api/ContactsMethods.h
    src/jmap/api/Error.cpp
    src/jmap/api/Error.h
    src/jmap/api/BlobUpload.cpp
    src/jmap/api/BlobUpload.h
    src/jmap/api/MailMethods.cpp
    src/jmap/api/MailMethods.h
    src/jmap/api/MethodCaller.cpp
    src/jmap/api/MethodCaller.h
    src/jmap/api/JmapMethodTransport.cpp
    src/jmap/api/JmapMethodTransport.h
    src/jmap/api/MethodEnvelope.cpp
    src/jmap/api/MethodEnvelope.h
    src/jmap/api/PatchObject.cpp
    src/jmap/api/PatchObject.h
    src/jmap/api/RequestBuilder.h
    src/jmap/api/ResponseReader.h
    src/jmap/api/Session.cpp
    src/jmap/api/Session.h
    src/jmap/api/SessionClient.cpp
    src/jmap/api/SessionClient.h
    src/jmap/api/SessionParser.cpp
    src/jmap/api/SessionParser.h
    src/jmap/contacts/ContactTypes.cpp
    src/jmap/contacts/ContactTypes.h
    src/jmap/contacts/ContactInterchange.cpp
    src/jmap/contacts/ContactInterchange.h
    src/jmap/contacts/ContactResults.h
    src/jmap/contacts/ContactService.cpp
    src/jmap/contacts/ContactMediaService.h
    src/jmap/contacts/ContactMutationEngine.h
    src/jmap/contacts/ContactProtocolClient.h
    src/jmap/contacts/ContactSyncEngine.h
    src/jmap/contacts/ContactIdentityLookup.cpp
    src/jmap/contacts/ContactIdentityLookup.h
    src/jmap/api/Transport.cpp
    src/jmap/api/Transport.h
    src/storage/account/AccountRepository.cpp
    src/jmap/cache/AccountRepository.h
    src/storage/account/AccountReadRepository.cpp
    src/jmap/cache/AccountReadRepository.h
    src/storage/messages/MailTagReadRepository.cpp
    src/jmap/cache/MailTagReadRepository.h
    src/jmap/cache/MailTagReader.h
    src/storage/messages/MailboxStatisticsReadRepository.cpp
    src/jmap/cache/MailboxStatisticsReadRepository.h
    src/jmap/cache/MailboxStatisticsReader.h
    src/storage/messages/MailboxMessageReadRepository.cpp
    src/jmap/cache/MailboxMessageReadRepository.h
    src/jmap/cache/MailboxMessageReader.h
    src/storage/messages/MailboxFilterReadRepository.cpp
    src/jmap/cache/MailboxFilterReadRepository.h
    src/jmap/cache/MailboxFilterReader.h
    src/storage/messages/MailboxReadRepository.cpp
    src/jmap/cache/MailboxReadRepository.h
    src/storage/compose/ComposeSessionRepository.cpp
    src/jmap/cache/ComposeSessionRepository.h
    src/storage/contacts/ContactRepository.cpp
    src/jmap/cache/ContactRepository.h
    src/jmap/cache/ContactReader.h
    src/storage/messages/MessageListReadSupport.cpp
    src/jmap/cache/MessageListReadSupport.h
    src/jmap/cache/MessageListReadTypes.h
    src/storage/messages/MessageSummaryReadRepository.cpp
    src/jmap/cache/MessageSummaryReadRepository.h
    src/jmap/cache/MessageSummaryReader.h
    src/storage/messages/SearchWindowReadRepository.cpp
    src/jmap/cache/SearchWindowReadRepository.h
    src/storage/messages/MailboxWindowReadRepository.cpp
    src/jmap/cache/MailboxWindowReadRepository.h
    src/storage/messages/QueryWindowReadRepository.cpp
    src/jmap/cache/QueryWindowReadRepository.h
    src/jmap/cache/QueryWindowReader.h
    src/storage/messages/ThreadReadRepository.cpp
    src/jmap/cache/ThreadReadRepository.h
    src/jmap/cache/ThreadReader.h
    src/storage/messages/EmailRepository.cpp
    src/jmap/cache/EmailRepository.h
    src/jmap/cache/MailVault.h
    src/jmap/cache/EmailListSortSql.h
    src/storage/messages/MailSearchIndex.cpp
    src/jmap/cache/MailSearchIndex.h
    src/storage/messages/MailSearchReadRepository.cpp
    src/jmap/cache/MailSearchReadRepository.h
    src/jmap/cache/MailSearchReader.h
    src/storage/identity/IdentityRepository.cpp
    src/jmap/cache/IdentityRepository.h
    src/jmap/cache/IdentityReader.h
    src/storage/messages/MailboxRepository.cpp
    src/jmap/cache/MailboxRepository.h
    src/jmap/cache/MessageContentTypes.h
    src/jmap/cache/MessageViewService.h
    src/jmap/cache/MessageViewReader.h
    src/jmap/cache/MimeMessageParser.h
    src/jmap/cache/RawMessageSourceRepository.h
    src/storage/messages/RawMessageSourceRepository.cpp
    src/storage/account/SessionRepository.cpp
    src/jmap/cache/SessionRepository.h
    src/storage/sieve/SieveRepository.cpp
    src/jmap/cache/SieveRepository.h
    src/storage/sync/JmapTransportPreferenceRepository.cpp
    src/jmap/cache/JmapTransportPreferenceRepository.h
    src/storage/compose/SubmissionRepository.cpp
    src/jmap/cache/SubmissionRepository.h
    src/storage/sync/SyncStateRepository.cpp
    src/jmap/cache/SyncStateRepository.h
    src/storage/messages/ThreadRepository.cpp
    src/jmap/cache/ThreadRepository.h
    src/jmap/domain/MailEntities.h
    src/jmap/domain/MailEntityParsers.cpp
    src/jmap/domain/MailEntityParsers.h
    src/jmap/query/MessageGrouping.cpp
    src/jmap/query/MessageGrouping.h
    src/jmap/query/EmailListSort.h
    src/jmap/query/QueryDiff.cpp
    src/jmap/query/QueryDiff.h
    src/jmap/render/HtmlMessageDocumentBuilder.h
    src/jmap/render/InlineMessageUrl.h
    src/jmap/search/EmailSearch.cpp
    src/jmap/search/EmailSearch.h
    src/jmap/sync/LongPollWorker.cpp
    src/jmap/sync/LongPollWorker.h
    src/jmap/sync/EventSourceLongPoll.cpp
    src/jmap/sync/EventSourceLongPoll.h
    src/jmap/sync/PushProtocol.cpp
    src/jmap/sync/PushProtocol.h
    src/jmap/sync/PushActivityTracker.cpp
    src/jmap/sync/PushActivityTracker.h
    src/jmap/sync/PushStreamSession.cpp
    src/jmap/sync/PushStreamSession.h
    src/jmap/sync/WebSocketPushChannel.cpp
    src/jmap/sync/WebSocketPushChannel.h
    src/jmap/sync/ConsistencyDomain.cpp
    src/jmap/sync/ConsistencyDomain.h
    src/jmap/sync/MailDeltaRefreshExecutor.cpp
    src/jmap/sync/MailDeltaRefreshExecutor.h
    src/jmap/sync/MailboxRefreshExecutor.cpp
    src/jmap/sync/MailboxRefreshExecutor.h
    src/jmap/sync/MailboxStateRefreshExecutor.cpp
    src/jmap/sync/MailboxStateRefreshExecutor.h
    src/jmap/sync/MailboxQueryDescriptor.cpp
    src/jmap/sync/MailboxQueryDescriptor.h
    src/jmap/sync/MutationJournal.cpp
    src/jmap/sync/MutationJournal.h
    src/jmap/sync/EmailMutationJournal.cpp
    src/jmap/sync/EmailMutationJournal.h
    src/jmap/sync/MailboxMutationJournal.cpp
    src/jmap/sync/MailboxMutationJournal.h
    src/jmap/contacts/ContactMutationJournal.cpp
    src/jmap/contacts/ContactMutationJournal.h
    src/jmap/calendar/CalendarMutationJournal.cpp
    src/jmap/calendar/CalendarMutationJournal.h
    src/jmap/identity/IdentityCommandTypes.h
    src/jmap/identity/IdentityMutationJournal.cpp
    src/jmap/identity/IdentityMutationJournal.h
    src/jmap/identity/IdentityService.cpp
    src/jmap/identity/IdentityService.h
    src/jmap/contacts/AddressBookMutationJournal.cpp
    src/jmap/contacts/AddressBookMutationJournal.h
    src/jmap/sync/RefreshNotificationPlanner.cpp
    src/jmap/sync/RefreshNotificationPlanner.h
    src/jmap/sync/SyncPlanner.cpp
    src/jmap/sync/SyncPlanner.h
    src/jmap/sync/SyncReconciler.cpp
    src/jmap/sync/SyncReconciler.h
    src/jmap/auth/AccessTokenResolver.cpp
    src/jmap/auth/AccessTokenResolver.h
    src/jmap/auth/Auth.cpp
    src/jmap/auth/Auth.h
    src/jmap/auth/AccountOnboardingService.cpp
    src/jmap/auth/AccountOnboardingService.h
    src/app/OnboardingTypes.h
    src/jmap/JmapCore.cpp
    src/jmap/AccountBootstrapClient.h
    src/jmap/MessageContentClient.h
    src/jmap/api/LiveConnectionSettings.h
    src/jmap/api/SessionRefreshClient.h
    src/jmap/query/MailQueryClient.h
    src/jmap/query/MailQueryMaterializer.h
    src/jmap/sync/EmailMutationEngine.h
    src/jmap/sync/MailboxMutationEngine.h
    src/jmap/OperationError.cpp
    src/jmap/OperationError.h
    src/jmap/submission/ComposeService.cpp
    src/jmap/submission/ComposeService.h
    src/jmap/submission/ComposeRevisionGate.h
    src/jmap/submission/ComposeTypes.h
    src/jmap/submission/DraftSnapshotSerialization.h
    src/jmap/submission/DraftMutationJournal.cpp
    src/jmap/submission/DraftMutationJournal.h
    src/jmap/sieve/SieveService.cpp
    src/jmap/sieve/SieveMutationEngine.h
    src/jmap/sieve/SieveProtocolClient.h
    src/jmap/sieve/SieveMutationJournal.cpp
    src/jmap/sieve/SieveMutationJournal.h
    src/jmap/sieve/SieveTypes.h
)
# The mail model is listed above for source discoverability, but it has one production owner.
# Remove those entries from the protocol target and consume the model through its target instead.
get_target_property(javelin_jmap_sources javelin_jmap SOURCES)
get_target_property(javelin_mail_model_sources javelin_mail_model SOURCES)
list(REMOVE_ITEM javelin_jmap_sources ${javelin_mail_model_sources})
set_property(TARGET javelin_jmap PROPERTY SOURCES ${javelin_jmap_sources})
target_include_directories(javelin_jmap PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)
target_link_libraries(javelin_jmap
    PUBLIC
        javelin_cache_read
        javelin_mail_model
        QCoro6::Coro
        Qt::Core
        Qt::Network
        Qt::Sql
        Qt::WebSockets
        KPim6::Mime
    PRIVATE
        KF6::Codecs
        QCoro6::Network
        Qt::Concurrent
        glaze::glaze
)
javelin_set_project_warnings(javelin_jmap)
javelin_enable_static_analysis(javelin_jmap)
javelin_enable_sanitizers(javelin_jmap)

add_library(javelin_translation STATIC
    src/gui/translation/TranslationTypes.h
    src/gui/translation/TranslationSettingsPolicy.cpp
    src/gui/translation/TranslationSettingsStore.h
    src/gui/translation/TranslationSettingsStore.cpp
    src/gui/translation/TranslationCache.h
    src/gui/translation/TranslationCache.cpp
    src/gui/translation/TranslationBackend.h
    src/gui/translation/GoogleTranslationBackend.h
    src/gui/translation/GoogleTranslationBackend.cpp
    src/gui/translation/TranslationService.h
    src/gui/translation/TranslationService.cpp
    src/gui/translation/LanguageDetection.h
    src/gui/translation/LanguageDetection.cpp
    src/gui/translation/FastTextLanguageDetector.h
    src/gui/translation/FastTextLanguageDetector.cpp
)
if(JAVELIN_ENABLE_BERGAMOT_TRANSLATION)
    target_sources(javelin_translation PRIVATE
        src/gui/translation/TranslationModelManifest.h
        src/gui/translation/TranslationModelManifest.cpp
        src/gui/translation/TranslationModelStore.h
        src/gui/translation/TranslationModelStore.cpp
        src/gui/translation/BergamotTranslationBackend.h
        src/gui/translation/BergamotTranslationBackend.cpp
    )
endif()
target_include_directories(javelin_translation PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)
target_link_libraries(javelin_translation
    PUBLIC
        QCoro6::Core
        Qt::Core
    PRIVATE
        QCoro6::Network
        Qt::Concurrent
        Qt::Network
        Qt::Sql
        KF6::I18n
)
if(JAVELIN_ENABLE_FASTTEXT_LANGUAGE_DETECTION)
    target_link_libraries(javelin_translation PRIVATE fasttext_javelin)
    target_compile_definitions(javelin_translation
        PUBLIC
            JAVELIN_ENABLE_FASTTEXT_LANGUAGE_DETECTION=1
            JAVELIN_FASTTEXT_LANGUAGE_MODEL_PATH=\"${JAVELIN_FASTTEXT_LANGUAGE_MODEL_PATH}\"
    )
else()
    target_compile_definitions(javelin_translation
        PUBLIC JAVELIN_ENABLE_FASTTEXT_LANGUAGE_DETECTION=0)
endif()
if(JAVELIN_ENABLE_BERGAMOT_TRANSLATION)
    target_link_libraries(javelin_translation PRIVATE
        bergamot-translator-source
        PkgConfig::ZSTD
    )
    target_compile_definitions(javelin_translation PUBLIC
        JAVELIN_ENABLE_BERGAMOT_TRANSLATION=1
        JAVELIN_SSPLIT_PREFIX_DIR=\"${CMAKE_INSTALL_PREFIX}/${KDE_INSTALL_DATADIR}/javelinmail/models/ssplit\"
    )
    target_include_directories(javelin_translation SYSTEM PRIVATE
        "${JAVELIN_BERGAMOT_SOURCE_DIR}/inference"
        "${JAVELIN_BERGAMOT_SOURCE_DIR}/inference/src"
    )
else()
    target_compile_definitions(javelin_translation PUBLIC
        JAVELIN_ENABLE_BERGAMOT_TRANSLATION=0)
endif()
target_compile_options(javelin_translation PRIVATE -fexceptions)
javelin_set_project_warnings(javelin_translation)
javelin_enable_static_analysis(javelin_translation)
javelin_enable_sanitizers(javelin_translation)

add_library(javelin_gui STATIC
    src/gui/compose/AddressSuggestionStore.cpp
    src/gui/compose/AddressSuggestionStore.h
    src/gui/messageview/InlineMessageSchemeHandler.cpp
    src/gui/messageview/InlineMessageSchemeHandler.h
    src/app/MessageSubject.h
    src/gui/tasks/WorkTaskModel.cpp
    src/gui/tasks/WorkTaskModel.h
    src/app/WorkTaskPort.h
    src/gui/IconUtils.cpp
    src/gui/IconUtils.h
    src/gui/developer/DeveloperOptionsDialog.cpp
    src/gui/developer/DeveloperOptionsDialog.h
    src/gui/logging/LogViewerDialog.cpp
    src/gui/logging/LogViewerDialog.h
    src/gui/mailboxes/MailboxPresentation.cpp
    src/gui/mailboxes/MailboxPresentation.h
    src/gui/mailboxes/MailboxPropertiesDialog.cpp
    src/gui/mailboxes/MailboxPropertiesDialog.h
    src/gui/mailboxes/MailboxSelection.cpp
    src/gui/mailboxes/MailboxSelection.h
    src/gui/compose/ComposeTabWidget.cpp
    src/gui/compose/ComposeTabWidget.h
    src/gui/compose/AttachmentController.cpp
    src/gui/compose/AttachmentController.h
    src/gui/compose/ComposeAutosaveController.cpp
    src/gui/compose/ComposeAutosaveController.h
    src/gui/compose/ComposeIdentityController.cpp
    src/gui/compose/ComposeIdentityController.h
    src/gui/compose/InlineImageController.cpp
    src/gui/compose/InlineImageController.h
    src/gui/compose/ComposeRecipientController.cpp
    src/gui/compose/ComposeRecipientController.h
    src/gui/compose/IdentityPresentation.cpp
    src/gui/compose/IdentityPresentation.h
    src/gui/compose/SignatureController.cpp
    src/gui/compose/SignatureController.h
    src/gui/compose/SignatureTrackingPolicy.cpp
    src/gui/compose/SignatureTrackingPolicy.h
    src/gui/compose/SendingIdentitiesDialog.cpp
    src/gui/compose/SendingIdentitiesDialog.h
    src/gui/compose/UndoSendDialog.cpp
    src/gui/compose/UndoSendDialog.h
    src/gui/identity/IdentityManagerDialog.cpp
    src/gui/identity/IdentityManagerDialog.h
    src/gui/compose/ComposeUiPreferences.cpp
    src/gui/compose/ComposeUiPreferences.h
    src/gui/compose/ComposerInlineImageCodec.cpp
    src/gui/compose/ComposerInlineImageCodec.h
    src/gui/compose/JavelinComposerEdit.cpp
    src/gui/compose/JavelinComposerEdit.h
    src/gui/compose/ComposeBodyConverter.cpp
    src/gui/compose/ComposeBodyConverter.h
    src/gui/contacts/AddressBookController.cpp
    src/gui/contacts/AddressBookController.h
    src/gui/contacts/ContactDetailsView.cpp
    src/gui/contacts/ContactDetailsView.h
    src/gui/contacts/ContactEditor.cpp
    src/gui/contacts/ContactEditor.h
    src/gui/contacts/ContactGroupController.cpp
    src/gui/contacts/ContactGroupController.h
    src/gui/contacts/ContactPhotoController.cpp
    src/gui/contacts/ContactPhotoController.h
    src/gui/contacts/ContactsBrowser.cpp
    src/gui/contacts/ContactsBrowser.h
    src/gui/contacts/ContactsManagerWidget.cpp
    src/gui/contacts/ContactsManagerWidget.h
    src/gui/contacts/ContactsViewState.h
    src/gui/widgets/EmailAddressLineEdit.cpp
    src/gui/widgets/EmailAddressLineEdit.h
    src/gui/calendar/MonthCalendarWidget.cpp
    src/gui/calendar/MonthCalendarWidget.h
    src/gui/calendar/MonthCalendarLayout.cpp
    src/gui/calendar/MonthCalendarLayout.h
    src/gui/calendar/DayAgendaDialog.cpp
    src/gui/calendar/DayAgendaDialog.h
    src/gui/calendar/CalendarEventContextMenuEditorDialog.cpp
    src/gui/calendar/CalendarEventContextMenuEditorDialog.h
    src/gui/calendar/CalendarEventContextMenuLayout.cpp
    src/gui/calendar/CalendarEventContextMenuLayout.h
    src/gui/calendar/EventDialog.cpp
    src/gui/calendar/EventDialog.h
    src/gui/calendar/CalendarPresentation.cpp
    src/gui/calendar/CalendarPresentation.h
    src/gui/calendar/RecurrenceDialog.cpp
    src/gui/calendar/RecurrenceDialog.h
    src/gui/calendar/RecurrencePattern.cpp
    src/gui/calendar/RecurrencePattern.h
    src/gui/mailboxes/MailboxTreeDelegate.cpp
    src/gui/mailboxes/MailboxTreeDelegate.h
    src/gui/mailboxes/MailboxTreeModel.cpp
    src/gui/mailboxes/MailboxTreeModel.h
    src/gui/mailboxes/MailboxTreeView.cpp
    src/gui/mailboxes/MailboxTreeView.h
    src/gui/messages/InfiniteScroll.h
    src/gui/messages/MessageListWindowPersistence.h
    src/gui/messages/MessageListModel.cpp
    src/gui/messages/MessageListModel.h
    src/gui/messages/MessageListPanePresenter.cpp
    src/gui/messages/MessageListPanePresenter.h
    src/gui/messages/MessageListDelegate.cpp
    src/gui/messages/MessageListDelegate.h
    src/gui/messages/MessageDragListView.cpp
    src/gui/messages/MessageDragListView.h
    src/gui/messages/MessageActionSelection.cpp
    src/gui/messages/MessageActionSelection.h
    src/gui/messages/MessageSelectionRestoration.cpp
    src/gui/messages/MessageSelectionRestoration.h
    src/gui/messageview/HtmlMessageView.cpp
    src/gui/messageview/HtmlMessageView.h
    src/gui/messageview/MessageBannerWidget.cpp
    src/gui/messageview/MessageBannerWidget.h
    src/gui/messageview/MessageAttachmentPanel.cpp
    src/gui/messageview/MessageAttachmentPanel.h
    src/gui/messageview/MessageBannerCoordinator.cpp
    src/gui/messageview/MessageBannerCoordinator.h
    src/gui/messageview/MessageBodyPresenter.cpp
    src/gui/messageview/MessageBodyPresenter.h
    src/gui/messageview/MessageTranslationController.cpp
    src/gui/messageview/MessageTranslationController.h
    src/gui/messageview/RemoteContentController.cpp
    src/gui/messageview/RemoteContentController.h
    src/gui/messageview/MessageViewContainer.cpp
    src/gui/messageview/MessageViewContainer.h
    src/gui/messageview/MessageViewPresentation.h
    src/gui/messageview/PlainTextLinkifier.cpp
    src/gui/messageview/PlainTextLinkifier.h
    src/gui/search/AdvancedSearchDialog.cpp
    src/gui/search/AdvancedSearchDialog.h
    src/gui/search/SearchSessionPersistence.cpp
    src/gui/search/SearchSessionPersistence.h
    src/gui/settings/PreferencesDialog.cpp
    src/gui/settings/PreferencesDialog.h
    src/gui/settings/PreferencesPages.cpp
    src/gui/settings/PreferencesPages.h
    src/gui/onboarding/FirstRunWizard.cpp
    src/gui/onboarding/FirstRunWizard.h
    src/gui/shell/ElidingLabel.cpp
    src/gui/shell/ElidingLabel.h
    src/gui/shell/FocusedCommandRouter.cpp
    src/gui/shell/FocusedCommandRouter.h
    src/gui/shell/MessageFileUtils.cpp
    src/gui/shell/MessageFileUtils.h
    src/gui/shell/MessageFileController.cpp
    src/gui/shell/MessageFileController.h
    src/gui/shell/MessageCommandController.cpp
    src/gui/shell/MessageCommandController.h
    src/gui/shell/MessageListTabController.cpp
    src/gui/shell/MessageListTabController.h
    src/gui/shell/MessageListTabPolicy.cpp
    src/gui/shell/MessageListTabPolicy.h
    src/gui/shell/TabActivationPolicy.cpp
    src/gui/shell/TabActivationPolicy.h
    src/gui/shell/TabPersistence.cpp
    src/gui/shell/TabPersistencePolicy.cpp
    src/gui/shell/TabPersistence.h
    src/gui/shell/TabWorkspace.cpp
    src/gui/shell/TabWorkspacePolicy.cpp
    src/gui/shell/TabWorkspace.h
    src/gui/shell/TabBarPresenter.cpp
    src/gui/shell/TabBarPresenter.h
    src/gui/shell/MainWindowStateStore.cpp
    src/gui/shell/MainWindowStateStore.h
    src/gui/shell/LayeredStatusBar.cpp
    src/gui/shell/LayeredStatusBar.h
    src/gui/shell/MainWindow.cpp
    src/gui/shell/MainWindow.h
    src/gui/shell/CalendarTabController.cpp
    src/gui/shell/CalendarTabController.h
    src/gui/shell/ContactsTabController.cpp
    src/gui/shell/ContactsTabController.h
    src/gui/shell/ComposeTabController.cpp
    src/gui/shell/ComposeTabController.h
    src/gui/shell/ComposeTabPolicy.cpp
    src/gui/shell/ComposeTabPolicy.h
    src/gui/shell/MessageActionPolicy.cpp
    src/gui/shell/MessageActionPolicy.h
    src/gui/shell/EmailContextMenuLayout.cpp
    src/gui/shell/EmailContextMenuLayout.h
    src/gui/shell/EmailContextMenuEditorDialog.cpp
    src/gui/shell/EmailContextMenuEditorDialog.h
    src/gui/shell/MessageListPresentationPolicy.cpp
    src/gui/shell/MessageListPresentationPolicy.h
    src/gui/shell/MessageListTabBindingPresenter.cpp
    src/gui/shell/MessageListTabBindingPresenter.h
    src/gui/shell/MessageListTabPresenter.cpp
    src/gui/shell/MessageListTabPresenter.h
    src/gui/shell/MessageNavigationController.cpp
    src/gui/shell/MessageNavigationController.h
    src/gui/shell/MessageNavigationPolicy.cpp
    src/gui/shell/MessageNavigationPolicy.h
    src/gui/settings/ConnectionSettings.h
    src/gui/settings/ConnectionSettingsAdapter.cpp
    src/gui/settings/ConnectionSettingsAdapter.h
    src/gui/shell/AccountRefreshController.cpp
    src/gui/shell/AccountRefreshController.h
    src/gui/shell/AuthenticationPromptCoordinator.cpp
    src/gui/shell/AuthenticationPromptCoordinator.h
    src/gui/shell/MessageContentController.cpp
    src/gui/shell/MessageContentController.h
    src/gui/shell/MessageContentPolicy.cpp
    src/gui/shell/MessageContentPolicy.h
    src/gui/shell/MessageSelectionController.cpp
    src/gui/shell/MessageSelectionController.h
    src/gui/shell/QuickFilterController.cpp
    src/gui/shell/QuickFilterController.h
    src/gui/shell/MailActionController.cpp
    src/gui/shell/MailActionController.h
    src/gui/shell/MailWorkspaceController.cpp
    src/gui/shell/MailWorkspaceController.h
    src/gui/shell/ThemeController.cpp
    src/gui/shell/ThemeController.h
    src/gui/sieve/SieveEditorDialog.cpp
    src/gui/sieve/SieveEditorDialog.h
    src/gui/tasks/TaskCenterDialog.cpp
    src/gui/tasks/TaskCenterDialog.h
)
target_sources(javelin_gui PRIVATE
    src/app/SettingsApplicationPorts.h
    src/gui/messages/MessageConfirmationPresentation.cpp
    src/gui/messages/MessageConfirmationPresentation.h
    src/gui/messageview/MessageAppearance.cpp
    src/gui/messageview/MessageAppearance.h
    src/gui/settings/GuiSettings.cpp
    src/gui/settings/GuiSettings.h
    src/gui/settings/WorkspaceSettingsPort.h
)
qt_add_resources(javelin_gui "javelin_darkreader"
    PREFIX "/vendor/darkreader"
    BASE "res/vendor/darkreader"
    FILES
        "res/vendor/darkreader/darkreader.js"
)

file(GLOB_RECURSE javelin_gui_boundary_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gui/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gui/*.h"
)
foreach(gui_source IN LISTS javelin_gui_boundary_sources)
    file(READ "${gui_source}" gui_source_contents)
    if(gui_source_contents MATCHES "#include \"jmap/api/(Transport|MethodEnvelope)\\.h\"" OR
       gui_source_contents MATCHES "#include \"jmap/cache/(AccountRepository|ContactRepository|IdentityRepository|MessageViewService|QueryService|Database)\\.h\"" OR
       gui_source_contents MATCHES "#include \"jmap/calendar/(CalendarCacheReader|CalendarProtocolClient|CalendarSyncEngine|CalendarMutationEngine)\\.h\"" OR
       gui_source_contents MATCHES "#include \"jmap/contacts/(ContactProtocolClient|ContactSyncEngine|ContactMutationEngine|ContactMediaService)\\.h\"" OR
       gui_source_contents MATCHES "#include \"jmap/sieve/(SieveProtocolClient|SieveMutationEngine)\\.h\"" OR
       gui_source_contents MATCHES "javelin::jmap::cache::Database(Connection|Transaction|WriteScope)" OR
       gui_source_contents MATCHES "#include \"jmap/sync/(MailboxRefreshExecutor|MailboxStateRefreshExecutor)\\.h\"" OR
       gui_source_contents MATCHES "#include \"app/(AccountRuntimeManager|MailQueryApplicationService|MailMutationApplicationService|MessageContentApplicationService|MailNotificationService|ContactApplicationService|CalendarApplicationService|SieveApplicationService)\\.h\"" OR
       gui_source_contents MATCHES "javelin::app::(AccountRuntimeManager|MailQueryApplicationService|MailMutationApplicationService|MessageContentApplicationService|MailNotificationService|ContactApplicationService|CalendarApplicationService|SieveApplicationService)" OR
       gui_source_contents MATCHES "#include \"app/MailApplicationService\\.h\"" OR
       gui_source_contents MATCHES "javelin::app::MailApplicationService" OR
       gui_source_contents MATCHES "AccountSyncConfiguration" OR
       gui_source_contents MATCHES "javelin::jmap::MailboxPage(Result|Summary)" OR
       gui_source_contents MATCHES "javelin::jmap::MessageSearch(Result|Summary)" OR
       gui_source_contents MATCHES "javelin::jmap::LiveConnectionSettings" OR
       gui_source_contents MATCHES "javelin::jmap::api::((AddressBook|ContactCard)(Set|Copy)Request|ContactDocument)" OR
       gui_source_contents MATCHES "javelin::jmap::submission::ComposeService" OR
       gui_source_contents MATCHES "javelin::app::(AccountSyncCoordinator|LongPoll(Service|Coordinator|AccountConfiguration))" OR
       gui_source_contents MATCHES "#include \"app/(DaemonServices|ProcessServices|WorkScheduler|MessageNavigationCoordinator)\\.h\"" OR
       gui_source_contents MATCHES "javelin::app::(DaemonServices|ProcessServices|WorkScheduler|MessageNavigationCoordinator|UndoManager)")
        message(FATAL_ERROR "GUI boundary violation in ${gui_source}")
    endif()
endforeach()

set(javelin_main_window_shell_sources
    src/gui/shell/MainWindow.cpp
    src/gui/shell/MainWindow.h
)
foreach(main_window_source IN LISTS javelin_main_window_shell_sources)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${main_window_source}" main_window_source_contents)
    if(main_window_source_contents MATCHES
           "quickFilterPinned|pinnedQuickFilter|lastQuickFilter|authenticationRequiredAccount|authenticationPromptOpen|paletteRefreshPending" OR
       main_window_source_contents MATCHES
           "javelin::jmap::cache::ContactReader|javelin::jmap::calendar::CalendarReader|javelin::app::(CalendarCommandPort|ContactCommandPort|ComposeCommandPort)" OR
       main_window_source_contents MATCHES
           "setSelectionFlagged|setSelectionJunk|markSelectionUnread|archiveSelection|populateDestinationMenus|saveTagDefinition|listEmailKeywordMemberships")
        message(FATAL_ERROR
                "MainWindow must remain a shell; feature policy belongs in focused GUI controllers: ${main_window_source}")
    endif()
endforeach()

set(javelin_contacts_gui_sources
    src/gui/contacts/ContactsManagerWidget.cpp
    src/gui/contacts/ContactsManagerWidget.h
    src/gui/shell/ContactsTabController.cpp
    src/gui/shell/ContactsTabController.h
)
foreach(contacts_gui_source IN LISTS javelin_contacts_gui_sources)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${contacts_gui_source}" contacts_gui_source_contents)
    if(contacts_gui_source_contents MATCHES "javelin::app::(ContactCommandService|ContactApplicationService|MailApplicationService)")
        message(FATAL_ERROR "Contacts GUI must depend on application ports: ${contacts_gui_source}")
    endif()
endforeach()

set(javelin_mail_command_gui_sources
    src/gui/shell/MessageCommandController.cpp
    src/gui/shell/MessageCommandController.h
)
foreach(mail_command_gui_source IN LISTS javelin_mail_command_gui_sources)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${mail_command_gui_source}"
         mail_command_gui_source_contents)
    if(mail_command_gui_source_contents MATCHES
       "javelin::app::(MailApplicationService|MailMutationApplicationService)|app/(MailApplicationService|MailMutationApplicationService)\\.h")
        message(FATAL_ERROR
                "Message command GUI must depend on MailCommandPort: ${mail_command_gui_source}")
    endif()
endforeach()

set(javelin_compose_command_gui_sources
    src/gui/compose/ComposeTabWidget.cpp
    src/gui/compose/ComposeTabWidget.h
    src/gui/compose/AttachmentController.cpp
    src/gui/compose/AttachmentController.h
    src/gui/compose/ComposeAutosaveController.cpp
    src/gui/compose/ComposeAutosaveController.h
    src/gui/compose/ComposeIdentityController.cpp
    src/gui/compose/ComposeIdentityController.h
    src/gui/compose/InlineImageController.cpp
    src/gui/compose/InlineImageController.h
    src/gui/compose/ComposeRecipientController.cpp
    src/gui/compose/ComposeRecipientController.h
    src/gui/compose/SignatureController.cpp
    src/gui/compose/SignatureController.h
    src/gui/shell/ComposeTabController.cpp
    src/gui/shell/ComposeTabController.h
)
foreach(compose_command_gui_source IN LISTS javelin_compose_command_gui_sources)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${compose_command_gui_source}"
         compose_command_gui_source_contents)
    if(compose_command_gui_source_contents MATCHES
       "javelin::app::ComposeService|app/ComposeService\\.h")
        message(FATAL_ERROR
                "Compose GUI must depend on ComposeCommandPort: ${compose_command_gui_source}")
    endif()
endforeach()

set(javelin_calendar_command_gui_sources
    src/gui/shell/CalendarTabController.cpp
    src/gui/shell/CalendarTabController.h
)
foreach(calendar_command_gui_source IN LISTS javelin_calendar_command_gui_sources)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${calendar_command_gui_source}"
         calendar_command_gui_source_contents)
    if(calendar_command_gui_source_contents MATCHES
       "javelin::app::(MailApplicationService|CalendarApplicationService)|app/(MailApplicationService|CalendarApplicationService)\\.h")
        message(FATAL_ERROR
                "Calendar GUI must depend on CalendarCommandPort: ${calendar_command_gui_source}")
    endif()
endforeach()

set(javelin_sieve_command_gui_sources
    src/gui/sieve/SieveEditorDialog.cpp
    src/gui/sieve/SieveEditorDialog.h
)
foreach(sieve_command_gui_source IN LISTS javelin_sieve_command_gui_sources)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${sieve_command_gui_source}"
         sieve_command_gui_source_contents)
    if(sieve_command_gui_source_contents MATCHES
       "javelin::app::(MailApplicationService|SieveApplicationService)|app/(MailApplicationService|SieveApplicationService)\.h")
        message(FATAL_ERROR
                "Sieve GUI must depend on SieveCommandPort: ${sieve_command_gui_source}")
    endif()
endforeach()

set(javelin_refresh_content_gui_sources
    src/gui/shell/AccountRefreshController.cpp
    src/gui/shell/AccountRefreshController.h
    src/gui/shell/MessageContentController.cpp
    src/gui/shell/MessageContentController.h
    src/gui/shell/MessageFileController.cpp
    src/gui/shell/MessageFileController.h
    src/gui/shell/MessageFileUtils.cpp
    src/gui/shell/MessageFileUtils.h
)
foreach(refresh_content_gui_source IN LISTS javelin_refresh_content_gui_sources)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${refresh_content_gui_source}"
         refresh_content_gui_source_contents)
    if(refresh_content_gui_source_contents MATCHES
       "javelin::app::(MailApplicationService|AccountRuntimeManager|MailQueryApplicationService|MessageContentApplicationService)|app/(MailApplicationService|AccountRuntimeManager|MailQueryApplicationService|MessageContentApplicationService)\.h")
        message(FATAL_ERROR
                "Refresh/content GUI must depend on application ports: ${refresh_content_gui_source}")
    endif()
endforeach()

target_sources(javelin_gui PRIVATE
    src/gui/accessibility/AccessibleFactory.h
    src/gui/calendar/CalendarEventButton.cpp
    src/gui/calendar/CalendarEventButton.h
)
target_include_directories(javelin_gui PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)
target_link_libraries(javelin_gui
    PUBLIC
        Qt::Core
        Qt::Network
        Qt::Sql
        Qt::Svg
        Qt::Widgets
        Qt::WebEngineWidgets
        KF6::ConfigWidgets
        KF6::I18n
        KF6::WidgetsAddons
        KF6::XmlGui
        KF6::TextEditor
        KPim6::MessageComposer
        QCoro6::Coro
        javelin_translation
    PRIVATE
        Qt::Concurrent
        javelin_cache_read
        javelin_app_shared
        javelin_observability
)
javelin_set_project_warnings(javelin_gui)
javelin_enable_static_analysis(javelin_gui)
javelin_enable_sanitizers(javelin_gui)
# QCoro uses exceptions internally
target_compile_options(javelin_gui PRIVATE -fexceptions)

add_library(javelin_daemon_core STATIC
    src/app/AccountApplicationPorts.h
    src/app/DeveloperDiagnostics.h
    src/app/developer/DeveloperDiagnosticsService.cpp
    src/app/DeveloperDiagnosticsService.h
    src/app/DeveloperMaintenance.h
    src/app/developer/DeveloperMaintenanceService.cpp
    src/app/DeveloperMaintenanceService.h
    src/app/messages/MailboxMaintenanceRegistry.cpp
    src/app/MailboxMaintenanceRegistry.h
    src/app/account/AccountCredentialStore.cpp
    src/app/AccountCredentialStore.h
    src/app/account/KWalletAccountCredentialStore.cpp
    src/app/account/AccountCommandService.cpp
    src/app/AccountCommandService.h
    src/app/AccountRefreshApplicationPorts.h
    src/app/account/AccountRefreshCommandService.cpp
    src/app/AccountRefreshCommandService.h
    src/app/runtime/CommandDispatcher.cpp
    src/app/CommandDispatcher.h
    src/app/runtime/CacheInvalidationPublisher.cpp
    src/app/CacheInvalidationPublisher.h
    src/app/RefreshGeneration.h
    src/app/messages/MailApplicationEventsService.cpp
    src/app/MailApplicationEventsService.h
    src/app/AccountConnectionProvider.h
    src/app/AccountConnectionSettings.h
    src/app/runtime/ApplicationErrorCoordinator.cpp
    src/app/ApplicationErrorCoordinator.h
    src/app/calendar/CalendarNotificationService.cpp
    src/app/CalendarNotificationService.h
    src/app/calendar/CalendarInvitationService.cpp
    src/app/CalendarInvitationService.h
    src/app/calendar/CalendarCommandService.cpp
    src/app/CalendarCommandService.h
    src/app/ContactApplicationPorts.h
    src/app/MailApplicationTypes.h
    src/app/MailApplicationPorts.h
    src/app/messages/MailCommandService.cpp
    src/app/MailCommandService.h
    src/app/MessageContentApplicationPorts.h
    src/app/messages/MessageContentCommandService.cpp
    src/app/MessageContentCommandService.h
    src/app/MessageListCacheRead.h
    src/app/MessageSubject.h
    src/app/MailboxTreeCacheRead.h
    src/app/MessageListSessionFactory.h
    src/app/SieveApplicationPorts.h
    src/app/sieve/SieveCommandService.cpp
    src/app/SieveCommandService.h
    src/app/IdentityApplicationPorts.h
    src/app/identity/IdentityCommandService.cpp
    src/app/IdentityCommandService.h
    src/app/undo/UndoCommandService.cpp
    src/app/UndoCommandService.h
    src/app/compose/ComposeService.cpp
    src/app/ComposeService.h
    src/app/ComposeApplicationPorts.h
    src/app/compose/ComposeCommandService.cpp
    src/app/ComposeCommandService.h
    src/app/compose/ComposePreferences.cpp
    src/app/ComposePreferences.h
    src/app/compose/DeferredSendRepository.cpp
    src/app/DeferredSendRepository.h
    src/app/messages/MailTransferRepository.cpp
    src/app/MailTransferRepository.h
    src/app/messages/MailTransferPlanning.cpp
    src/app/MailTransferPlanning.h
    src/app/messages/MailTransferApplicationService.cpp
    src/app/MailTransferApplicationService.h
    src/app/messages/MailTransferCommandService.cpp
    src/app/MailTransferCommandService.h
    src/app/messages/MailTransferExecutor.cpp
    src/app/MailTransferExecutor.h
    src/app/compose/DeferredSendService.cpp
    src/app/DeferredSendService.h
    src/app/contacts/ContactCommandPreparation.cpp
    src/app/ContactCommandPreparation.h
    src/app/ContactCommands.h
    src/app/contacts/ContactCommandService.cpp
    src/app/ContactCommandService.h
    src/desktop/notifications/DesktopNotificationController.cpp
    src/desktop/notifications/DesktopNotificationController.h
    src/app/messages/FullMailSyncService.cpp
    src/app/FullMailSyncService.h
    src/app/undo/HistoryRepository.cpp
    src/app/undo/HistoryRepository.h
    src/app/undo/HistorySerialization.cpp
    src/app/undo/HistorySerialization.h
    src/app/undo/HistoryTypes.cpp
    src/app/undo/HistoryTypes.h
    src/app/undo/HistoryCommandExecutor.h
    src/app/undo/DraftHistoryExecutor.cpp
    src/app/undo/DraftHistoryExecutor.h
    src/app/undo/DraftHistoryPort.h
    src/app/undo/CalendarHistoryExecutor.cpp
    src/app/undo/CalendarHistoryExecutor.h
    src/app/undo/CalendarHistoryPort.h
    src/app/undo/CalendarPreferenceExecutor.cpp
    src/app/undo/CalendarPreferenceExecutor.h
    src/app/undo/CalendarPreferencePort.h
    src/app/undo/AddressBookHistoryExecutor.cpp
    src/app/undo/AddressBookHistoryExecutor.h
    src/app/undo/AddressBookHistoryPort.h
    src/app/undo/ContactHistoryExecutor.cpp
    src/app/undo/ContactHistoryExecutor.h
    src/app/undo/ContactHistoryPort.h
    src/app/undo/MailHistoryExecutor.cpp
    src/app/undo/MailHistoryExecutor.h
    src/app/undo/MailHistoryPort.h
    src/app/undo/MailTransferHistoryCoordinator.cpp
    src/app/undo/MailTransferHistoryCoordinator.h
    src/app/undo/MailTransferHistoryExecutor.cpp
    src/app/undo/MailTransferHistoryExecutor.h
    src/app/undo/MailTransferHistoryPort.h
    src/app/undo/MailTransferHistoryService.cpp
    src/app/undo/MailTransferHistoryService.h
    src/app/undo/SieveHistoryExecutor.cpp
    src/app/undo/SieveHistoryExecutor.h
    src/app/undo/SieveHistoryPort.h
    src/app/undo/UndoManager.cpp
    src/app/undo/UndoManager.h
    src/app/runtime/LocalMaintenanceService.cpp
    src/app/LocalMaintenanceService.h
    src/app/account/AccountSyncCoordinator.cpp
    src/app/account/AccountSyncCoordinator.h
    src/app/account/OAuthRefreshSingleFlight.cpp
    src/app/OAuthRefreshSingleFlight.h
    src/app/messages/MailApplicationService.cpp
    src/app/AccountRuntimeManager.h
    src/app/CalendarApplicationService.h
    src/app/ContactApplicationService.h
    src/app/MailMutationApplicationService.h
    src/app/MailNotificationService.h
    src/app/MailQueryApplicationService.h
    src/app/MessageContentApplicationService.h
    src/app/SieveApplicationService.h
    src/app/messages/EmailMutationBatchSubmitter.cpp
    src/app/EmailMutationBatchSubmitter.h
    src/app/messages/ThreadMaterializationCoordinator.cpp
    src/app/ThreadMaterializationCoordinator.h
    src/app/messages/ThreadMembershipMaterializationWorker.cpp
    src/app/ThreadMembershipMaterializationWorker.h
    src/app/messages/MailboxSelectionMutation.cpp
    src/app/MailboxSelectionMutation.h
    src/app/messages/MailboxSyncCoverage.cpp
    src/app/MailboxSyncCoverage.h
    src/app/messages/MailIndexService.cpp
    src/app/MailIndexService.h
    src/app/messages/MessageSelection.cpp
    src/app/MessageSelection.h
    src/daemon/DaemonBackgroundController.cpp
    src/daemon/DaemonBackgroundController.h
    src/daemon/DaemonProcess.cpp
    src/daemon/DaemonProcess.h
    src/daemon/actions/DaemonRemoteActionDispatcher.cpp
    src/daemon/actions/DaemonRemoteActionDispatcher.h
    src/daemon/actions/DaemonRemoteActionAccountHandler.cpp
    src/daemon/actions/DaemonRemoteActionCalendarHandler.cpp
    src/daemon/actions/DaemonRemoteActionComposeHandler.cpp
    src/daemon/actions/DaemonRemoteActionContactHandler.cpp
    src/daemon/actions/DaemonRemoteActionDeveloperHandler.cpp
    src/daemon/actions/DaemonRemoteActionHistoryHandler.cpp
    src/daemon/actions/DaemonRemoteActionIdentityHandler.cpp
    src/daemon/actions/DaemonRemoteActionMailHandler.cpp
    src/daemon/actions/DaemonRemoteActionSieveHandler.cpp
    src/daemon/actions/DaemonRemoteActionWorkHandler.cpp
    src/app/RemoteActionTypes.h
    src/app/OnboardingTypes.h
    src/app/RemoteCodec.h
    src/daemon/DaemonServices.cpp
    src/daemon/DaemonServices.h
    src/desktop/tray/DaemonTrayController.cpp
    src/desktop/tray/DaemonTrayController.h
    src/app/settings/SettingsRepository.cpp
    src/app/SettingsRepository.h
    src/app/work/WorkScheduler.cpp
    src/app/WorkScheduler.h
    src/app/WorkTaskPort.h
)
target_include_directories(javelin_daemon_core PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)
target_link_libraries(javelin_daemon_core
    PUBLIC
        Qt::Core
        javelin_app_contracts
        javelin_app_shared
        javelin_protocol
        javelin_jmap
    PRIVATE
        QCoro6::Network
        Qt::Concurrent
        Qt::DBus
        Qt::Network
        KF6::I18n
        KF6::Wallet
        glaze::glaze
        javelin_observability
)
javelin_set_project_warnings(javelin_daemon_core)
javelin_enable_static_analysis(javelin_daemon_core)
javelin_enable_sanitizers(javelin_daemon_core)
# QCoro and daemon composition use exceptions internally
target_compile_options(javelin_daemon_core PRIVATE -fexceptions)

# Phase 11 composition checks.  The daemon target is the socket-safe half of
# the application graph.  The GUI bootstrap consumes only the typed protocol,
# read-only cache, and presentation targets.
get_target_property(javelin_cache_read_links javelin_cache_read LINK_LIBRARIES)
if(javelin_cache_read_links MATCHES "Qt::(Widgets|Gui)|Qt::WebEngine")
    message(FATAL_ERROR "javelin_cache_read must remain usable from QCoreApplication processes")
endif()
get_target_property(javelin_jmap_links javelin_jmap LINK_LIBRARIES)
if(javelin_jmap_links MATCHES "Qt::(Widgets|Gui)|Qt::WebEngine")
    message(FATAL_ERROR "javelin_jmap must remain usable from QCoreApplication processes")
endif()
get_target_property(javelin_daemon_core_links javelin_daemon_core LINK_LIBRARIES)
if(javelin_daemon_core_links MATCHES "Qt::(Widgets|Gui)|Qt::WebEngine")
    message(FATAL_ERROR "javelin_daemon_core must not link GUI or WebEngine targets")
endif()
get_target_property(javelin_gui_links javelin_gui LINK_LIBRARIES)
if(javelin_gui_links MATCHES "javelin_daemon_core|javelin_jmap")
    message(FATAL_ERROR "javelin_gui must not link daemon or JMAP transport targets")
endif()
get_target_property(javelin_daemon_core_sources javelin_daemon_core SOURCES)
foreach(daemon_source IN LISTS javelin_daemon_core_sources)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${daemon_source}")
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${daemon_source}" daemon_source_contents)
        if(daemon_source_contents MATCHES "QWebEngine|QWidget|QApplication|MainWindow")
            message(FATAL_ERROR "Daemon boundary violation in ${daemon_source}")
        endif()
    endif()
endforeach()

qt_add_executable(javelind
    src/daemon/main.cpp
)
target_link_libraries(javelind
    PRIVATE
        javelin_daemon_core
        Qt::Core
        KF6::I18n
)
target_compile_definitions(javelind PRIVATE JAVELIN_APP_VERSION="${PROJECT_VERSION}")
target_compile_options(javelind PRIVATE -fexceptions)
javelin_set_project_warnings(javelind)
javelin_enable_static_analysis(javelind)
javelin_enable_sanitizers(javelind)

add_library(javelin_gui_session STATIC
    src/client/GuiDaemonSession.cpp
    src/client/GuiDaemonSession.h
    src/client/GuiMailApplicationEvents.cpp
    src/client/GuiMailApplicationEvents.h
)
target_include_directories(javelin_gui_session PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)
target_link_libraries(javelin_gui_session
    PUBLIC
        javelin_app_contracts
        javelin_protocol
        Qt::Core
    PRIVATE
        javelin_cache_read
        javelin_observability
)
javelin_set_project_warnings(javelin_gui_session)
javelin_enable_static_analysis(javelin_gui_session)
javelin_enable_sanitizers(javelin_gui_session)
target_compile_options(javelin_gui_session PRIVATE -fexceptions)

qt_add_executable(javelin
    src/app/DeveloperDiagnostics.h
    src/app/DeveloperMaintenance.h
    src/client/GuiServices.cpp
    src/client/GuiServices.h
    src/app/MessageListMaterializationPort.h
    src/client/RemoteActionClient.cpp
    src/client/RemoteActionClient.h
    src/app/RemoteActionTypes.h
    src/app/OnboardingApplicationPorts.h
    src/app/OnboardingTypes.h
    src/client/RemoteOnboardingPort.cpp
    src/client/RemoteOnboardingPort.h
    src/client/RemoteApplicationPorts.cpp
    src/client/RemoteApplicationPorts.h
    src/app/RemoteCodec.h
    src/client/main.cpp
)
target_link_libraries(javelin
    PRIVATE
        javelin_protocol
        javelin_cache_read
        javelin_gui
        javelin_app_shared
        javelin_gui_session
        javelin_observability
        glaze::glaze
        QCoro6::Core
        Qt::Widgets
)
file(GLOB javelin_runtime_icon_resources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/res/thunderbird-icons/*.svg"
)
qt_add_resources(javelin "javelin_runtime_icons"
    PREFIX "/icons"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/res"
    FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/res/icon.svg"
        ${javelin_runtime_icon_resources}
)
qt_add_resources(javelin "javelin_runtime_darkreader"
    PREFIX "/vendor/darkreader"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/res/vendor/darkreader"
    FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/res/vendor/darkreader/darkreader.js"
)
if(JAVELIN_ENABLE_BERGAMOT_TRANSLATION)
    set_source_files_properties(
        "${JAVELIN_BERGAMOT_SOURCE_DIR}/LICENSE"
        PROPERTIES QT_RESOURCE_ALIAS "MPL-2.0.txt")
    qt_add_resources(javelin "javelin_translation_models"
        PREFIX "/javelin/translation"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/res/models/translations"
        FILES
            "${CMAKE_CURRENT_SOURCE_DIR}/res/models/translations/manifest-v1.json"
    )
    qt_add_resources(javelin "javelin_translation_licenses"
        PREFIX "/javelin/translation/licenses"
        FILES
            "${JAVELIN_BERGAMOT_SOURCE_DIR}/LICENSE"
    )
endif()
target_compile_definitions(javelin PRIVATE
    JAVELIN_APP_VERSION="${PROJECT_VERSION}"
    JAVELIN_INSTALL_BINDIR="${JAVELIN_INSTALL_BINDIR_ABSOLUTE}"
)
target_compile_options(javelin PRIVATE -fexceptions)
javelin_set_project_warnings(javelin)
javelin_enable_static_analysis(javelin)
javelin_enable_sanitizers(javelin)

add_custom_target(extract-translations
    COMMAND ${CMAKE_COMMAND}
        -DJAVELIN_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DJAVELIN_TRANSLATION_DOMAIN=javelinmail
        -DJAVELIN_TRANSLATION_OUTPUT=${PROJECT_SOURCE_DIR}/po/javelinmail.pot
        -P ${PROJECT_SOURCE_DIR}/cmake/ExtractTranslations.cmake
    COMMENT "Extracting KI18n messages"
    VERBATIM
)

ki18n_install(po)

get_target_property(javelin_bootstrap_links javelin LINK_LIBRARIES)
if(javelin_bootstrap_links MATCHES "javelin_daemon_core|javelin_jmap")
    message(FATAL_ERROR "javelin GUI bootstrap must not link daemon or JMAP transport targets")
endif()
get_target_property(javelind_links javelind LINK_LIBRARIES)
if(javelind_links MATCHES "javelin_gui|Qt::(Widgets|WebEngine)")
    message(FATAL_ERROR "javelind must not link GUI or WebEngine targets")
endif()
get_target_property(javelin_mail_model_links javelin_mail_model LINK_LIBRARIES)
if(javelin_mail_model_links MATCHES "javelin_(jmap|daemon_core)|Qt::(Network|Widgets)|Qt::WebEngine")
    message(FATAL_ERROR "javelin_mail_model must remain transport- and GUI-independent")
endif()
get_target_property(javelin_app_shared_links javelin_app_shared LINK_LIBRARIES)
if(javelin_app_shared_links MATCHES "javelin_(jmap|daemon_core)|Qt::(Network|Widgets)|Qt::WebEngine")
    message(FATAL_ERROR "javelin_app_shared must remain usable by both process compositions")
endif()

# Phase 9 source-ownership guard. A production implementation belongs to exactly one target;
# other layers consume it through target links rather than recompiling the same .cpp file.
set(javelin_production_source_targets
    javelin_protocol
    javelin_cache_read
    javelin_observability
    javelin_app_contracts
    javelin_mail_model
    javelin_app_shared
    javelin_jmap
    javelin_translation
    javelin_gui
    javelin_daemon_core
    javelin_gui_session
    javelin
    javelind
)
set(javelin_owned_production_sources)
set(javelin_owned_production_source_targets)
foreach(production_target IN LISTS javelin_production_source_targets)
    get_target_property(production_sources ${production_target} SOURCES)
    get_target_property(production_source_dir ${production_target} SOURCE_DIR)
    foreach(production_source IN LISTS production_sources)
        if(IS_ABSOLUTE "${production_source}")
            set(production_source_path "${production_source}")
        else()
            get_filename_component(production_source_path
                "${production_source_dir}/${production_source}" ABSOLUTE)
        endif()
        get_filename_component(production_source_extension "${production_source_path}" EXT)
        string(FIND "${production_source_path}" "${PROJECT_SOURCE_DIR}/src/"
            production_source_prefix)
        if(NOT production_source_extension STREQUAL ".cpp" OR
           NOT production_source_prefix EQUAL 0)
            continue()
        endif()

        list(FIND javelin_owned_production_sources "${production_source_path}"
            production_source_index)
        if(production_source_index GREATER_EQUAL 0)
            list(GET javelin_owned_production_source_targets ${production_source_index}
                production_source_owner)
            if(NOT production_source_owner STREQUAL production_target)
                message(FATAL_ERROR
                    "Production source has multiple owners: ${production_source_path} "
                    "(${production_source_owner}, ${production_target})")
            endif()
        else()
            list(APPEND javelin_owned_production_sources "${production_source_path}")
            list(APPEND javelin_owned_production_source_targets "${production_target}")
        endif()
    endforeach()
endforeach()

# Phase 10 physical ownership guard. Application implementations live in focused feature/runtime
# directories, while cache persistence implementations live under storage rather than jmap/cache.
file(GLOB javelin_app_root_implementations CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app/*.cpp")
if(javelin_app_root_implementations)
    message(FATAL_ERROR
        "Application implementations must not return to src/app: ${javelin_app_root_implementations}")
endif()
file(GLOB javelin_legacy_cache_implementations CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/jmap/cache/*.cpp")
if(javelin_legacy_cache_implementations)
    message(FATAL_ERROR
        "Cache persistence implementations belong under src/storage: ${javelin_legacy_cache_implementations}")
endif()

function(javelin_assert_gui_settings_boundary target)
    get_target_property(target_sources ${target} SOURCES)
    foreach(source IN LISTS target_sources)
        if(IS_ABSOLUTE "${source}")
            set(source_path "${source}")
        else()
            set(source_path "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
        endif()
        if(NOT EXISTS "${source_path}")
            continue()
        endif()
        file(RELATIVE_PATH relative_source "${CMAKE_CURRENT_SOURCE_DIR}" "${source_path}")
        file(READ "${source_path}" source_contents)
        if(source_contents MATCHES "QSettings" AND
           NOT relative_source STREQUAL "src/gui/translation/TranslationSettingsStore.cpp")
            message(FATAL_ERROR
                    "Production GUI source accesses canonical QSettings: ${relative_source}")
        endif()
    endforeach()
endfunction()

javelin_assert_gui_settings_boundary(javelin_translation)
javelin_assert_gui_settings_boundary(javelin_gui)
javelin_assert_gui_settings_boundary(javelin)

qt_add_executable(jmap-transport-benchmark
    src/tools/JmapTransportBenchmark.cpp
)
target_link_libraries(jmap-transport-benchmark PRIVATE javelin_jmap Qt::Core Qt::Network)
target_compile_options(jmap-transport-benchmark PRIVATE -fexceptions)
javelin_set_project_warnings(jmap-transport-benchmark)
javelin_enable_sanitizers(jmap-transport-benchmark)

qt_add_executable(jmap-query
    src/tools/JmapQuery.cpp
)
target_link_libraries(jmap-query PRIVATE javelin_daemon_core Qt::Core Qt::Network glaze::glaze)
target_compile_options(jmap-query PRIVATE -fexceptions)
javelin_set_project_warnings(jmap-query)
javelin_enable_sanitizers(jmap-query)

qt_add_executable(javelin-undo-live-check
    src/tools/UndoRedoAutonomousSuite.cpp
    src/tools/UndoRedoAutonomousSuite.h
    src/tools/UndoRedoLiveCheck.cpp
)
target_link_libraries(javelin-undo-live-check PRIVATE javelin_daemon_core Qt::Widgets)
target_compile_options(javelin-undo-live-check PRIVATE -fexceptions)
javelin_set_project_warnings(javelin-undo-live-check)
javelin_enable_sanitizers(javelin-undo-live-check)

# KDEInstallDirs can leave KDE_INSTALL_KXMLGUIDIR empty in some local/dev
# configurations; default to the documented DATAROOTDIR/kxmlgui5 location.
if(NOT KDE_INSTALL_KXMLGUIDIR)
    set(KDE_INSTALL_KXMLGUIDIR "${KDE_INSTALL_DATAROOTDIR}/kxmlgui5")
endif()
if(NOT KDE_INSTALL_ICONDIR)
    set(KDE_INSTALL_ICONDIR "${KDE_INSTALL_DATAROOTDIR}/icons")
endif()

# Install data files (KXMLGUI .rc, etc.) into the local install prefix so
# KXmlGuiWindow can find them at runtime.  We copy only our own files
# rather than running `cmake --install` (which would also try to install
# FetchContent dependencies).
set(_local_kxmlgui_dir "${CMAKE_INSTALL_PREFIX}/${KDE_INSTALL_KXMLGUIDIR}/javelinmail")
set(_local_kxmlgui_runtime_dir
    "${CMAKE_INSTALL_PREFIX}/${KDE_INSTALL_DATADIR}/kxmlgui/javelinmail")
set(_local_icon_theme_dir "${CMAKE_INSTALL_PREFIX}/${KDE_INSTALL_ICONDIR}")
if(JAVELIN_ENABLE_BERGAMOT_TRANSLATION)
    set(_ssplit_prefix_source_dir
        "${JAVELIN_BERGAMOT_SOURCE_DIR}/inference/3rd_party/ssplit-cpp/nonbreaking_prefixes")
    set(_local_ssplit_prefix_dir
        "${CMAKE_INSTALL_PREFIX}/${KDE_INSTALL_DATADIR}/javelinmail/models/ssplit")
    set(_local_translation_notice_dir
        "${CMAKE_INSTALL_PREFIX}/${KDE_INSTALL_DATADIR}/licenses/javelinmail/translations")
endif()
target_compile_definitions(javelin_daemon_core
    PRIVATE JAVELIN_ICON_THEME_PATH="${_local_icon_theme_dir}"
)
if(JAVELIN_ENABLE_LOCAL_DATA_INSTALL)
    set(_install_data_stamp "${CMAKE_CURRENT_BINARY_DIR}/install_data.stamp")
    add_custom_command(
        OUTPUT "${_install_data_stamp}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_local_kxmlgui_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PROJECT_SOURCE_DIR}/res/javelinmailui.rc"
            "${_local_kxmlgui_dir}/javelinmailui.rc"
        # KF6 resolves application XMLGUI resources below share/kxmlgui.  Some
        # KDEInstallDirs versions still report the historical kxmlgui5 path,
        # so keep the configured install copy and also populate the runtime
        # lookup location used by KXmlGuiWindow.
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_local_kxmlgui_runtime_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PROJECT_SOURCE_DIR}/res/javelinmailui.rc"
            "${_local_kxmlgui_runtime_dir}/javelinmailui.rc"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${_local_icon_theme_dir}/hicolor/scalable/apps"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PROJECT_SOURCE_DIR}/res/icon.svg"
            "${_local_icon_theme_dir}/hicolor/scalable/apps/javelinmail.svg"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CMAKE_INSTALL_PREFIX}/${KDE_INSTALL_DATADIR}/javelinmail/models/fasttext"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${PROJECT_SOURCE_DIR}/res/models/fasttext"
            "${CMAKE_INSTALL_PREFIX}/${KDE_INSTALL_DATADIR}/javelinmail/models/fasttext"
        COMMAND ${CMAKE_COMMAND} -E touch "${_install_data_stamp}"
        DEPENDS
            "${PROJECT_SOURCE_DIR}/res/javelinmailui.rc"
            "${PROJECT_SOURCE_DIR}/res/icon.svg"
            "${PROJECT_SOURCE_DIR}/res/models/fasttext/LICENSE"
            "${JAVELIN_FASTTEXT_SOURCE_MODEL_PATH}"
            "${PROJECT_SOURCE_DIR}/res/models/fasttext/README.md"
        COMMENT "Installing data files to local prefix"
        VERBATIM
    )
    add_custom_target(install_data DEPENDS "${_install_data_stamp}")
endif()

if(JAVELIN_ENABLE_LOCAL_DATA_INSTALL AND JAVELIN_ENABLE_BERGAMOT_TRANSLATION)
    set(_install_translation_data_stamp
        "${CMAKE_CURRENT_BINARY_DIR}/install_translation_data.stamp")
    add_custom_command(
        OUTPUT "${_install_translation_data_stamp}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_local_ssplit_prefix_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${_ssplit_prefix_source_dir}"
            "${_local_ssplit_prefix_dir}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_local_translation_notice_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${JAVELIN_BERGAMOT_SOURCE_DIR}/LICENSE"
            "${_local_translation_notice_dir}/Mozilla-Translations-MPL-2.0.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${JAVELIN_BERGAMOT_SOURCE_DIR}/inference/3rd_party/ssplit-cpp/LICENSE.md"
            "${_local_translation_notice_dir}/ssplit-cpp-LICENSE.md"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${JAVELIN_BERGAMOT_SOURCE_DIR}/inference/marian-fork/LICENSE.md"
            "${_local_translation_notice_dir}/Marian-LICENSE.md"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PROJECT_SOURCE_DIR}/res/models/translations/manifest-v1.json"
            "${_local_translation_notice_dir}/manifest-v1.json"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PROJECT_SOURCE_DIR}/res/models/translations/README.md"
            "${_local_translation_notice_dir}/README.md"
        COMMAND ${CMAKE_COMMAND} -E touch "${_install_translation_data_stamp}"
        DEPENDS
            "${JAVELIN_BERGAMOT_SOURCE_DIR}/LICENSE"
            "${JAVELIN_BERGAMOT_SOURCE_DIR}/inference/3rd_party/ssplit-cpp/LICENSE.md"
            "${JAVELIN_BERGAMOT_SOURCE_DIR}/inference/marian-fork/LICENSE.md"
            "${PROJECT_SOURCE_DIR}/res/models/translations/manifest-v1.json"
            "${PROJECT_SOURCE_DIR}/res/models/translations/README.md"
        COMMENT "Installing local translation resources"
        VERBATIM
    )
    add_custom_target(install_translation_data
                      DEPENDS "${_install_translation_data_stamp}")
endif()

if(JAVELIN_ENABLE_LOCAL_DATA_INSTALL)
    add_dependencies(javelin install_data)
    add_dependencies(javelind install_data)
    if(JAVELIN_ENABLE_BERGAMOT_TRANSLATION)
        add_dependencies(javelin install_translation_data)
    endif()
endif()

file(GLOB_RECURSE JAVELIN_ALL_FORMAT_FILES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/src/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/*.h"
    "${PROJECT_SOURCE_DIR}/tests/*.cpp"
    "${PROJECT_SOURCE_DIR}/tests/*.h"
)
javelin_add_format_targets(${JAVELIN_ALL_FORMAT_FILES})

include(cmake/CheckCoroutineOwnership.cmake)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

install(TARGETS javelin javelind
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

if(JAVELIN_INSTALL_SYSTEMD_USER_SERVICE)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/javelind.service"
        DESTINATION ${KDE_INSTALL_LIBDIR}/systemd/user
    )
endif()

install(FILES res/javelinmailui.rc
    DESTINATION ${KDE_INSTALL_KXMLGUIDIR}/javelinmail
)
install(FILES res/vendor/darkreader/LICENSE
    DESTINATION ${KDE_INSTALL_DATADIR}/licenses/javelinmail/darkreader
)

install(DIRECTORY res/models/fasttext/
    DESTINATION ${KDE_INSTALL_DATADIR}/javelinmail/models/fasttext
)

if(JAVELIN_ENABLE_BERGAMOT_TRANSLATION)
    install(DIRECTORY "${_ssplit_prefix_source_dir}/"
        DESTINATION ${KDE_INSTALL_DATADIR}/javelinmail/models/ssplit
    )
    install(FILES
        "${JAVELIN_BERGAMOT_SOURCE_DIR}/LICENSE"
        DESTINATION ${KDE_INSTALL_DATADIR}/licenses/javelinmail/translations
        RENAME Mozilla-Translations-MPL-2.0.txt
    )
    install(FILES
        "${JAVELIN_BERGAMOT_SOURCE_DIR}/inference/3rd_party/ssplit-cpp/LICENSE.md"
        DESTINATION ${KDE_INSTALL_DATADIR}/licenses/javelinmail/translations
        RENAME ssplit-cpp-LICENSE.md
    )
    install(FILES
        "${JAVELIN_BERGAMOT_SOURCE_DIR}/inference/marian-fork/LICENSE.md"
        DESTINATION ${KDE_INSTALL_DATADIR}/licenses/javelinmail/translations
        RENAME Marian-LICENSE.md
    )
    install(FILES
        res/models/translations/manifest-v1.json
        res/models/translations/README.md
        DESTINATION ${KDE_INSTALL_DATADIR}/licenses/javelinmail/translations
    )
endif()

install(FILES res/javelinmail.desktop
    DESTINATION ${KDE_INSTALL_APPDIR}
)

install(FILES res/app.javelin.JavelinMail.metainfo.xml
    DESTINATION ${KDE_INSTALL_METAINFODIR}
)

install(FILES res/icon.svg
    DESTINATION ${KDE_INSTALL_ICONDIR}/hicolor/scalable/apps
    RENAME javelinmail.svg
)

qt_generate_deploy_app_script(
    TARGET javelin
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
if(WIN32 OR APPLE)
    install(SCRIPT ${deploy_script})
endif()
