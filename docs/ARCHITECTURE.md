# Architecture

## Status and document map

This document describes the current code-level architecture: the major runtime components, their
ownership boundaries, and the paths followed by commands and synchronized data. User-facing setup
belongs in the repository [README](../README.md), while build and test instructions live in
[DEVELOPMENT.md](DEVELOPMENT.md).

The process split and IPC contract are defined in
[DAEMON_GUI_ARCHITECTURE.md](DAEMON_GUI_ARCHITECTURE.md); its implementation history and remaining
release validation are tracked in
[DAEMON_GUI_IMPLEMENTATION_PLAN.md](DAEMON_GUI_IMPLEMENTATION_PLAN.md). Cross-cutting invariants live
in the focused documents for [optimistic consistency](OPTIMISTIC_CONSISTENCY.md),
[offline mail](OFFLINE_MAIL_ARCHITECTURE.md), [query windows](QUERY_WINDOWS.md),
[database access](DATABASE_ACCESS.md), [Undo/Redo](UNDO_REDO.md),
[email signatures](EMAIL_SIGNATURES_DESIGN.md), [message rendering](RENDERING.md),
[mail export](MAIL_EXPORT_IMPLEMENTATION_PLAN.md), and
[mail import](MAIL_IMPORT_IMPLEMENTATION_PLAN.md). The planned
split between foreground collapsed-query materialization and bounded background thread hydration is
specified in [THREAD_MATERIALIZATION_IMPLEMENTATION_PLAN.md](THREAD_MATERIALIZATION_IMPLEMENTATION_PLAN.md).
The long-term, behavior-preserving module and ownership cleanup is tracked in
[STRUCTURAL_REFACTOR_IMPLEMENTATION_PLAN.md](STRUCTURAL_REFACTOR_IMPLEMENTATION_PLAN.md).

Javelin targets Qt 6.10 or newer, KDE Frameworks 6.27 or newer, KDE PIM 6.8 or newer, and C++23. Glaze provides typed JSON parsing at
the JMAP boundary; QCoro provides coroutine-based Qt networking. KDE Plasma is the primary desktop
integration target rather than merely one supported shell.

## KDE desktop integration

The GUI is intentionally a KDE application built on Qt Widgets, not a generic Qt shell with optional
KDE theming:

- `MainWindow` derives from `KXmlGuiWindow`; menus, toolbar composition, shortcut editing, and saved
  toolbar state use KXMLGUI and KDE standard actions. Feature policy and mutable workflow state live
  in focused controllers rather than in the shell.
- Preferences use `KConfigDialog`, keeping configuration presentation consistent with Plasma and
  other KDE applications while the daemon remains the canonical settings owner.
- Compose source/plain-text modes and the Sieve editor use `KTextEditor`, including KDE syntax and
  editor behavior rather than a private text-editor implementation.
- Icons are resolved through the desktop icon theme, and installed resources follow KDE install
  directories and KXMLGUI lookup conventions.
- `javelind` implements `org.kde.StatusNotifierItem` directly over QtDBus so tray presence and menu
  actions remain available without loading Qt Widgets in the daemon.
- Desktop notifications use the freedesktop notification service with stable activation routes back
  into the KDE GUI.
- On a normal Plasma session, `javelind.service` is attached to the systemd graphical user session.
  Portable packages fall back to launching the adjacent daemon executable because they do not own a
  host systemd unit.

Other Linux desktops can work when they provide compatible StatusNotifierItem, notifications,
icon-theme, D-Bus, and session behavior, but architectural decisions and release validation should
prefer correct KDE Plasma integration.

## Runtime shape

Javelin is a split-process desktop application:

```text
                                 private local JVIP socket
┌────────────────────────────┐  commands / replies / invalidations  ┌───────────────────────────┐
│ javelin                    │ <───────────────────────────────────> │ javelind                  │
│ Qt Widgets + WebEngine     │                                       │ application coordination  │
│ presentation controllers  │                                       │ JMAP + synchronization    │
│ read-only SQLite queries  │                                       │ sole cache/settings writer│
└──────────────┬─────────────┘                                       └─────────────┬─────────────┘
               │ reads                                                             │ writes
               └──────────────────────────┬─────────────────────────────────────────┘
                                          ▼
                              SQLite cache, search indexes,
                              and the filesystem mail vault
                                          │
                                          ▼
                                      JMAP server
```

The server is the recoverable source of truth. SQLite is the local data plane and the immediate
source rendered by the GUI. IPC is the command and coordination plane. A cache commit always occurs
before the daemon publishes its invalidation. The daemon process owns the single authoritative
invalidation epoch; the internal cache-change publisher batches semantic changes but does not
allocate a second pre-boundary epoch.

