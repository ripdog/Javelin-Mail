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
[database access](DATABASE_ACCESS.md), [Undo/Redo](UNDO_REDO.md), and
[message rendering](RENDERING.md).

Javelin targets Qt 6.6 or newer, KDE Frameworks 6, and C++23. Glaze provides typed JSON parsing at
the JMAP boundary; QCoro provides coroutine-based Qt networking. KDE Plasma is the primary desktop
integration target rather than merely one supported shell.

## KDE desktop integration

The GUI is intentionally a KDE application built on Qt Widgets, not a generic Qt shell with optional
KDE theming:

- `MainWindow` derives from `KXmlGuiWindow`; menus, toolbar composition, shortcut editing, and saved
  toolbar state use KXMLGUI and KDE standard actions.
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
before the daemon publishes its invalidation.

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

- `javelin_protocol` owns bounded process-boundary values, framing, socket transport, and the test-only
  in-process endpoint.
- `javelin_cache_read` owns database-location discovery and reusable read-only cache, MIME, vault, and
  rendering primitives.
- `javelin_jmap` owns typed JMAP protocol, capability negotiation, transport, cache repositories,
  synchronization primitives, contacts, calendars, submission, Sieve, and optimistic journals. It
  has no Widgets or WebEngine dependency.
- `javelin_daemon_core` owns application coordination, writable repositories, settings, background
  work, notifications, tray integration, deferred send, history, and synchronization lifecycle.
- `javelin_gui` and the `javelin` bootstrap own presentation, read-only repositories, remote port
  adapters, WebEngine, editing state, selection, and navigation.

Configuration fails when production GUI sources access canonical `QSettings`, when GUI targets link
`javelin_jmap` or `javelin_daemon_core`, or when daemon sources acquire Widgets/WebEngine dependencies.

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
| `src/app/` | Composition roots, application commands, settings, background scheduling, notifications, history, and GUI remote adapters |
| `src/jmap/api/` | Session discovery, capabilities, typed JMAP envelopes, HTTP/WebSocket transport, and resource transfer |
| `src/jmap/cache/` | SQLite schema and repositories, read models, query windows, MIME source storage, and search indexes |
| `src/jmap/sync/` | State-change sources, refresh planning, reconciliation, mutation projection, and consistency fences |
| `src/jmap/submission/` | Draft snapshots, attachment manifests, compose revisions, and EmailSubmission workflows |
| `src/jmap/contacts/` | JSContact conversion, synchronization, editing, import/export, and mutation journals |
| `src/jmap/calendar/` | JSCalendar values, recurrence editing, occurrence materialization, and calendar mutations |
| `src/jmap/sieve/` | Sieve domain values, service operations, and optimistic mutation support |
| `src/gui/` | KDE/Qt main window, KXMLGUI actions, tabs, controllers, models, delegates, message rendering, KTextEditor integration, and KConfig preferences |

The principal runtime objects are:

| Component | Process | Role |
| --- | --- | --- |
| `DaemonProcess` | daemon | Starts settings, cache recovery, service composition, sockets, and daemon lifecycle |
| `DaemonServices` | daemon | Owns writable repositories, JMAP transports, coordinators, command services, and background work |
| `DaemonRemoteActionDispatcher` | daemon | Decodes typed remote actions and routes them to application services |
| `CommandDispatcher` | daemon | Admits stateful commands, preserves command identity, and separates rejection from later failure |
| `SettingsRepository` | daemon | Owns the canonical revisioned settings snapshot and migration |
| `WorkScheduler` | daemon | Prioritizes foreground, synchronization, indexing, offline, and maintenance work |
| `DaemonBackgroundController` | daemon | Owns notifications, reminders, delayed-send actions, network recovery, and tray integration |
| `GuiDaemonSession` | GUI | Connects, negotiates protocol/build identity, handles reconnect, and coordinates cache barriers |
| `GuiServices` | GUI | Constructs read-only repositories and typed remote application-port adapters |
| `RemoteActionClient` | GUI | Correlates bounded request/reply actions over the daemon session |
| `MainWindow` and controllers | GUI | Own KDE presentation, KXMLGUI actions, editing, selection, navigation, and user interaction |
| `DaemonTrayController` | daemon | Publishes the KDE StatusNotifierItem and D-Bus menu without a Widgets dependency |
| `DesktopNotificationController` | daemon | Publishes desktop notifications and stable GUI activation routes |
| cache repositories | both, split by API | Daemon repositories write; GUI repositories use read-only/query-only connections |

