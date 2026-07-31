# Daemon and GUI split implementation plan

## Authority and purpose

[DAEMON_GUI_ARCHITECTURE.md](DAEMON_GUI_ARCHITECTURE.md) is the authoritative design. This document
turns that design into an ordered implementation programme for the current Javelin codebase. It does
not redefine process ownership, consistency, failure semantics, settings authority, cache behaviour,
or UX policy.

When this plan names a type, target, directory, or class that does not yet exist, the name is a
recommended implementation name rather than a new architectural requirement. The contracts in the
design document take precedence over naming convenience.

The implementation must preserve the existing contracts listed under
[Design constraints on existing subsystems](DAEMON_GUI_ARCHITECTURE.md#design-constraints-on-existing-subsystems),
especially optimistic consistency, authoritative query windows, Undo/Redo ordering, and stable user
intent.

## Completion definition

The split is complete when:

- the installed GUI executable contains Widgets and WebEngine presentation but no JMAP transport,
  synchronization coordinator, write-capable cache repository, mutation executor, notification
  publisher, tray implementation, or canonical `QSettings` access;
- the installed daemon executable owns every responsibility listed under
  [The daemon is the sole operational authority](DAEMON_GUI_ARCHITECTURE.md#the-daemon-is-the-sole-operational-authority);
- GUI commands, settings, materialization requests, activation, invalidations, status, and failures
  cross one typed interface whose in-process and socket implementations pass the same conformance
  tests;
- the GUI uses only the read surface defined by
  [The GUI is a read-only cache client](DAEMON_GUI_ARCHITECTURE.md#the-gui-is-a-read-only-cache-client);
- closing the final GUI window can terminate the GUI process while daemon synchronization,
  notifications, tray actions, delayed send, and background work continue; and
- the required functional, failure, performance, and UX tests in the design document pass from a
  clean checkout.

## Migration rules

Every phase must leave the application buildable and usable. Changes should be committed in small,
bisectable groups whose tests establish the new boundary before old access is removed.

Do not maintain two permanent paths for the same operation. A short-lived adapter may bridge an
individual migration step, but once a feature uses the typed boundary, its direct service path must be
removed in the same phase. The socket transport must not be introduced while GUI controllers still
reach concrete daemon services.

Do not weaken a transaction, split an atomic consistency transition, advance a state token early, or
bypass mutation projection to meet a phase deadline. The correctness rule in
[Database concurrency](DAEMON_GUI_ARCHITECTURE.md#database-concurrency) applies throughout the
migration, including while everything still runs in one process.

## Current migration surface

The current monolithic process is assembled by `ApplicationBootstrap` and `ProcessServices`.
`ProcessServices` owns the main database connection, JMAP transports, synchronization, repositories,
mutations, history, deferred send, indexing, maintenance, translation, and read services. It also
installs a WebEngine URL scheme handler. `ApplicationBootstrap` owns the main window, system tray,
notifications, network reachability, account settings application, and process lifetime.

The most important existing direct crossings are:

- `MainWindow` receives concrete repositories and application services;
- message, compose, contact, calendar, file, refresh, and Undo controllers call concrete daemon-side
  services;
- GUI widgets and controllers read or write `QSettings` directly through `PreferencesDialog`,
  `MainWindowStateStore`, message appearance, calendar colour, search persistence, compose, and file
  preferences;
- GUI presentation repositories share the same write-capable `DatabaseConnection` as operational
  services; and
- `javelin_application` links `Qt::WebEngineCore` because GUI-only scheme handling is mixed into
  `ProcessServices`.

These are migration inputs, not exceptions to the target architecture.

## Phase dependency order

```text
0. Baseline and guardrails
        |
1. Typed boundary and in-process endpoint
        |
2. Canonical daemon settings
        |
3. Separate cache read and write surfaces
        |
4. Route every application command through the boundary
        |
5. Route materialization, invalidation, and presentation refresh
        |
6. Complete compose, file, lease, and scheduling boundary work
        |
7. Split service composition and CMake dependency graph
        |
8. Add framed local-socket transport
        |
9. Split executables and implement lifecycle/recovery
        |
10. Move tray and notifications
        |
11. Remove transitional paths and enforce architecture
        |
12. Performance, reliability, and release gate
```

Phases 2 and 3 may be developed in parallel after phase 1, but both must finish before broad command
migration. The executable split must not begin before phases 1 through 7 are complete.

## Phase 0: baseline and architectural guardrails

### Work

1. Record the current clean Debug test count, optimized test count, startup behaviour, idle process
   memory, idle SQLite writes, and representative command latency.
2. Add or extend lightweight dependency checks that can fail CI when:
   - GUI targets include daemon-only service headers;
   - GUI targets link network transport, synchronization, mutation, history execution, or
     write-capable cache targets;
   - daemon-safe targets link Widgets or WebEngine; or
   - GUI production sources instantiate canonical `QSettings` after the settings migration.
3. Add reusable test fixtures for a temporary runtime directory, cache path, settings identity, and
   paired in-process client/server endpoint.
4. Identify every GUI-to-service call and every direct GUI settings access. Keep the inventory in
   tests or implementation issues rather than duplicating it permanently in this document.

### Exit gate

- The existing application still behaves identically.
- The baseline measurements are reproducible.
- New boundary checks are present but initially permit explicitly listed legacy crossings so later
  phases can remove the allow-list monotonically.

## Phase 1: typed boundary and in-process endpoint

This phase implements the first step in
[Implementation sequence](DAEMON_GUI_ARCHITECTURE.md#1-define-typed-process-boundary-interfaces-in-process)
without serialization.

### Work

1. Create a small `javelin_protocol` target containing only process-boundary value types and pure
   framing-independent interfaces. It must not depend on Widgets, WebEngine, SQLite connections,
   JMAP transports, or concrete application services.
2. Define the closed typed families required by
   [IPC protocol](DAEMON_GUI_ARCHITECTURE.md#ipc-protocol):
   - application commands and command IDs;
   - acceptance, rejection, and later operation failure;
   - materialization requests, request IDs, and scope IDs;
   - settings snapshot/update values and revisions;
   - cache invalidations and epochs;
   - daemon/account status;
   - activation routes;
   - cache-access suspend/resume messages; and
   - protocol/build/cache identity values.
3. Define narrow client-side ports, for example:
   - `CommandClient`;
   - `MaterializationClient`;
   - `SettingsClient`;
   - `DaemonStatusClient`;
   - `ActivationClient`; and
   - `CacheAccessClient`.
4. Implement an in-process endpoint that dispatches the same typed request and response values that
   the socket will eventually carry. Do not expose concrete service pointers through this endpoint.
5. Add a protocol conformance test suite that is transport-neutral. Initially run it against the
   in-process endpoint; phase 8 will run the same suite against the local socket.
6. Define size accounting and validation hooks now, even though the in-process endpoint does not need
   framing. This prevents the in-process API from accepting values the socket implementation cannot
   safely encode later.

### Initial integration

Wire one low-risk command and one transient request through the endpoint as a proof of shape. Good
candidates are an explicit account refresh command and a cancellable visible-window materialization
request. Remove their old direct call paths once the typed paths pass tests.

### Exit gate

- The protocol target has no dependency on GUI or operational implementation targets.
- Direct and later failure are represented separately as required by
  [Direct rejection versus later failure](DAEMON_GUI_ARCHITECTURE.md#direct-rejection-versus-later-failure).
- The proof commands work through the in-process endpoint with no raw JMAP or SQLite type in the
  process-boundary API.

## Phase 2: move all persisted settings behind the daemon boundary

This phase implements [Settings](DAEMON_GUI_ARCHITECTURE.md#settings) and
[Settings lifecycle](DAEMON_GUI_ARCHITECTURE.md#settings-lifecycle). All persisted settings remain one
bulk daemon-owned schema, including GUI workspace state.

### Work

1. Define one typed `SettingsSnapshot` covering the complete currently persisted shape:
   - account and authentication configuration;
   - synchronization and notification choices;
   - compose, translation, attachment, appearance, calendar, and search preferences;
   - window, tab, sort, and other retained workspace state; and
   - settings schema and revision.
2. Create a daemon-side settings repository/service responsible for:
   - the canonical `QSettings` identity;
   - legacy migration and validation;
   - complete snapshot loading;
   - revision-checked typed updates;
   - atomic application of related values; and
   - notification of operational services after persistence.
3. Convert `PreferencesDialog` into a pure editor of a supplied snapshot. Its Apply/OK path submits a
   typed update and handles stale-revision rejection; its static storage helpers are removed.
4. Replace direct settings access in, at minimum:
   - `ApplicationBootstrap` account configuration;
   - `TranslationService` and `ComposePreferences`;
   - `ApplicationErrorCoordinator`;
   - `MainWindowStateStore`, `TabPersistence`, and search-session persistence;
   - calendar colour overrides;
   - message appearance and remote-content preferences;
   - attachment save preferences;
   - compose account/default identity lookups; and
   - account refresh and mailbox/notification selection.
5. At GUI startup, fetch exactly one complete snapshot after the daemon handshake. Store the active
   snapshot in a GUI settings model or immutable shared value and pass typed subsets to presenters and
   dialogs. Do not reintroduce per-widget canonical storage reads.
6. Debounce high-frequency workspace persistence and submit aggregate revisioned updates. Preserve
   final normal-shutdown persistence through the daemon.
7. Add migration tests using copies of representative legacy settings and verify that a failed or
   ambiguous migration does not start operational services with empty or invented configuration.

### Exit gate

- Production GUI sources no longer instantiate canonical `QSettings`.
- The daemon can start and configure all operational services without a GUI.
- A GUI receives the complete settings state in one typed startup snapshot.
- Stale settings writes are rejected and tested.

## Phase 3: separate daemon write access from GUI read access

This phase implements
[Make the daemon side the sole cache writer](DAEMON_GUI_ARCHITECTURE.md#2-make-the-daemon-side-the-sole-cache-writer),
[The GUI is a read-only cache client](DAEMON_GUI_ARCHITECTURE.md#the-gui-is-a-read-only-cache-client),
and [Database concurrency](DAEMON_GUI_ARCHITECTURE.md#database-concurrency).

### Work

1. Introduce one shared cache-location provider that returns paths and cache identity without opening a
   database.
2. Create distinct factories:
   - a daemon factory that may create, migrate, validate, repair, and open write-capable connections;
   - a GUI factory that opens existing databases read-only, enables `query_only`, never migrates, and
     uses the read contention policy from the design.
3. Split mixed repositories into read and write surfaces where necessary. GUI-visible query types may
   remain shared value types, but no GUI-linked repository API may expose mutation methods or a mutable
   `DatabaseConnection`.
4. Establish the `javelin_cache_read` target proposed under
   [Build and dependency boundaries](DAEMON_GUI_ARCHITECTURE.md#build-and-dependency-boundaries).
   Move or wrap the query parts used by:
   - mailbox and message lists;
   - message view snapshots;
   - contacts and address suggestions;
   - identities;
   - calendars and visible ranges;
   - task/history labels needed for presentation; and
   - vault metadata.
5. While still in one process, construct daemon services with the writer connection and GUI models with
   separate persistent read-only worker connections. This deliberately exercises WAL and snapshot
   behaviour before a process boundary complicates diagnosis.
6. Implement the cache-access barrier as an in-process coordination interface. Test cancellation of
   reads, closure of every GUI connection, migration/replacement, reopen, and full active-view reload.
7. Add a cache instance ID distinct from daemon instance and schema version. Assign a new ID whenever
   the underlying cache file is replaced.
8. Add `data_version` support to GUI reconnect/read coordination without adding persistent generation
   writes.

### Exit gate

- No GUI object receives the daemon writer connection.
- GUI connection creation cannot run migrations or issue writes even accidentally.
- Cache replacement while GUI reads are active succeeds only through the tested barrier.
- Existing optimistic, query-window, and cache-recovery tests still pass unchanged in semantics.

### Phase 3 status

Complete for the current in-process composition. The cache read/write boundary is now explicit and
covered by the build graph and tests:

- `CacheLocationProvider` resolves the database, vault, search-index, and cache-instance identity
  without opening SQLite; daemon and GUI factories are separate.
- GUI-visible account, mailbox, query, message-view, contact, identity, and calendar reads use
  reader ports backed by persistent `ReadOnlyDatabaseConnection` instances. Background mailbox,
  search, address-suggestion, message-view, and calendar reads use the read-only worker factory.
- `javelin_cache_read` contains the shared read-side database/vault/email/source primitives, while
  the remaining service adapters stay in `javelin_jmap` during the single-process transition.
- The GUI source boundary rejects direct writer-repository, mutable-database, transport, and sync
  includes. Contact invalidations are relayed from the daemon repository to the GUI read surface
  without exposing the writer to widgets.
- `CacheAccessBarrier` coordinates close, replacement, and reopen; tests cover ordering, rollback,
  and reopening a replaced cache. `data_version` is available through the database and query-reader
  surfaces for reconnect/read coordination.

Task and history command ownership remains an application-service concern and is intentionally left
for Phase 4 command routing; it does not expose a cache connection to GUI presentation code.

## Phase 4: route all application commands through the typed boundary

This phase moves interpretation and execution behind the daemon endpoint while retaining the current
single process. Follow [Mutations and optimistic consistency](DAEMON_GUI_ARCHITECTURE.md#mutations-and-optimistic-consistency),
[Undo and Redo](DAEMON_GUI_ARCHITECTURE.md#undo-and-redo), and
[Command identity and retries](DAEMON_GUI_ARCHITECTURE.md#command-identity-and-retries).

### Work common to every command

1. Add a daemon command dispatcher over existing application coordination services. It validates the
   typed command, invokes exactly one application workflow, and returns typed admission.
2. Assign a UUID at the GUI intent boundary and retain it for retry. Do not generate replacement UUIDs
   after an uncertain connection loss.
3. Include the committed cache epoch and bounded affected hints in successful admission.
4. Preserve direct rejection versus later failure. GUI controllers must not reinterpret local
   admission as server success.
5. Map service-specific errors to a closed process-boundary error taxonomy while retaining diagnostic
   detail for logs.
6. Add one conformance test per command family covering acceptance, rejection, same-UUID retry, later
   failure, and disconnect timing where applicable.

### Migration order

Migrate one vertical slice at a time and remove its direct path immediately:

1. **Mail commands**
   - mark read/unread, flag/unflag, move, archive, junk, delete, restore, and mailbox actions;
   - refresh and role-sensitive application actions;
   - update `MessageCommandController`, mailbox controllers, and related menu/action routing.
2. **Undo and Redo**
   - request the observed entry ID and history revision;
   - render daemon-provided labels and history availability;
   - present stale-head rejection directly.
3. **Compose and draft commands**
   - create/reply/forward/edit-draft preparation;
   - save revision, stage attachment, send, cancel delayed send, and draft deletion;
   - update `ComposeTabController` and `ComposeTabWidget` to depend on typed ports and read values.
4. **Contacts and address books**
   - replace `ContactCommandPort` and refresh-service references in GUI constructors with the generic
     typed command/materialization clients;
   - retain pure contact parsing/editing helpers as shared domain code.
5. **Calendar**
   - route visible-range materialization and every event/calendar mutation through the boundary;
   - retain pure recurrence and editor transformation helpers in shared domain code.
6. **Sieve, files, translation, account refresh, and maintenance actions**
   - route all operations that can write cache, settings, vault, or remote state through typed
     commands or materialization requests;
   - remove GUI helpers that take `MailApplicationService`, `ComposeService`, or other concrete daemon
     services.

### Exit gate

- `MainWindow` and GUI controllers receive typed clients and read repositories, not concrete daemon
  application services.
- No stateful GUI action bypasses the command dispatcher.
- Every operation that previously used optimistic consistency still uses the same transaction and
  reconciliation subsystem behind the dispatcher.
- The GUI observes its own projection from the admission epoch without waiting for a duplicate
  invalidation.

## Phase 5: materialization, invalidation, and presentation refresh

This phase implements [Cache invalidation and refresh](DAEMON_GUI_ARCHITECTURE.md#cache-invalidation-and-refresh),
[Materialization requests](DAEMON_GUI_ARCHITECTURE.md#materialization-requests), and the user-intent
rules in [Data changes never clobber user intent](DAEMON_GUI_ARCHITECTURE.md#data-changes-never-clobber-user-intent).

### Work

1. Replace direct daemon-service-to-widget signals with typed cache invalidations and transient status.
   Keep cache commits as the source of visible state.
2. Add one daemon invalidation publisher that:
   - advances volatile epochs only after commit;
   - merges compatible domains and bounded affected keys;
   - preserves lossless command and barrier messages; and
   - applies the queue/backpressure policy from the design.
3. Convert mailbox, search, message content, contacts, calendar, address suggestion, history/task, and
   other GUI refresh paths to scoped materialization requests and cache reads.
4. Give every presenter a refresh generation and scope identity. Discard stale worker results before
   model installation.
5. Move SQL reads and model-diff preparation off the GUI thread. Apply only bounded Qt model operations
   on the GUI thread.
6. Add a model-update guard that suppresses selection-driven navigation until stable IDs, current
   object, viewport anchor, focus, and editor state are restored.
7. Replace unnecessary model resets with precise changes. Where reset remains unavoidable, test the
   complete preservation path and explicit selected-object deletion state.
8. Track installed and pending query windows separately during sparse jumps and navigation.
9. Cancel undispatched requests when their GUI scope closes; allow already dispatched useful cache
   materialization to finish without activating stale UI.

### Exit gate

- An older read finishing late cannot roll a model backward.
- Push, mutation projection, rejection, background sync, and pagination all refresh through the same
  invalidation/read mechanism.
- Selection and viewport preservation tests cover insertion, removal, reset, page replacement, and
  selected-object deletion.
- No database query or unbounded diff runs on the GUI thread.

## Phase 6: compose transfer, vault leases, and priority scheduling

This phase completes the process-sensitive mechanisms described in
[Compose behaviour](DAEMON_GUI_ARCHITECTURE.md#compose-behaviour),
[Materialization requests](DAEMON_GUI_ARCHITECTURE.md#materialization-requests), and
[Resource and scheduling policy](DAEMON_GUI_ARCHITECTURE.md#resource-and-scheduling-policy).

### Compose and attachments

1. Define the bounded complete compose revision value used by autosave and Send.
2. Add revision ordering and exact accepted-manifest validation in the daemon adapter around the
   existing compose services.
3. Add attachment staging commands and immutable daemon-owned staging storage. The GUI must receive
   acceptance only after the staged object is safe from source-path replacement.
4. Ensure Send references an accepted body revision and accepted attachment manifest.
5. Test out-of-order saves, GUI shutdown during save, source-file replacement, failed staging,
   delayed-send cancellation, and retry after lost admission.

### Vault lifetime

1. Add a lease or open-handle abstraction for actively displayed/downloaded vault content.
2. Make eviction respect active leases and release all leases on GUI disconnect.
3. Keep paths and byte streams out of ordinary object invalidations.

### Work scheduling

1. Add priority-aware admission in front of the existing writer coordinator.
2. Classify foreground commands, visible materialization, prefetch, indexing, offline synchronization,
   and maintenance according to the design.
3. Break only logically independent background commits into bounded batches. Never split one required
   atomic projection, query-window commit, reconciliation, or state-token transition.
4. Add account fairness and bounded queued work.
5. Instrument queue wait, transaction duration, and foreground admission latency.

### Exit gate

- Compose and attachment behaviour is fully expressible through typed boundary values.
- Active vault content cannot disappear between metadata lookup and use.
- Foreground work can pass queued background work but never interrupt an active transaction.
- Scheduling tests prove both responsiveness and unchanged consistency semantics.

## Phase 7: split service composition and enforce the target dependency graph

This phase prepares the codebase for two executables without yet adding socket transport. Follow
[Build and dependency boundaries](DAEMON_GUI_ARCHITECTURE.md#build-and-dependency-boundaries).

### Work

1. Replace `ProcessServices` with two explicit composition roots:
   - `DaemonServices`, owning database writes, JMAP, synchronization, commands, history, deferred send,
     notifications support, settings, scheduling, indexing, maintenance, and invalidation publication;
   - `GuiServices`, owning typed clients, read-only repositories/workers, presentation support,
     WebEngine integration, and GUI settings snapshot state.
2. Split `ApplicationBootstrap` responsibilities into daemon-safe and GUI-only bootstrap classes while
   they can still be hosted by one temporary executable for integration testing.
3. Move `InlineMessageSchemeHandler` and all `QWebEngineProfile` work to the GUI side and make the
   handler read through the GUI cache/vault surface.
4. Keep translation networking/cache writes on the operational side if translation remains a cached
   service; expose it as a typed command/materialization workflow. Keep only HTML presentation in the
   GUI.
5. Reshape CMake toward the targets named by the design:
   - `javelin_protocol`;
   - `javelin_cache_read`;
   - `javelin_daemon_core`;
   - `javelin_gui`; and
   - thin bootstrap targets.
6. Remove `Qt::WebEngineCore`, Widgets, and GUI source dependencies from daemon-core targets.
7. Remove JMAP transport, write repositories, concrete application coordination, and canonical
   settings dependencies from GUI targets.
8. Run the complete application through the in-process endpoint using the two composition roots.

### Exit gate

- The full application works in one process while respecting the final target dependency direction.
- CMake, include, and linkage checks prove the daemon core is GUI-free and the GUI is operationally
  read-only.
- No GUI controller retains a concrete daemon-service reference.
- Introducing a socket no longer requires changing application semantics or controller APIs.

## Phase 8: framed local-socket transport

This phase implements [Introduce the local socket](DAEMON_GUI_ARCHITECTURE.md#6-introduce-the-local-socket)
and [Locality](DAEMON_GUI_ARCHITECTURE.md#locality).

### Work

1. Choose and document the concrete encoding and framing. Keep the typed values from phase 1 unchanged
   unless an actual bounded-encoding defect is found.
2. Implement:
   - frame header and maximum sizes;
   - protocol and build compatibility negotiation;
   - request/reply correlation;
   - asynchronous ordered writes;
   - bounded output queues and coalescing classes;
   - malformed/unknown message rejection;
   - peer credential and runtime-directory checks;
   - disconnect classification; and
   - cancellation and scope cleanup.
3. Implement `SocketDaemonEndpoint` and `SocketDaemonClient` behind the same interfaces as the
   in-process endpoint.
4. Run the phase-1 transport-neutral conformance suite against both implementations.
5. Add adversarial tests for partial frames, oversized frames, invalid enums/variants, unknown message
   kinds, reply loss, slow readers, disconnect during command admission, and reconnect.
6. Add cache-access barrier transport tests before using the socket in production bootstrap.

### Exit gate

- The socket implementation is behaviourally equivalent to the in-process implementation.
- A slow GUI cannot cause unbounded daemon memory growth.
- No C++ object address, Qt object, raw JMAP payload, bearer token, or mutable SQLite object crosses the
  socket.

## Phase 9: split executables and implement lifecycle

This phase implements [Split the executables](DAEMON_GUI_ARCHITECTURE.md#7-split-the-executables),
[Daemon startup and version compatibility](DAEMON_GUI_ARCHITECTURE.md#daemon-startup-and-version-compatibility),
[Reconnection](DAEMON_GUI_ARCHITECTURE.md#reconnection), and
[Single GUI instance and activation](DAEMON_GUI_ARCHITECTURE.md#single-gui-instance-and-activation).

### Daemon executable

1. Add `javelind` with a daemon bootstrap and `DaemonServices`.
2. Establish the canonical settings identity before migration or service construction.
3. Open/migrate/recover the cache, recover mutation/history/deferred-send state, start operational
   coordinators, then expose readiness.
4. Host the private local socket and one active GUI connection.
5. Remain alive without a GUI and release GUI-scoped work on disconnect.

### GUI executable

1. Replace the monolithic bootstrap with a GUI bootstrap that:
   - locates, starts, or safely restarts the daemon;
   - completes compatibility negotiation;
   - retrieves the bulk settings snapshot;
   - opens read-only cache workers;
   - restores the workspace; and
   - processes any queued activation route.
2. Enforce one GUI process through daemon-mediated activation.
3. On daemon failure, keep rendered state only inside the coherent recovery surface defined by the
   design; do not expose a nominal daemon-free application mode.
4. Implement same-daemon and replaced-daemon reconnect using cache instance, schema, epoch, and
   `data_version`.
5. Implement graceful protocol-version restart. Do not terminate the daemon inside a transaction or
   while operation state remains unclassified.

### Cache replacement

Implement the production cache-access suspend/acknowledge/resume barrier before enabling automatic
migration or corruption recovery with a connected GUI.

### Exit gate

- Closing the GUI process leaves `javelind` operational.
- Starting the GUI with no daemon, an old daemon, a hung daemon, or an incompatible daemon follows one
  tested bootstrap/recovery flow.
- A second GUI invocation activates the existing GUI or becomes the sole GUI if none exists.
- Cache migration/replacement cannot occur while GUI handles remain open.

## Phase 10: move tray and notifications into the daemon

This phase implements [Notifications and tray icon](DAEMON_GUI_ARCHITECTURE.md#notifications-and-tray-icon)
and [Move tray and notification ownership](DAEMON_GUI_ARCHITECTURE.md#8-move-tray-and-notification-ownership).

### Work

1. Move `DesktopNotificationController`, notification dispatch recovery, calendar notification
   scanning, notification actions, and notification retry ownership into daemon composition.
2. Route notification activation to the daemon activation queue; launch or raise the GUI and deliver
   the route only after GUI readiness.
3. Move tray ownership and task summary/actions into the daemon. Prefer direct StatusNotifierItem/menu
   integration through QtDBus when implementing the Linux path.
4. Keep synchronization independent of notification and tray availability.
5. Move network reachability and suspend/resume transport recovery out of the GUI bootstrap.
6. Define daemon quit separately from closing the GUI. Tray Quit must intentionally stop daemon
   services and the GUI, while closing all GUI windows stops only the GUI process.

### Exit gate

- New mail and calendar notifications, delayed-send actions, and tray controls work with no GUI
  process.
- D-Bus or shell failure does not stop JMAP synchronization.
- Notification activation preserves exact target identity and does not alter current GUI state until
  treated as an explicit navigation request.

## Phase 11: remove transitional architecture

### Work

1. Remove the temporary single-process composition executable and production use of the in-process
   endpoint. Retain the in-process endpoint only as a fast test implementation if useful.
2. Delete `ProcessServices` and the old mixed `ApplicationBootstrap` once their responsibilities are
   fully represented by the two composition roots.
3. Remove all legacy direct settings helpers and storage access from GUI code.
4. Remove all direct GUI service constructors and compatibility adapters.
5. Remove GUI linkage to daemon-core implementation targets and daemon linkage to GUI/WebEngine
   targets.
6. Tighten phase-0 architecture checks by deleting every legacy allow-list entry.
7. Update developer documentation and executable/install/service packaging.

### Exit gate

The following searches or equivalent build checks return no production violations:

- GUI includes of concrete daemon coordination services;
- GUI construction of write-capable database connections;
- GUI canonical `QSettings` access;
- daemon references to Widgets, WebEngine, `MainWindow`, or presentation models;
- direct mutation/cache writes outside the existing consistency subsystem; and
- old dual bootstrap or fallback paths.

## Phase 12: performance, reliability, and release gate

Use [Required tests](DAEMON_GUI_ARCHITECTURE.md#required-tests) and
[Performance and UX acceptance criteria](DAEMON_GUI_ARCHITECTURE.md#performance-and-ux-acceptance-criteria)
as the authoritative checklist.

### Required validation

1. Run the complete Debug build and test suite under the repository test harness.
2. Run optimized tests, sanitizer-enabled tests, formatting, clang-tidy, and clazy as supported by the
   existing project workflow.
3. Exercise deterministic process tests for:
   - daemon/GUI startup in every ordering;
   - command admission and lost replies;
   - daemon crash before and after dispatch;
   - GUI crash with accepted operations;
   - stale reads and invalidation races;
   - cache migration and replacement;
   - settings migration and stale updates;
   - singleton activation and notification routes;
   - suspend across delayed send;
   - D-Bus failure and restoration; and
   - socket backpressure.
4. Measure and compare against phase 0:
   - GUI startup and first usable cached view;
   - common command admission latency;
   - rapid navigation responsiveness;
   - idle daemon RSS and CPU wake-ups;
   - idle and synchronization SQLite write rates;
   - WAL growth and checkpoint behaviour;
   - large refresh transaction duration;
   - GUI RSS before and after WebEngine use; and
   - memory released after GUI exit.
5. Investigate regressions rather than loosening correctness or UX criteria. Optimise preparation,
   batching, indexes, invalidation detail, and model patches before considering any architectural
   exception.

### Final release gate

The split may replace the monolithic release only when:

- all completion-definition conditions in this document hold;
- every architecture invariant remains true under process failure and reconnect;
- no known operation can bypass optimistic consistency or history ordering;
- no cache migration/replacement path can race an open GUI handle;
- no stale model result can overwrite newer user-visible state;
- daemon idle resource use is acceptable for continuous operation; and
- closing and reopening the GUI is observably faster and lighter than retaining the monolithic
  Widgets/WebEngine process.

## Suggested commit structure

The exact number of commits is implementation-dependent, but each series should remain reviewable and
bisectable. A practical grouping is:

1. protocol values and in-process conformance harness;
2. daemon settings schema/service and GUI snapshot model;
3. read-only database factory and cache-read target;
4. command dispatcher plus one commit series per vertical feature slice;
5. invalidation/materialization infrastructure and presenter generations;
6. compose staging, vault leases, and priority admission;
7. daemon/GUI composition roots and target separation;
8. socket codec, server/client, and transport conformance;
9. daemon and GUI executables plus lifecycle;
10. tray/notification move;
11. removal of transitional paths and dependency enforcement; and
12. performance fixes and final documentation.

Avoid combining protocol invention, broad controller migration, executable splitting, and tray changes
in one commit series. Those changes have different failure modes and should remain independently
reviewable.