The end-to-end command path is deliberately one-way:

```text
user action
  -> GUI controller and typed application port
  -> RemoteActionClient
  -> JVIP socket
  -> DaemonRemoteActionDispatcher / CommandDispatcher
  -> daemon application service
  -> typed JMAP method layer
  -> transport
```

The return path is a typed admission or failure result followed by committed cache state and bounded
invalidations. The GUI never constructs JMAP method JSON, resolves credentials, opens a writable
cache connection, or treats an invalidation payload as object state.

## Build and module boundaries

The CMake graph enforces the architectural split:

- `javelin_protocol` owns bounded process-boundary values, framing, and local socket transport. The
  in-process endpoint is test-only support and is not part of the production protocol target.
- `javelin_cache_read` owns database-location discovery and reusable read-only database, MIME, vault,
  and rendering infrastructure.
- `javelin_app_contracts` owns shared QObject application contracts and their generated metaobjects.
- `javelin_mail_model` owns transport-independent cache/storage implementations, search/query values,
  and contact/calendar domain helpers shared by the daemon and GUI compositions. Persistence
  implementations live under `src/storage/`; their established public cache interfaces remain under
  `src/jmap/cache/`.
- `javelin_app_shared` owns list sessions, session construction, message navigation coordination, and
  process-instance locking shared by both process compositions.
- `javelin_jmap` owns typed JMAP protocol, capability negotiation, transport, synchronization,
  protocol-specific repositories, contacts, calendars, submission, Sieve, and optimistic journals.
  It has no Widgets or WebEngine dependency and consumes `javelin_mail_model` rather than recompiling
  shared implementations.
- `javelin_daemon_core` owns daemon application coordination, settings, background work,
  notifications, tray integration, deferred send, history, and synchronization lifecycle.
- `javelin_gui` owns presentation, WebEngine, editing state, selection, and GUI controllers;
  `javelin_gui_session` owns the client-side daemon session/event bridge. GUI-process composition and
  remote application-port adapters live under `src/client/`; the `javelin` executable remains
  composition/bootstrap only.

Configuration fails when a production `.cpp` has more than one target owner, production GUI sources
access canonical `QSettings`, GUI targets link `javelin_jmap` or `javelin_daemon_core`, shared
mail/application targets gain transport or Widgets/WebEngine dependencies, or daemon sources acquire
Widgets/WebEngine dependencies. Test targets additionally reject direct production `.cpp` sources.

### First-run account onboarding

The GUI presents the first-run wizard only while the daemon-owned account list is empty. Daemon
availability is established before the wizard opens; an unavailable daemon is handled by a small
recovery surface that can start it for the current session or enable and start its systemd user
unit.

Account discovery and authentication remain daemon services. Discovery follows the JMAP DNS and
well-known flow, inspects the unauthenticated session when the provider exposes it, and reads OAuth
protected-resource and authorization-server metadata. Providers with dynamic client registration
can use the OAuth Profile for Open Public Clients: RFC 7591 registration metadata, Authorization
Code with PKCE, issuer verification, resource indicators, and a temporary loopback callback through
the user's system browser. Manual HTTPS JMAP URL and bearer-token entry remains available when OAuth
metadata or automatic client registration is unavailable. The wizard renders only friendly
outcomes and typed capability results; it does not expose socket, transport, or wire-format
diagnostics.

## Source and component map

The source tree is organized by responsibility rather than by feature alone:

| Area | Responsibility |
| --- | --- |
| `src/protocol/` | Process-boundary value types, framing, socket transport, correlation, limits, and transport conformance |
| `src/app/` | Stable application contracts and public service interfaces; implementations are grouped below by responsibility rather than accumulated at the root |
| `src/app/account/`, `calendar/`, `compose/`, `contacts/`, `messages/`, `sieve/`, `identity/`, `work/` | Focused application workflow implementations for their domains |
| `src/app/runtime/` | Cross-domain application runtime coordination such as cache barriers, invalidation publication, command dispatch, and process locking |
| `src/app/undo/` | History storage, typed history executors, and Undo/Redo application coordination |
| `src/client/` | GUI-process composition, daemon session/reconnect handling, and typed remote application-port adapters |
| `src/daemon/` | Daemon process/bootstrap composition and lifecycle; `src/daemon/actions/` contains the remote-action dispatcher handlers |
| `src/desktop/` | Desktop integration owned by the daemon, currently notifications and tray/StatusNotifierItem support |
| `src/storage/` | SQLite infrastructure, migrations, and persistence implementations grouped by account/messages/contacts/calendar/compose/identity/sieve/sync |
| `src/jmap/api/` | Session discovery, capabilities, typed JMAP envelopes, HTTP/WebSocket transport, and resource transfer |
| `src/jmap/cache/` | Stable cache/read/repository interfaces and cache value types; persistence `.cpp` implementations live under `src/storage/` |
| `src/jmap/sync/` | State-change sources, refresh planning, reconciliation, mutation projection, and consistency fences |
| `src/jmap/submission/` | Draft snapshots, attachment manifests, compose revisions, and EmailSubmission workflows |
| `src/jmap/contacts/` | JSContact values plus focused protocol, synchronization, mutation/group, media-transfer, and mutation-journal components |
| `src/jmap/calendar/` | JSCalendar values plus focused cache-reader, protocol, synchronization, mutation, recurrence, and occurrence-materialization components |
| `src/jmap/sieve/` | Sieve domain values plus separate protocol/read-validation and mutation mechanics with optimistic mutation support |
| `src/gui/` | KDE/Qt presentation only: shell, feature controllers/widgets, models, delegates, WebEngine rendering, KTextEditor integration, and KConfig preferences |
| `src/observability/` | Shared logging and performance instrumentation implementations |

