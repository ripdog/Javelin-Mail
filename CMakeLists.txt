cmake_minimum_required(VERSION 3.19)
project(Javelin-Mail VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(JAVELIN_ENABLE_LOCAL_DATA_INSTALL "Copy app data into the active install prefix during builds" ON)
option(JAVELIN_ENABLE_FASTTEXT_LANGUAGE_DETECTION "Use fastText for local mail language detection" ON)

include(cmake/Warnings.cmake)
include(cmake/Dependencies.cmake)
include(cmake/StaticAnalysis.cmake)
include(cmake/Sanitizers.cmake)
include(CTest)

# 1. Find Qt6 FIRST
find_package(Qt6 6.6 REQUIRED COMPONENTS Core DBus Network Sql Widgets Concurrent LinguistTools WebEngineCore WebEngineWidgets Svg WebSockets)

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

find_package(KF6 REQUIRED COMPONENTS ConfigWidgets XmlGui CoreAddons TextEditor)
find_package(KPim6Mime REQUIRED)

qt_standard_project_setup()

set(JAVELIN_FORMAT_FILES
    src/jmap/sync/PreferredStateChangeSource.cpp
    src/jmap/sync/PreferredStateChangeSource.h
    tests/jmap/PreferredStateChangeSourceTest.cpp
    src/jmap/api/Cancellation.cpp
    src/jmap/api/Cancellation.h
    src/jmap/cache/SearchWindowRepository.cpp
    src/jmap/cache/SearchWindowRepository.h
    src/jmap/cache/MailboxWindowRepository.cpp
    src/jmap/cache/MailboxWindowRepository.h
    src/jmap/cache/NotificationRepository.cpp
    src/jmap/cache/NotificationRepository.h
    src/jmap/cache/CalendarNotificationRepository.cpp
    src/jmap/cache/CalendarNotificationRepository.h
    src/jmap/sync/StateChangeSource.h
    src/jmap/sync/MailboxInterestRegistry.cpp
    src/jmap/sync/MailboxInterestRegistry.h
    src/app/AccountConnectionSettings.h
    src/app/ApplicationErrorCoordinator.cpp
    src/app/ApplicationErrorCoordinator.h
    src/app/CalendarNotificationService.cpp
    src/app/CalendarNotificationService.h
    src/app/AddressSuggestionStore.cpp
    src/app/AddressSuggestionStore.h
    src/app/ComposeService.cpp
    src/app/ComposeService.h
    src/app/ApplicationBootstrap.cpp
    src/app/ApplicationBootstrap.h
    src/app/DesktopNotificationController.cpp
    src/app/DesktopNotificationController.h
    src/app/FullMailSyncService.cpp
    src/app/FullMailSyncService.h
    src/app/MailIndexService.cpp
    src/app/MailIndexService.h
    src/app/LocalMaintenanceService.cpp
    src/app/LocalMaintenanceService.h
    src/app/InlineMessageSchemeHandler.cpp
    src/app/InlineMessageSchemeHandler.h
    src/app/LogStore.cpp
    src/app/LogStore.h
    src/app/LongPollService.cpp
    src/app/LongPollService.h
    src/app/MailApplicationService.cpp
    src/app/MailApplicationService.h
    src/app/MailboxSelectionMutation.cpp
    src/app/MailboxSelectionMutation.h
    src/app/MessageSelection.cpp
    src/app/MessageSelection.h
    src/app/MailboxSyncCoverage.cpp
    src/app/MailboxSyncCoverage.h
    src/app/MessageNavigationCoordinator.cpp
    src/app/MessageNavigationCoordinator.h
    src/app/WebEngineSetup.h
    src/app/main.cpp
    src/app/ProcessServices.cpp
    src/app/ProcessServices.h
    src/gui/IconUtils.cpp
    src/gui/IconUtils.h
    src/gui/logging/LogViewerDialog.cpp
    src/gui/logging/LogViewerDialog.h
    src/gui/mailboxes/MailboxIconUtils.cpp
    src/gui/mailboxes/MailboxIconUtils.h
    src/gui/mailboxes/MailboxPropertiesDialog.cpp
    src/gui/mailboxes/MailboxPropertiesDialog.h
    src/gui/mailboxes/MailboxSort.h
    src/gui/mailboxes/MailboxTreeDelegate.cpp
    src/gui/mailboxes/MailboxTreeDelegate.h
    src/gui/mailboxes/MailboxTreeModel.cpp
    src/gui/mailboxes/MailboxTreeModel.h
    src/gui/mailboxes/MailboxTreeView.cpp
    src/gui/mailboxes/MailboxTreeView.h
    src/gui/compose/ComposeTabWidget.cpp
    src/gui/compose/ComposeTabWidget.h
    src/gui/contacts/ContactsManagerWidget.cpp
    src/gui/contacts/ContactsManagerWidget.h
    src/gui/widgets/EmailAddressLineEdit.cpp
    src/gui/widgets/EmailAddressLineEdit.h
    src/gui/calendar/MonthCalendarWidget.cpp
    src/gui/calendar/MonthCalendarWidget.h
    src/gui/calendar/MonthCalendarLayout.cpp
    src/gui/calendar/MonthCalendarLayout.h
    src/gui/calendar/EventDialog.cpp
    src/gui/calendar/EventDialog.h
    src/gui/calendar/RecurrenceDialog.cpp
    src/gui/calendar/RecurrenceDialog.h
    src/gui/calendar/RecurrencePattern.cpp
    src/gui/calendar/RecurrencePattern.h
    src/gui/messages/MessageListModel.cpp
    src/gui/messages/Pagination.h
    src/gui/messages/MessageListModel.h
    src/gui/messages/MessageListDelegate.cpp
    src/gui/messages/MessageListDelegate.h
    src/gui/messages/MessageDragListView.cpp
    src/gui/messages/MessageDragListView.h
    src/gui/messageview/GoogleHtmlTranslator.cpp
    src/gui/messageview/GoogleHtmlTranslator.h
    src/gui/messageview/HtmlMessageView.cpp
    src/gui/messageview/HtmlMessageView.h
    src/gui/messageview/MessageViewContainer.cpp
    src/gui/messageview/MessageViewContainer.h
    src/gui/messageview/MessageViewPresentation.h
    src/gui/messageview/PlainTextLinkifier.cpp
    src/gui/messageview/PlainTextLinkifier.h
    src/gui/search/AdvancedSearchDialog.cpp
    src/gui/search/AdvancedSearchDialog.h
    src/app/SearchSession.cpp
    src/app/SearchSession.h
    src/app/MessageListSession.h
    src/app/MailboxSession.cpp
    src/app/MailboxSession.h
    src/gui/search/SearchSessionPersistence.cpp
    src/gui/search/SearchSessionPersistence.h
    src/gui/settings/PreferencesDialog.cpp
    src/gui/settings/PreferencesDialog.h
    src/gui/shell/ElidingLabel.cpp
    src/gui/shell/ElidingLabel.h
    src/gui/shell/MessageFileUtils.cpp
    src/gui/shell/MessageFileUtils.h
    src/gui/shell/MessageFileController.cpp
    src/gui/shell/MessageFileController.h
    src/gui/shell/LayeredStatusBar.cpp
    src/gui/shell/LayeredStatusBar.h
    src/gui/shell/MainWindow.cpp
    src/gui/shell/MainWindow.h
    src/jmap/api/Error.cpp
    src/jmap/api/Error.h
    src/jmap/api/ContactsMethods.cpp
    src/jmap/api/ContactsMethods.h
    src/jmap/api/CalendarMethods.cpp
    src/jmap/api/CalendarMethods.h
    src/jmap/calendar/CalendarTypes.h
    src/jmap/calendar/CalendarEventEditing.cpp
    src/jmap/calendar/CalendarEventEditing.h
    src/jmap/calendar/CalendarService.cpp
    src/jmap/calendar/CalendarService.h
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
    src/jmap/cache/AccountRepository.cpp
    src/jmap/cache/AccountRepository.h
    src/jmap/cache/ComposeSessionRepository.cpp
    src/jmap/cache/ComposeSessionRepository.h
    src/jmap/cache/ContactRepository.cpp
    src/jmap/cache/ContactRepository.h
    src/jmap/cache/CalendarRepository.cpp
    src/jmap/cache/CalendarRepository.h
    src/jmap/cache/Database.cpp
    src/jmap/cache/Database.h
    src/jmap/cache/EmailRepository.cpp
    src/jmap/cache/EmailRepository.h
    src/jmap/cache/MailVault.cpp
    src/jmap/cache/MailVault.h
    src/jmap/cache/MailSearchIndex.cpp
    src/jmap/cache/MailSearchIndex.h
    src/jmap/cache/IdentityRepository.cpp
    src/jmap/cache/IdentityRepository.h
    src/jmap/cache/MailboxRepository.cpp
    src/jmap/cache/MailboxRepository.h
    src/jmap/cache/MessageContentTypes.h
    src/jmap/cache/MessageViewService.cpp
    src/jmap/cache/MessageViewService.h
    src/jmap/cache/MimeMessageParser.cpp
    src/jmap/cache/MimeMessageParser.h
    src/jmap/cache/QueryService.cpp
    src/jmap/cache/QueryService.h
    src/jmap/cache/RawMessageSourceRepository.cpp
    src/jmap/cache/RawMessageSourceRepository.h
    src/jmap/cache/SessionRepository.cpp
    src/jmap/cache/SessionRepository.h
    src/jmap/cache/SieveRepository.cpp
    src/jmap/cache/SieveRepository.h
    src/jmap/cache/JmapTransportPreferenceRepository.cpp
    src/jmap/cache/JmapTransportPreferenceRepository.h
    src/jmap/cache/SubmissionRepository.cpp
    src/jmap/cache/SubmissionRepository.h
    src/jmap/cache/SyncStateRepository.cpp
    src/jmap/cache/SyncStateRepository.h
    src/jmap/cache/ThreadRepository.cpp
    src/jmap/cache/ThreadRepository.h
    src/jmap/cache/TranslationCacheRepository.cpp
    src/jmap/cache/TranslationCacheRepository.h
    src/jmap/domain/MailEntities.h
    src/jmap/contacts/ContactTypes.cpp
    src/jmap/contacts/ContactTypes.h
    src/jmap/contacts/ContactInterchange.cpp
    src/jmap/contacts/ContactInterchange.h
    src/jmap/contacts/ContactService.cpp
    src/jmap/contacts/ContactService.h
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
    src/jmap/sync/WebSocketPushChannel.cpp
    src/jmap/sync/WebSocketPushChannel.h
    src/jmap/sync/ConsistencyDomain.cpp
    src/jmap/sync/ConsistencyDomain.h
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
    src/jmap/JmapCore.h
    src/jmap/OperationError.cpp
    src/jmap/OperationError.h
    src/jmap/EmailMutation.h
    src/jmap/language/FastTextLanguageDetector.cpp
    src/jmap/language/FastTextLanguageDetector.h
    src/jmap/language/LanguageDetection.cpp
    src/jmap/language/LanguageDetection.h
    src/jmap/submission/ComposeService.cpp
    src/jmap/submission/ComposeService.h
    src/jmap/submission/ComposeTypes.h
    src/jmap/submission/DraftMutationJournal.cpp
    src/jmap/submission/DraftMutationJournal.h
    src/jmap/sieve/SieveService.cpp
    src/jmap/sieve/SieveService.h
    src/jmap/sieve/SieveMutationJournal.cpp
    src/jmap/sieve/SieveMutationJournal.h
    src/jmap/sieve/SieveTypes.h
    src/gui/sieve/SieveEditorDialog.cpp
    src/gui/sieve/SieveEditorDialog.h
    tests/FixtureReader.cpp
    tests/FixtureReader.h
    tests/jmap/AccountRepositoryTest.cpp
    tests/jmap/AuthTest.cpp
    tests/jmap/ContactsMethodsTest.cpp
    tests/jmap/ContactRepositoryTest.cpp
    tests/jmap/ContactServiceTest.cpp
    tests/jmap/CalendarMethodsTest.cpp
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
)

add_library(javelin_jmap STATIC
    src/jmap/EmailMutation.h
    src/jmap/api/CalendarMethods.cpp
    src/jmap/api/CalendarMethods.h
    src/jmap/calendar/CalendarTypes.h
    src/jmap/calendar/CalendarService.cpp
    src/jmap/calendar/CalendarService.h
    src/jmap/calendar/CalendarEventEditing.cpp
    src/jmap/calendar/CalendarEventEditing.h
    src/jmap/cache/CalendarRepository.cpp
    src/jmap/cache/CalendarRepository.h
    src/jmap/sync/PreferredStateChangeSource.cpp
    src/jmap/sync/PreferredStateChangeSource.h
    src/jmap/api/Cancellation.cpp
    src/jmap/api/Cancellation.h
    src/jmap/cache/SearchWindowRepository.cpp
    src/jmap/cache/SearchWindowRepository.h
    src/jmap/cache/MailboxWindowRepository.cpp
    src/jmap/cache/MailboxWindowRepository.h
    src/jmap/cache/NotificationRepository.cpp
    src/jmap/cache/NotificationRepository.h
    src/jmap/cache/CalendarNotificationRepository.cpp
    src/jmap/cache/CalendarNotificationRepository.h
    src/jmap/sync/StateChangeSource.h
    src/jmap/sync/MailboxInterestRegistry.cpp
    src/jmap/sync/MailboxInterestRegistry.h
    src/jmap/api/SessionDiscovery.cpp
    src/jmap/api/SessionDiscovery.h
    src/jmap/api/ContactsMethods.cpp
    src/jmap/api/ContactsMethods.h
    src/jmap/api/Error.cpp
    src/jmap/api/Error.h
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
    src/jmap/contacts/ContactService.cpp
    src/jmap/contacts/ContactService.h
    src/jmap/contacts/ContactIdentityLookup.cpp
    src/jmap/contacts/ContactIdentityLookup.h
    src/jmap/api/Transport.cpp
    src/jmap/api/Transport.h
    src/jmap/cache/AccountRepository.cpp
    src/jmap/cache/AccountRepository.h
    src/jmap/cache/ComposeSessionRepository.cpp
    src/jmap/cache/ComposeSessionRepository.h
    src/jmap/cache/ContactRepository.cpp
    src/jmap/cache/ContactRepository.h
    src/jmap/cache/Database.cpp
    src/jmap/cache/Database.h
    src/jmap/cache/EmailRepository.cpp
    src/jmap/cache/EmailRepository.h
    src/jmap/cache/MailVault.cpp
    src/jmap/cache/MailVault.h
    src/jmap/cache/MailSearchIndex.cpp
    src/jmap/cache/MailSearchIndex.h
    src/jmap/cache/IdentityRepository.cpp
    src/jmap/cache/IdentityRepository.h
    src/jmap/cache/MailboxRepository.cpp
    src/jmap/cache/MailboxRepository.h
    src/jmap/cache/MessageContentTypes.h
    src/jmap/cache/MessageViewService.cpp
    src/jmap/cache/MessageViewService.h
    src/jmap/cache/MimeMessageParser.cpp
    src/jmap/cache/MimeMessageParser.h
    src/jmap/cache/QueryService.cpp
    src/jmap/cache/QueryService.h
    src/jmap/cache/RawMessageSourceRepository.cpp
    src/jmap/cache/RawMessageSourceRepository.h
    src/jmap/cache/SessionRepository.cpp
    src/jmap/cache/SessionRepository.h
    src/jmap/cache/SieveRepository.cpp
    src/jmap/cache/SieveRepository.h
    src/jmap/cache/JmapTransportPreferenceRepository.cpp
    src/jmap/cache/JmapTransportPreferenceRepository.h
    src/jmap/cache/SubmissionRepository.cpp
    src/jmap/cache/SubmissionRepository.h
    src/jmap/cache/SyncStateRepository.cpp
    src/jmap/cache/SyncStateRepository.h
    src/jmap/cache/ThreadRepository.cpp
    src/jmap/cache/ThreadRepository.h
    src/jmap/cache/TranslationCacheRepository.cpp
    src/jmap/cache/TranslationCacheRepository.h
    src/jmap/domain/MailEntities.h
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
    src/jmap/sync/WebSocketPushChannel.cpp
    src/jmap/sync/WebSocketPushChannel.h
    src/jmap/sync/ConsistencyDomain.cpp
    src/jmap/sync/ConsistencyDomain.h
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
    src/jmap/JmapCore.h
    src/jmap/OperationError.cpp
    src/jmap/OperationError.h
    src/jmap/language/FastTextLanguageDetector.cpp
    src/jmap/language/FastTextLanguageDetector.h
    src/jmap/language/LanguageDetection.cpp
    src/jmap/language/LanguageDetection.h
    src/jmap/submission/ComposeService.cpp
    src/jmap/submission/ComposeService.h
    src/jmap/submission/ComposeTypes.h
    src/jmap/submission/DraftMutationJournal.cpp
    src/jmap/submission/DraftMutationJournal.h
    src/jmap/sieve/SieveService.cpp
    src/jmap/sieve/SieveService.h
    src/jmap/sieve/SieveMutationJournal.cpp
    src/jmap/sieve/SieveMutationJournal.h
    src/jmap/sieve/SieveTypes.h
)
target_include_directories(javelin_jmap PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)
target_link_libraries(javelin_jmap
    PUBLIC
        QCoro6::Coro
        Qt::Core
        Qt::Gui
        Qt::Network
        Qt::Sql
        Qt::WebSockets
        KPim6::Mime
    PRIVATE
        QCoro6::Network
        Qt::Concurrent
        glaze::glaze
)
if(JAVELIN_ENABLE_FASTTEXT_LANGUAGE_DETECTION)
    target_link_libraries(javelin_jmap PRIVATE fasttext_javelin)
    target_compile_definitions(javelin_jmap
        PUBLIC
            JAVELIN_ENABLE_FASTTEXT_LANGUAGE_DETECTION=1
            JAVELIN_FASTTEXT_LANGUAGE_MODEL_PATH="${CMAKE_INSTALL_PREFIX}/${KDE_INSTALL_DATADIR}/javelinmail/models/fasttext/lid.176.ftz"
    )
    target_compile_options(javelin_jmap PRIVATE -fexceptions)
endif()
javelin_set_project_warnings(javelin_jmap)
javelin_enable_static_analysis(javelin_jmap)
javelin_enable_sanitizers(javelin_jmap)

add_library(javelin_gui STATIC
    src/app/AddressSuggestionStore.cpp
    src/app/AddressSuggestionStore.h
    src/app/LogStore.cpp
    src/app/LogStore.h
    src/gui/IconUtils.cpp
    src/gui/IconUtils.h
    src/gui/logging/LogViewerDialog.cpp
    src/gui/logging/LogViewerDialog.h
    src/gui/mailboxes/MailboxIconUtils.cpp
    src/gui/mailboxes/MailboxIconUtils.h
    src/gui/mailboxes/MailboxPropertiesDialog.cpp
    src/gui/mailboxes/MailboxPropertiesDialog.h
    src/gui/mailboxes/MailboxSort.h
    src/gui/compose/ComposeTabWidget.cpp
    src/gui/compose/ComposeTabWidget.h
    src/gui/contacts/ContactsManagerWidget.cpp
    src/gui/contacts/ContactsManagerWidget.h
    src/gui/widgets/EmailAddressLineEdit.cpp
    src/gui/widgets/EmailAddressLineEdit.h
    src/gui/calendar/MonthCalendarWidget.cpp
    src/gui/calendar/MonthCalendarWidget.h
    src/gui/calendar/MonthCalendarLayout.cpp
    src/gui/calendar/MonthCalendarLayout.h
    src/gui/calendar/EventDialog.cpp
    src/gui/calendar/EventDialog.h
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
    src/gui/messages/MessageListModel.cpp
    src/gui/messages/Pagination.h
    src/gui/messages/MessageListModel.h
    src/gui/messages/MessageListDelegate.cpp
    src/gui/messages/MessageListDelegate.h
    src/gui/messages/MessageDragListView.cpp
    src/gui/messages/MessageDragListView.h
    src/gui/messageview/GoogleHtmlTranslator.cpp
    src/gui/messageview/GoogleHtmlTranslator.h
    src/gui/messageview/HtmlMessageView.cpp
    src/gui/messageview/HtmlMessageView.h
    src/gui/messageview/MessageViewContainer.cpp
    src/gui/messageview/MessageViewContainer.h
    src/gui/messageview/MessageViewPresentation.h
    src/gui/messageview/PlainTextLinkifier.cpp
    src/gui/messageview/PlainTextLinkifier.h
    src/gui/search/AdvancedSearchDialog.cpp
    src/gui/search/AdvancedSearchDialog.h
    src/app/SearchSession.cpp
    src/app/SearchSession.h
    src/app/MessageListSession.h
    src/app/MailboxSession.cpp
    src/app/MailboxSession.h
    src/gui/search/SearchSessionPersistence.cpp
    src/gui/search/SearchSessionPersistence.h
    src/gui/settings/PreferencesDialog.cpp
    src/gui/settings/PreferencesDialog.h
    src/gui/shell/ElidingLabel.cpp
    src/gui/shell/ElidingLabel.h
    src/gui/shell/MessageFileUtils.cpp
    src/gui/shell/MessageFileUtils.h
    src/gui/shell/MessageFileController.cpp
    src/gui/shell/MessageFileController.h
    src/gui/shell/LayeredStatusBar.cpp
    src/gui/shell/LayeredStatusBar.h
    src/gui/shell/MainWindow.cpp
    src/gui/shell/MainWindow.h
    src/gui/sieve/SieveEditorDialog.cpp
    src/gui/sieve/SieveEditorDialog.h
)

file(GLOB_RECURSE javelin_gui_boundary_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gui/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gui/*.h"
)
foreach(gui_source IN LISTS javelin_gui_boundary_sources)
    file(READ "${gui_source}" gui_source_contents)
    if(gui_source_contents MATCHES "#include \"jmap/api/(Transport|MethodEnvelope)\\.h\"" OR
       gui_source_contents MATCHES "#include \"jmap/sync/(MailboxRefreshExecutor|MailboxStateRefreshExecutor)\\.h\"" OR
       gui_source_contents MATCHES "AccountSyncConfiguration" OR
       gui_source_contents MATCHES "javelin::jmap::MailboxPage(Result|Summary)" OR
       gui_source_contents MATCHES "javelin::jmap::MessageSearch(Result|Summary)" OR
       gui_source_contents MATCHES "javelin::jmap::LiveConnectionSettings" OR
       gui_source_contents MATCHES "javelin::jmap::submission::ComposeService" OR
       gui_source_contents MATCHES "javelin::app::LongPoll(Service|Coordinator|AccountConfiguration)")
        message(FATAL_ERROR "GUI boundary violation in ${gui_source}")
    endif()
endforeach()
target_include_directories(javelin_gui PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)
target_link_libraries(javelin_gui
    PUBLIC
        Qt::Core
        Qt::Network
        Qt::Svg
        Qt::Widgets
        Qt::WebEngineWidgets
        KF6::ConfigWidgets
        KF6::XmlGui
        KF6::TextEditor
    PRIVATE
        Qt::Concurrent
        javelin_jmap
)
javelin_set_project_warnings(javelin_gui)
javelin_enable_static_analysis(javelin_gui)
javelin_enable_sanitizers(javelin_gui)
# QCoro uses exceptions internally
target_compile_options(javelin_gui PRIVATE -fexceptions)

add_library(javelin_calendar_gui STATIC
    src/gui/calendar/MonthCalendarLayout.cpp
    src/gui/calendar/MonthCalendarLayout.h
    src/gui/calendar/MonthCalendarWidget.cpp
    src/gui/calendar/MonthCalendarWidget.h
    src/gui/calendar/EventDialog.cpp
    src/gui/calendar/EventDialog.h
    src/gui/calendar/RecurrenceDialog.cpp
    src/gui/calendar/RecurrenceDialog.h
    src/gui/calendar/RecurrencePattern.cpp
    src/gui/calendar/RecurrencePattern.h
)
target_include_directories(javelin_calendar_gui PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)
target_link_libraries(javelin_calendar_gui PUBLIC Qt::Widgets PRIVATE javelin_jmap)
javelin_set_project_warnings(javelin_calendar_gui)
javelin_enable_static_analysis(javelin_calendar_gui)
javelin_enable_sanitizers(javelin_calendar_gui)
target_compile_options(javelin_calendar_gui PRIVATE -fexceptions)

add_library(javelin_app STATIC
    src/app/AccountConnectionSettings.h
    src/app/ApplicationErrorCoordinator.cpp
    src/app/ApplicationErrorCoordinator.h
    src/app/CalendarNotificationService.cpp
    src/app/CalendarNotificationService.h
    src/app/ComposeService.cpp
    src/app/ComposeService.h
    src/app/ApplicationBootstrap.cpp
    src/app/ApplicationBootstrap.h
    src/app/DesktopNotificationController.cpp
    src/app/DesktopNotificationController.h
    src/app/FullMailSyncService.cpp
    src/app/FullMailSyncService.h
    src/app/MailIndexService.cpp
    src/app/MailIndexService.h
    src/app/LocalMaintenanceService.cpp
    src/app/LocalMaintenanceService.h
    src/app/InlineMessageSchemeHandler.cpp
    src/app/InlineMessageSchemeHandler.h
    src/app/LongPollService.cpp
    src/app/LongPollService.h
    src/app/MailApplicationService.cpp
    src/app/MailApplicationService.h
    src/app/MailboxSelectionMutation.cpp
    src/app/MailboxSelectionMutation.h
    src/app/MessageSelection.cpp
    src/app/MessageSelection.h
    src/app/MailboxSyncCoverage.cpp
    src/app/MailboxSyncCoverage.h
    src/app/MessageNavigationCoordinator.cpp
    src/app/MessageNavigationCoordinator.h
    src/app/WebEngineSetup.h
    src/app/ProcessServices.cpp
    src/app/ProcessServices.h
    src/app/WorkScheduler.cpp
    src/app/WorkScheduler.h
    src/gui/tasks/TaskCenterDialog.cpp
    src/gui/tasks/TaskCenterDialog.h
)
qt_add_resources(javelin_app "javelin_icons"
    PREFIX "/icons"
    BASE "res"
    FILES
        "res/icon.svg"
        "res/thunderbird-icons/arrow-down-12.svg"
        "res/thunderbird-icons/arrow-right-12.svg"
        "res/thunderbird-icons/archive.svg"
        "res/thunderbird-icons/attachment.svg"
        "res/thunderbird-icons/cloud-download.svg"
        "res/thunderbird-icons/delete.svg"
        "res/thunderbird-icons/display-options.svg"
        "res/thunderbird-icons/draft.svg"
        "res/thunderbird-icons/filter.svg"
        "res/thunderbird-icons/folder.svg"
        "res/thunderbird-icons/forward.svg"
        "res/thunderbird-icons/inbox.svg"
        "res/thunderbird-icons/more.svg"
        "res/thunderbird-icons/nav-left.svg"
        "res/thunderbird-icons/nav-right.svg"
        "res/thunderbird-icons/new-mail.svg"
        "res/thunderbird-icons/remote-blocked.svg"
        "res/thunderbird-icons/replies.svg"
        "res/thunderbird-icons/reply-all.svg"
        "res/thunderbird-icons/reply.svg"
        "res/thunderbird-icons/search.svg"
        "res/thunderbird-icons/sent.svg"
        "res/thunderbird-icons/settings.svg"
        "res/thunderbird-icons/spam.svg"
        "res/thunderbird-icons/star.svg"
        "res/thunderbird-icons/starred.svg"
        "res/thunderbird-icons/trash.svg"
        "res/thunderbird-icons/unread.svg"
)
target_include_directories(javelin_app PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)
target_link_libraries(javelin_app
    PUBLIC
        Qt::Core
        Qt::DBus
        Qt::Widgets
        Qt::WebEngineCore
        Qt::Svg
        KF6::CoreAddons
    PRIVATE
        Qt::Concurrent
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
target_link_libraries(jmap-query PRIVATE javelin_jmap Qt::Core Qt::Network)
target_compile_options(jmap-query PRIVATE -fexceptions)
javelin_set_project_warnings(jmap-query)
javelin_enable_sanitizers(jmap-query)

# KDEInstallDirs can leave KDE_INSTALL_KXMLGUIDIR empty in some local/dev
# configurations; default to the documented DATAROOTDIR/kxmlgui5 location.
if(NOT KDE_INSTALL_KXMLGUIDIR)
    set(KDE_INSTALL_KXMLGUIDIR "${KDE_INSTALL_DATAROOTDIR}/kxmlgui5")
endif()

# Install data files (KXMLGUI .rc, etc.) into the local install prefix so
# KXmlGuiWindow can find them at runtime.  We copy only our own files
# rather than running `cmake --install` (which would also try to install
# FetchContent dependencies).
set(_local_kxmlgui_dir "${CMAKE_INSTALL_PREFIX}/${KDE_INSTALL_KXMLGUIDIR}/javelinmail")
set(_local_kxmlgui_runtime_dir
    "${CMAKE_INSTALL_PREFIX}/${KDE_INSTALL_DATADIR}/kxmlgui/javelinmail")
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
            "${CMAKE_INSTALL_PREFIX}/${KDE_INSTALL_DATADIR}/javelinmail/models/fasttext"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${PROJECT_SOURCE_DIR}/res/models/fasttext"
            "${CMAKE_INSTALL_PREFIX}/${KDE_INSTALL_DATADIR}/javelinmail/models/fasttext"
        COMMAND ${CMAKE_COMMAND} -E touch "${_install_data_stamp}"
        DEPENDS
            "${PROJECT_SOURCE_DIR}/res/javelinmailui.rc"
            "${PROJECT_SOURCE_DIR}/res/models/fasttext/LICENSE"
            "${PROJECT_SOURCE_DIR}/res/models/fasttext/lid.176.ftz"
            "${PROJECT_SOURCE_DIR}/res/models/fasttext/README.md"
        COMMENT "Installing data files to local prefix"
        VERBATIM
    )
    add_custom_target(install_data DEPENDS "${_install_data_stamp}")
endif()

qt_add_translations(
    TARGETS Javelin-Mail
    TS_FILES Javelin-Mail_en_NZ.ts
)

target_link_libraries(Javelin-Mail
    PRIVATE
        javelin_app
        Qt::Svg
)
target_compile_definitions(Javelin-Mail
    PRIVATE
        JAVELIN_APP_VERSION="${PROJECT_VERSION}"
        JAVELIN_DATA_DIR="${CMAKE_INSTALL_PREFIX}/${KDE_INSTALL_DATADIR}"
        JAVELIN_FASTTEXT_LANGUAGE_MODEL_PATH="${CMAKE_INSTALL_PREFIX}/${KDE_INSTALL_DATADIR}/javelinmail/models/fasttext/lid.176.ftz"
)
if(JAVELIN_ENABLE_LOCAL_DATA_INSTALL)
    add_dependencies(Javelin-Mail install_data)
endif()
javelin_set_project_warnings(Javelin-Mail)
javelin_enable_static_analysis(Javelin-Mail)
javelin_enable_sanitizers(Javelin-Mail)

javelin_add_format_targets(${JAVELIN_FORMAT_FILES})

include(cmake/CheckCoroutineOwnership.cmake)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

install(TARGETS Javelin-Mail
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES res/javelinmailui.rc
    DESTINATION ${KDE_INSTALL_KXMLGUIDIR}/javelinmail
)

install(DIRECTORY res/models/fasttext/
    DESTINATION ${KDE_INSTALL_DATADIR}/javelinmail/models/fasttext
)

install(FILES res/javelinmail.desktop
    DESTINATION ${KDE_INSTALL_APPDIR}
)

install(FILES res/icon.svg
    DESTINATION ${KDE_INSTALL_ICONDIR}/hicolor/scalable/apps
    RENAME javelinmail.svg
)

qt_generate_deploy_app_script(
    TARGET Javelin-Mail
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
if(WIN32 OR APPLE)
    install(SCRIPT ${deploy_script})
endif()