`DaemonServices` is the operational composition root. It is the only place where writable cache
repositories, JMAP transports, synchronization services, history executors, settings, and background
controllers are assembled together. `GuiServices` is deliberately smaller: it exposes read-only
cache readers and remote ports matching the interfaces expected by GUI controllers.

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
  -> account coordinator / LongPollService
  -> typed refresh and reconciliation
  -> cache commit and invalidation
  -> notification discovery outbox
  -> desktop notification publication
  -> optional activation route to GUI
```

Notification arrival does not change GUI selection. Activating a notification is a separate explicit
navigation request containing stable account, mailbox, thread, and Email identities.

## Presentation and application coordination

The GUI renders cache-backed models and reports user intent through typed ports. Mailbox visibility
is represented by an opaque observation registered with the daemon-side coordinator. Pagination,
search, content retrieval, downloads, contacts, calendars, Sieve, translation, mutations, bootstrap,
and explicit synchronization all use typed application requests.

`MessageCommandController` converts Qt selections into stable `MessageSelection` values, presents
confirmation and destination UI, and submits commands through `MailCommandPort`. Compose, contacts,
calendar, account refresh, message navigation, content loading, message-list sessions, and Undo/Redo
follow the same pattern: GUI controllers own interaction and presentation lifetime, while daemon
services own application policy and operational execution.

Mailbox and search tabs use application-layer sessions that own query-window reads, request
generations, observation lifetimes, pagination, stale recovery, and prefetch. `TabWorkspace` owns tab
identity and shared selection state. Selection restoration, activation, navigation, content ownership,
action availability, and list presentation are separated into deterministic policies plus narrow Qt
adapters so cache changes cannot reinterpret row numbers as user intent.

The account synchronization service owns state-change consumption, debounce and single-flight
refresh, mailbox interest, state tokens, cache reconciliation, retries, and post-commit publication.
Consumers reload affected state from SQLite only after the daemon has committed the corresponding
transaction.

## Cache materialization and navigation

A synchronization result is not UI state until its typed cache materializer has committed it.
Each JMAP data type owns its own adapter, schema, consistency domain, window semantics, and
optimistic rebase rules. The shared contract is deliberately narrow: capture the domain fence,
materialize confirmed objects and authoritative query membership, rebase active projections, then
publish one typed post-commit cache change. There is no generic cross-type object table and no
untyped JMAP value bag.

For Email, an authoritative mailbox materialization includes both the fetched Email/Thread objects
and the exact ordered `Email/query` window. Background watched-mailbox refresh uses the canonical
received-at-descending collapsed window, so a synchronized mailbox is immediately loadable from
SQLite. Any page fetch that writes server Email objects reapplies active Email projections before
the cache can be rendered. Contacts continue to materialize AddressBook and ContactCard snapshots
through their repositories; calendars continue to materialize CalendarEvent objects and bounded
occurrence windows through `CalendarService`. Their state tokens, eviction rules, and optimistic
adapters remain independent.

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

Mail notification discovery writes a persistent pending outbox before publication. Entries become
delivered only after the desktop-notification signal is emitted, making a process failure in that
gap retryable instead of silently losing the notification. Discovery is limited to threads present
in an authoritative mailbox query window; raw Email mailbox membership alone cannot produce a
notification for a message the mailbox view cannot render. Because a collapsed thread fetch may
materialize related Emails from other mailboxes, those other mailbox windows are invalidated in the
same transaction as the Email upsert and must be rematerialized before notification discovery.
Calendar reminder acknowledgement and snooze state remains in its separate calendar notification
repository.

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
The GUI consumes typed calendar domain values and commands through `CalendarService` and
renders committed SQLite state; it never constructs method names or raw JSON.

## Contacts synchronization

Contacts support follows RFC 9610 and preserves complete JSContact documents at the protocol
boundary. The initial synchronization fetches all AddressBooks and ContactCards. Later explicit
refreshes reconcile the small AddressBook set and advance the cached ContactCard state with
`ContactCard/changes`, fetching only created or updated ids in batches bounded by the server's
`maxObjectsInGet` capability. Every intermediate state is committed before requesting the next
changes page. A `cannotCalculateChanges` response invalidates the delta path and performs an
atomic full ContactCard replacement, as required by RFC 8620.

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