The principal runtime objects are:

| Component | Process | Role |
| --- | --- | --- |
| `DaemonProcess` | daemon | Starts settings, cache recovery, service composition, sockets, and daemon lifecycle |
| `DaemonServices` | daemon | Composition root for writable repositories, JMAP transports, focused application services, command services, and background work |
| `AccountRuntimeManager` | daemon | Owns account configuration, `AccountSyncCoordinator` lifetimes, session/authentication refresh, network recovery, and account status |
| `MailQueryApplicationService` | daemon | Owns mailbox observations, mailbox/search materialization demand, Thread materialization demand, and query-cache publication |
| `MailMutationApplicationService` | daemon | Expands selections, queues/submits/reconciles Email and mailbox mutations, owns tag jobs, and implements mail history operations |
| `MessageContentApplicationService` | daemon | Coordinates message body, attachment, and source retrieval plus content invalidation |
| `MailNotificationService` | daemon | Owns mail-notification delivery acknowledgement/release/recovery and forwards claimed notification candidates |
| `ContactApplicationService` / `CalendarApplicationService` / `SieveApplicationService` | daemon | Own domain application workflow and history-facing coordination without sharing mail query, mutation, or account-runtime state |
| `ContactProtocolClient` / `ContactSyncEngine` / `ContactMutationEngine` / `ContactMediaService` | daemon | Separate contacts wire access, authoritative cache synchronization, state-changing/group operations, and binary media transfer |
| `CalendarCacheReader` / `CalendarProtocolClient` / `CalendarSyncEngine` / `CalendarMutationEngine` | daemon | Separate cached calendar reads, JMAP protocol access, bounded refresh/materialization, and optimistic calendar/event mutations |
| `SieveProtocolClient` / `SieveMutationEngine` | daemon | Separate Sieve list/load/validation protocol work from save/delete/activation mutation mechanics |
| `SessionRefreshClient` / `AccountBootstrapClient` | daemon | Refresh JMAP session metadata and perform initial account/mailbox bootstrap without exposing query or mutation APIs |
| `MailQueryClient` / `MailQueryMaterializer` | daemon | Execute bounded JMAP mail queries and commit authoritative mailbox/search windows to the cache |
| `MessageContentClient` | daemon | Refresh MIME source/content and read cached source or attachment payloads |
| `EmailMutationEngine` | daemon | Queue exact Email mutations, submit bounded mutation-journal work, and preserve optimistic/ambiguous outcomes |
| `MailboxMutationEngine` | daemon | Execute and reconcile mailbox subscription, create, and destroy mutations through the mailbox mutation journal |
| `DaemonRemoteActionDispatcher` | daemon | Decodes typed remote actions and routes them to application services |
| `CommandDispatcher` | daemon | Admits stateful commands, preserves command identity, and separates rejection from later failure |
| `SettingsRepository` | daemon | Owns the canonical revisioned settings snapshot and migration |
| `WorkScheduler` | daemon | Prioritizes foreground, synchronization, indexing, offline, and maintenance work |
| `ThreadMaterializationCoordinator` | daemon | Coalesces transient Thread targets from committed query windows and admits prefetch or interactive demand without persistent job rows |
| `ThreadMembershipMaterializationWorker` | daemon | Fetches represented Thread membership and missing child Emails in explicit negotiated bounded batches, commits through optimistic consistency, and reconciles membership races |
| `DaemonBackgroundController` | daemon | Owns notifications, reminders, delayed-send actions, network recovery, and tray integration |
| `GuiDaemonSession` | GUI | Connects, negotiates protocol/build identity, handles reconnect, and coordinates cache barriers |
| `GuiServices` | GUI | Constructs read-only repositories and typed remote application-port adapters |
| `RemoteActionClient` | GUI | Correlates bounded request/reply actions over the daemon session |
| `MainWindow` | GUI | Owns the top-level KDE shell, KXMLGUI registration, shared presentation surfaces, and shutdown/persistence coordination |
| `MailWorkspaceController` | GUI | Owns workspace tabs, active-tab identity, mailbox/search list sessions, sort state, restoration, refresh routing, and mail-tab lifecycle |
| `QuickFilterController` / `MailActionController` | GUI | Own quick-filter state/pinning/continuity and mail action availability, trigger routing, tags, and message context-menu policy |
| `AuthenticationPromptCoordinator` / `ThemeController` | GUI | Own authentication prompt deduplication/reauth sequencing and dark-mode/palette/icon refresh state |
| `ComposeTabController` / `ContactsTabController` / `CalendarTabController` | GUI | Own feature-tab workflows and toolbar state; `src/client/main.cpp` constructs them through typed factories against the shell's workspace surfaces |
| `DaemonTrayController` | daemon | Publishes the KDE StatusNotifierItem and D-Bus menu without a Widgets dependency |
| `DesktopNotificationController` | daemon | Publishes desktop notifications and stable GUI activation routes |
| cache repositories | both, split by API | Daemon repositories write; GUI repositories use read-only/query-only connections |

`MessageListSessionFactoryService` is composed only by `GuiServices`: mailbox and search sessions
are GUI-process read models backed by read-only SQLite access and daemon materialization ports. The
daemon does not construct dormant list sessions of its own.

`DaemonServices` is the operational composition root. It is the only place where writable cache
repositories, JMAP transports, synchronization services, history executors, settings, and background
controllers are assembled together. Mail protocol work is intentionally injected as narrow
capabilities, and daemon application policy is likewise split by responsibility: no application
object combines account runtime, queries, content, mutations, notifications, contacts, calendars,
and Sieve. Calendar cache commits are published across the process boundary as `Calendars` cache
invalidations regardless of whether the change came from a GUI command, push synchronization, or
background work; GUI command adapters do not manufacture replacement invalidations from command
completion. Month and Day Agenda views share one materialized event presentation: the month view
owns visible event membership, timing, recurrence, and calendar colour, and an open Day Agenda
filters that same presentation by date. Detail reads may enrich an agenda event with RSVP and editor
state but never decide whether the event exists in the view. `GuiServices` is deliberately smaller:
it exposes
read-only cache readers and remote ports matching the interfaces expected by GUI controllers.
`gui_main` is the GUI composition root: concrete calendar, contacts, and compose dependencies stay
there and are captured by a typed `MainWindowFeatureFactories` set. `MainWindow` receives controller
factories rather than those feature services, avoiding both constructor capability sprawl and a
`GuiServices&` service-locator dependency.

## Representative runtime flows

### Startup and reconnect

```text
systemd or explicit launch
  -> javelind / DaemonProcess
  -> settings migration and cache recovery
  -> DaemonServices and account coordinators
  -> command and activation sockets become ready
  -> javelin / GuiDaemonSession handshake
  -> settings snapshot and cache identity
  -> read-only GUI repositories
  -> workspace restoration
```

The GUI never opens the normal workspace before a coherent daemon handshake. A replaced cache or
schema transition uses the cache suspend/acknowledge/resume barrier so the GUI closes every read
handle before daemon migration or replacement.

### Stateful user command

```text
GUI controller
  -> typed application port
  -> RemoteActionClient and JVIP socket
  -> DaemonRemoteActionDispatcher
  -> command/application service
  -> optimistic projection transaction
  -> command admission with committed cache epoch
  -> GUI reloads projected state
  -> JMAP dispatch
  -> acceptance, rejection, or unknown reconciliation
  -> post-commit invalidation
```

Admission means the daemon accepted responsibility and committed the local operation state. It does
not mean the server has accepted the mutation. A transport failure after dispatch is preserved as an
unknown result rather than guessed or replayed.

### Cache read and materialization

```text
GUI session requests page/content/range
  -> read current SQLite state
  -> request daemon materialization when coverage is missing
  -> WorkScheduler admission
  -> JMAP query/get or vault operation
  -> atomic cache and query-window commit
  -> bounded invalidation
  -> generation-fenced GUI read
  -> precise model update with stable selection restoration
```

IPC does not carry complete mailbox pages, contact documents, or calendar ranges. It requests work
and announces committed changes; the cache remains the data plane.

### Push and notifications

```text
WebSocket push or EventSource state change
  -> AccountSyncCoordinator
  -> typed refresh and reconciliation
  -> cache commit and invalidation
  -> notification discovery outbox
  -> desktop notification publication
  -> optional activation route to GUI
```

Notification arrival does not change GUI selection. Activating a notification is a separate explicit
navigation request containing stable account, mailbox, thread, and Email identities. When the
desktop notification service advertises action support, new-mail Archive and Mark Read actions are
daemon-owned application commands: they enter through `MailCommandPort`, create the same optimistic
Email mutations as GUI actions, and submit only their operation group. Reply remains a presentation
activation: the daemon carries the exact account and Email identity to a reply-compose route and can
launch the GUI when it is not already running.

## Presentation and application coordination

The GUI renders cache-backed models and reports user intent through typed ports. Mailbox visibility
is represented by an opaque observation registered with the daemon-side coordinator. Query-window
materialization, search, content retrieval, downloads, contacts, calendars, Sieve, translation, mutations, bootstrap,
and explicit synchronization all use typed application requests.

`MessageCommandController` converts Qt selections into stable `MessageSelection` values, presents
confirmation and destination UI, and submits commands through `MailCommandPort`. Compose, contacts,
calendar, account refresh, message navigation, content loading, message-list sessions, and Undo/Redo
follow the same pattern: GUI controllers own interaction and presentation lifetime, while daemon
services own application policy and operational execution.

External file drag-out follows the same boundary. Attachment drags request the exact attachment
through `MessageContentPort`, which materializes the raw message source when necessary before
extracting the part. Message drags retain Javelin's private mail-transfer MIME unchanged and lazily
promise a separate `text/uri-list`: only an external target requesting that format materializes the
`.eml` files through the existing selected-message Save and `RawMailMaterializer` path. Drag files
live in a GUI-owned cache staging area rather than `QTemporaryFile` scope: each staging directory is
private to the user, remains available for 24 hours so asynchronous drop consumers do not race source
cleanup, and is pruned by later drag preparation.
The staging files are never treated as user-owned exports or as the source of an internal mailbox
move/copy operation.

The mailbox associated with a list tab is selection context, not proof that every visible Email is
resident in that mailbox: an expanded conversation can expose members from other mailboxes. Move,
copy, delete, and junk planning therefore evaluates each resolved Email's effective cached
`mailboxIds`. Destination menus retain every writable mailbox, including the open mailbox, so a
mixed-residency selection can target a mailbox already containing only part of the selection.
Search selections use the same per-Email rule and never acquire an implied source mailbox.
Ordinary Delete always targets Trash from those real residencies; only the distinct permanent-delete
command may destroy Email objects, regardless of which mailbox tab happens to expose a row.
Cross-account and cross-server Move/Copy operations extend these semantics through the durable workflow in
[CROSS_SERVER_MAIL_TRANSFER_IMPLEMENTATION_PLAN.md](CROSS_SERVER_MAIL_TRANSFER_IMPLEMENTATION_PLAN.md);
same-account membership mutation remains the fast path. Cross-server transfer and
[mail export](MAIL_EXPORT_IMPLEMENTATION_PLAN.md) share daemon-owned exact scope enumeration,
raw-RFC-5322 materialization, and file-backed MailVault leases, but retain separate workflow journals
and destination/failure semantics.

Mailbox and search tabs use application-layer sessions that own query-window reads, request
generations, observation lifetimes, incremental list loading, stale recovery, and prefetch. The GUI
presents those bounded windows as one virtualized infinite-scrolling list; it does not expose page
navigation or turn continuation into ever-growing JMAP query limits. `TabWorkspace` owns tab
identity and shared selection state. Selection restoration, activation, navigation, content ownership,
action availability, and list presentation are separated into deterministic policies plus narrow Qt
adapters so cache changes cannot reinterpret row numbers as user intent.

`AccountSyncCoordinator` owns state-change consumption, debounce and single-flight refresh, state
tokens, cache reconciliation, retries, and post-commit publication for one configured account.
`AccountRuntimeManager` owns coordinator lifetime and configuration, while
`MailQueryApplicationService` owns transient mailbox observation demand. Consumers reload affected
state from SQLite only after the daemon has committed the corresponding transaction.

## Cache materialization and navigation

A synchronization result is not UI state until its typed cache materializer has committed it.
Each JMAP data type owns its own adapter, schema, consistency domain, window semantics, and
optimistic rebase rules. The shared contract is deliberately narrow: capture the domain fence,
materialize confirmed objects and authoritative query membership, rebase active projections, then
publish one typed post-commit cache change. There is no generic cross-type object table and no
untyped JMAP value bag.

The accepted next Email materialization architecture, pending implementation in
[THREAD_MATERIALIZATION_IMPLEMENTATION_PLAN.md](THREAD_MATERIALIZATION_IMPLEMENTATION_PLAN.md), makes
authoritative collapsed-query materialization intentionally narrower than complete conversation
hydration. A complete mailbox or search query window contains the exact ordered `Email/query`
representative ids and enough representative Email objects to render every row in that window.
Thread membership and non-representative child Email objects are separate cache coverage. Their
absence does not make the query window partial or stale.

After a collapsed window commits and becomes renderable, the daemon automatically schedules
bounded background thread materialization for its representatives. That work obtains Thread
membership, persists it, and fetches missing child Email objects in explicit batches no larger than
the server's negotiated object limits. It never uses a result reference that can flatten an
unbounded set of `Thread.emailIds` into one `Email/get`. A user-opened thread may wait for this
already-running materialization, while the global work/progress surface communicates the wait; the
GUI does not create a second thread-loading source of truth or perform network work itself.

Background watched-mailbox refresh uses the canonical received-at-descending collapsed window, so a
synchronized mailbox is immediately loadable from SQLite even while its conversation children are
still being prefetched. Any page or thread fetch that writes server Email objects reapplies active
Email projections before the cache can be rendered. `ContactSyncEngine` materializes AddressBook
and ContactCard snapshots through the contact repositories, while `CalendarSyncEngine` materializes
CalendarEvent objects and bounded occurrence windows through the calendar repositories. Their state
tokens, eviction rules, and optimistic adapters remain independent from mail and from each other.

Starting or restarting an account coordinator schedules an immediate synchronization pass for all
configured mailboxes; a quiet push stream is not proof that their cache already exists. Likewise,
an advertised Email state that is already recorded may suppress redundant object reconciliation
only when every watched mailbox still has authoritative canonical query coverage. A missing or
optimistically invalidated window always requires materialization.

External navigation is an application intent, not a transient widget selection. A notification
activation creates a typed Email route containing stable account, mailbox, thread, and Email ids.
The process-owned coordinator keeps that route alive while the GUI restores, renders any cached
message immediately, and—only when necessary—materializes an anchored authoritative mailbox page.
The route completes after the target has been selected/rendered, or is cancelled by superseding
user navigation. Contact and calendar routes may use the same lifecycle with their own typed route
values; they do not acquire Email pagination semantics.

New-mail notification discovery is owned by account-wide Email reconciliation, never by mailbox
query/window observation. When a committed server Email transition proves that a previously
unconsumed Email legitimately entered an active notification mailbox while unread, the Email
synchronizer creates the per-Email consumption marker and durable notification outbox entry in the
same SQLite transaction as the Email-object and global Email-state transition. Proof is available
for server-created Emails and for updated Emails whose prior object state Javelin retained. JMAP
`Email/changes.updated` contains ids rather than prior objects or changed properties, so an updated
Email that was previously uncached is not fetched solely for notifications and is not guessed to be
newly eligible. If `Email/changes` history is unavailable, a bounded account rebaseline applies the
same previous-to-current notification rule to retained Emails. An entirely unknown Email cannot be
classified safely as created during the lost history range, so notification is conservatively
omitted for that Email. The successful rebaseline nevertheless requires every tracked mailbox query
to reconcile its membership, recovering presentation and offline completeness without giving query
refresh ownership of the account Email cursor. Mailbox identity is retained as deterministic
routing/context metadata;
`(account_id, email_id)` is the notification identity, so later unread movement between enabled
mailboxes cannot create another event.

Combined account Mailbox/Email requests retain independent transition outcomes. A recoverable
`Mailbox/changes` gap schedules Mailbox recovery without discarding a valid `Email/changes`
transition from the same response, and an Email gap does not discard a valid Mailbox transition.

The account's global `Email` sync state is the only Email cursor. Notification storage contains only
the set of mailboxes whose notification baseline has completed; it does not duplicate the Email
state token. Enabling a new mailbox is serialized against in-flight Email work with the existing
Email consistency generation. The mailbox is added to the active notification set only in the final
transaction of a fresh Email reconciliation, so historical cached mail cannot become new mail merely
because notification settings changed. Disabling a mailbox removes it from the active set
immediately. During a pending settings baseline, both incremental and rebaseline Email transitions
may notify
only through the intersection of the already-active and newly desired mailbox sets. Existing active
mailboxes therefore keep notifying while newly enabled mailboxes remain fenced until the baseline
transaction commits. Because ordinary Email state advancement does not mutate this set, local
`Email/set`
confirmation and other Email-state writers cannot desynchronize notification eligibility from the
account cursor. `AccountRuntimeManager` retries durable configuration reads and installation of the
pending baseline fence; once installed, `AccountSyncCoordinator` retries execution of that baseline
through `MailDeltaRefreshExecutor`. Neither retry owner activates a mailbox independently: only the
committed Email-baseline transaction replaces the active set. The delivery service claims pending
outbox rows and revalidates unread state,
current mailbox eligibility, and object existence entirely from SQLite before showing a popup.
Successful delivery removes the outbox row while preserving the per-Email consumption marker; claim,
acknowledgement, release, and desktop-presentation failures are retried locally and do not request
JMAP synchronization solely to recover notification delivery.

Mailbox query windows remain presentation coverage only. Notification routing to a concrete Email
may use its mailbox context to open/materialize the relevant view, but query-window population,
rebuild, pagination, or thread materialization cannot itself create a notification event. Automatic
thread materialization follows as ordinary background work. Child Email commits may affect other
cached mailbox views according to normal Email delta and query-window invalidation rules, but the
initial representative-window commit must not manufacture cross-mailbox invalidations merely because
related children have not yet been fetched. Calendar reminder acknowledgement and snooze state
remains in its separate calendar notification repository.

`JmapMethodTransport` is the request/response boundary for typed JMAP envelopes.
`PreferredJmapMethodTransport` uses the RFC 8887 capability advertised by the cached Session to
keep an authenticated `jmap` WebSocket per owning account, correlate concurrent requests, and send
typed method envelopes over that connection. It falls back to `HttpJmapMethodTransport` only when
the request was not dispatched, so an uncertain disconnect cannot replay a mutation.

The owning-account relationship and advertised WebSocket URL are cached with the Session. WebSocket
failure cooldowns are process-local and shared by method and state-change transports: a failed
endpoint uses HTTP/EventSource for at most 15 minutes, while a newly advertised URL or a new daemon
process is probed immediately. State-change synchronization prefers RFC 8887 push and switches to the
JMAP EventSource endpoint during the same cooldown. Startup performs lightweight Session rediscovery
before restarting account synchronization, while account bootstrap discovers the same capability
during account addition. Session discovery and binary resource transfers remain HTTP operations.

## Message translation

Translation is a GUI-owned presentation subsystem under `src/gui/translation`. It is deliberately
absent from `javelind`, the process protocol, daemon settings, and the main mail database. Closing the
GUI therefore stops language detection, provider requests, model inference, and model memory use
without affecting synchronization or notifications.

`TranslationService` owns the provider setting, target language, per-sender/domain automatic rules,
cache policy, and the process-wide fastText detector. `GoogleTranslationBackend` retains the existing
remote service and is the migrated default. On x86-64 builds, `BergamotTranslationBackend` runs the
pinned native Mozilla/Marian engine on a private one-thread worker and supports direct directions or
English-pivot routes. Local model packs are selected from the committed Firefox manifest, downloaded
from Mozilla's attachment CDN only after an explicit translation or saved automatic rule, verified by
compressed and decompressed size and SHA-256, and installed atomically under the GUI's application
data directory.

Translated strings are cached in the dedicated GUI SQLite database
`CacheLocation/translations/cache-v1.sqlite3`. Cache identity includes provider, canonical source and
target tags, backend/model revision, and input hash; the GUI never writes translation data to the
main mail cache. Generic language offers may restore cached translations or use already-installed
local models, but cannot trigger network traffic. Explicit Translate actions and persisted automatic
rules may fetch from Google or download required local models.

`MessageViewContainer` extracts and reapplies plain-text or HTML chunks and retains a view-generation
token so navigation cannot apply a stale result to another message. Language-offer policy compares
the detected primary language with the configured target rather than assuming English. Preferences
store translation settings directly in the GUI's `translation` QSettings group; this is the sole
presentation-only exception to the normal rule that production GUI code does not access canonical
settings.

## Calendar protocol baseline

Calendar support is implemented against `draft-ietf-jmap-calendars-26` (published
5 November 2025) and RFC 8984 JSCalendar. The exact normative texts are vendored as
`specs/draft-ietf-jmap-calendars-26.txt` and `specs/rfc8984.txt`. Later JMAP Calendars
drafts are deliberately not accepted implicitly: changing the supported draft requires
an explicit protocol review, fixture update, and architecture change.

Calendar protocol envelopes and JSCalendar wire documents remain inside `javelin_jmap`.
`CalendarProtocolClient` and `CalendarMutationEngine` are daemon-only protocol/mutation components;
`CalendarSyncEngine` owns policy-neutral metadata, delta, and bounded range materialization, while
`CalendarApplicationService` is the single daemon coordinator that serializes those synchronization
demands per owning account. Calendar metadata has one authoritative refresh path. Consumers are
notified only after that coordinator has established usable cached metadata; when an active optimistic
Calendar mutation prevents authoritative replacement, the projected cache remains usable but the
coordinator does not mark metadata authoritative or suppress a later reconciliation fetch. Readiness
is retained and replayed when background invitation delivery starts, so an early startup refresh
cannot lose its consumer trigger. `CalendarInvitationService` consumes that cached Calendar metadata
and owns only invitation-specific `CalendarEventNotification` reconciliation,
`ParticipantIdentity` synchronization, and event fetches required to resolve invitations outside the
visible range. Pending invitations retain their own event snapshot for presentation and dispatch;
invitation reconciliation never advances the authoritative `CalendarEvent` state token or overwrites
the shared CalendarEvent cache. `CalendarNotificationService` separately owns reminder delivery and a
bounded daemon reminder horizon. That horizon reuses `CalendarSyncEngine`'s authoritative server-side
recurrence expansion and stores retention membership in `calendar_reminder_occurrences`; it is not a
presentation/query window and does not own another CalendarEvent cursor. Presentation-window eviction
therefore cannot delete an occurrence still needed by the daemon's reminder horizon. Conversely, when
an event changes, occurrence replacement drops its old reminder membership rather than retaining a
possibly stale trigger; the notification service queues authoritative horizon rematerialization from
CalendarEvent state changes and calendar cache commits. For Calendars-capable sessions, push
subscriptions include the draft-26 `CalendarAlert` pseudo-type; these alerts are event notifications,
not collection state, so handling them never advances a JMAP state token. The daemon persists each raw
pushed alert before any follow-up fetch or desktop delivery, then resolves the authoritative event and
shares the same durable notification identity used by local reminder scans. `CalendarAlert` remains the
exact server-driven path outside the bounded local horizon as well as within it. Fetch,
Calendar-metadata, desktop-publication, snooze, and restart recovery therefore cannot depend on a
GUI-visible occurrence window or on the server repeating a push. Dismissing a pushed reminder whose
event is not cached fetches the current authoritative event before submitting the acknowledgement
through the normal calendar mutation path. `CalendarCacheReader` owns cached reads. The GUI consumes typed
calendar values from its read-only cache surface and commands through application ports such as
`CalendarCommandPort`; it
never constructs method names or raw JSON. GUI connection-status changes are presentation state and
do not create calendar synchronization demand; opening or navigating a calendar range does.

## Contacts synchronization

Contacts support follows RFC 9610 and preserves complete JSContact documents at the protocol
boundary. The initial synchronization fetches all AddressBooks and ContactCards. Later explicit
refreshes reconcile the small AddressBook set and advance the cached ContactCard state with
`ContactCard/changes`, fetching only created or updated ids in batches bounded by the server's
`maxObjectsInGet` capability. Every intermediate state is committed before requesting the next
changes page. A `cannotCalculateChanges` response invalidates the delta path and performs an
atomic full ContactCard replacement, as required by RFC 8620.

`ContactProtocolClient` owns contact JMAP calls, `ContactSyncEngine` owns authoritative cache
refresh, `ContactMutationEngine` owns AddressBook/ContactCard/group mutation mechanics, and
`ContactMediaService` owns binary contact media transfer. Application/history policy remains in
`ContactApplicationService` and `ContactCommandService` rather than in those protocol components.
Contact cache commits publish through the process-owned `ContactRepository`. Compose completion,
message sender identity rendering, and the contacts view then reload from SQLite; they do not
retain a second contact data store.

Contact editing projects common JSContact maps into repeatable typed fields while retaining each
map key, label, preference rank, context set, and any unprojected properties in the original
document. Group members are stored as JSContact UIDs, including unresolved UIDs, so temporarily
inaccessible shared contacts are not silently removed. Photos use RFC 9610 blob-backed Media
objects and are fetched on demand rather than retained in the long-lived contact cache.

The application coordination layer evaluates account and AddressBook rights before exposing or
submitting create, update, move, star, merge, copy, and destroy operations. Duplicate discovery is
deliberately high-confidence: normalized email addresses and sufficiently long normalized phone
numbers connect cards of the same kind. A merge keeps the chosen primary UID and name, unions set
properties, preserves colliding mapped entries under new keys, and submits the update and redundant
card destruction together. vCard 4.0 import/export, line unfolding/folding, typed field parameters,
group members, and JSContact document preparation live in the non-GUI contacts layer.
