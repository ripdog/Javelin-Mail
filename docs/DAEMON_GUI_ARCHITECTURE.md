# Daemon and GUI Process Architecture

## Status

This document is the authoritative architecture for Javelin's implemented daemon/GUI process split.
The ordered implementation history, current validation evidence, and remaining release work are
maintained in [DAEMON_GUI_IMPLEMENTATION_PLAN.md](DAEMON_GUI_IMPLEMENTATION_PLAN.md).

The design separates Javelin into two long-lived roles:

- `javelind`, a background process that owns JMAP, synchronization, cache writes, mutations,
  Undo/Redo, delayed send, notifications, the tray icon, settings storage, and background work; and
- `javelin`, a single on-demand GUI process that owns windows, presentation models, WebEngine,
  editing state, selection, navigation, and user interaction.

The purpose of the split is to allow the operational side of Javelin, especially push handling,
notifications, delayed send, and the tray icon, to continue while the main GUI and WebEngine are not
running.

The design deliberately does **not** try to turn every small interval of local state into precious,
power-loss-proof data. The server remains the authoritative store for mail, drafts, contacts,
calendars, and other JMAP objects. SQLite is a useful local cache and recovery aid, but it remains
replaceable. Rare loss of very recent local operational state after catastrophic local failure is an
accepted trade-off in favour of a simpler and faster application.

## Final shape

```text
                          private local socket
             commands, replies, settings, activation,
              invalidations, liveness and telemetry
                 ┌──────────────────────────────────┐
                 │                                  │
┌────────────────▼──────────────┐   ┌───────────────▼────────────────┐
│ javelin                       │   │ javelind                       │
│                               │   │                                │
│ Qt Widgets and WebEngine      │   │ JMAP sessions and transports   │
│ active presentation models    │   │ synchronization and pagination │
│ read-only cache queries       │   │ mutation and history execution │
│ GUI editing and navigation    │   │ sole SQLite cache writer       │
│ typed command client          │   │ daemon-owned QSettings          │
│                               │   │ notifications and tray icon    │
└────────────────┬──────────────┘   └───────────────┬────────────────┘
                 │                                  │
                 │ reads                     writes │
                 └──────────────┬───────────────────┘
                                │
                     ┌──────────▼──────────┐
                     │ SQLite and vault    │
                     │ rebuildable cache   │
                     └─────────────────────┘
```

The central rules are:

> The daemon is the sole operational authority and the sole SQLite writer.

> The GUI sends typed commands and renders committed cache state. It never performs JMAP operations
> or mutates authoritative cache tables directly.

> IPC is the command and coordination plane. SQLite is the local data plane. The server remains the
> ultimate source of recoverable application data.

## Goals

The design must:

- continue push synchronization, notifications, tray operation, delayed send, and background work
  without a GUI;
- allow the whole GUI process, including WebEngine, to exit when no window is open;
- preserve the existing optimistic-consistency and authoritative-query-window models during normal
  operation and ordinary process crashes;
- ensure that cache changes never clobber selection, navigation, viewport, focus, or editor intent;
- give the GUI direct, typed acceptance or rejection for user commands;
- keep one operational authority and one cache writer;
- avoid using SQLite as a bidirectional message queue;
- avoid idle database polling;
- keep settings in one daemon-owned store independent of the cache database;
- avoid unnecessary cache writes and excessive SSD churn; and
- keep the architecture simple enough to maintain.

## Non-goals and accepted trade-offs

This design does not:

- replace SQLite with a database server;
- make every local operation survive power failure;
- guarantee retention of Undo history, delayed-send metadata, command deduplication state, or an
  unsynchronized compose revision after the cache database itself is destroyed;
- treat the local cache database as user data requiring backup;
- use the database as a request/reply transport;
- expose raw JMAP JSON, transport details, or C++ objects over IPC;
- support multiple simultaneous GUI processes;
- move tray or notification ownership out of the daemon;
- redesign the notification subsystem beyond changing its process ownership;
- use a separate high-durability operational database; or
- preserve old and new settings or database formats indefinitely.

The practical recovery model is:

- ordinary daemon or GUI crashes should recover cleanly from SQLite transactions and existing
  mutation logic;
- catastrophic cache loss causes Javelin to rebuild from the server;
- very recent local-only state may be lost in that case;
- server ambiguity after loss of the corresponding local journal may require a normal resync and may
  occasionally leave the user to resolve an uncertain send or mutation manually; and
- these rare cases do not justify slowing every normal command or introducing a second essential
  database.

## Architectural invariants

### The daemon is the sole operational authority

The daemon owns:

- account startup, session discovery, capability negotiation, and authentication;
- all JMAP requests and state-change transports;
- synchronization, pagination, query-window materialization, and cache eviction;
- mutation validation, optimistic projection, dispatch, reconciliation, and failure classification;
- Undo/Redo ordering and execution;
- delayed-send scheduling and cancellation;
- notification discovery, publication, and action handling;
- the tray icon and its actions;
- background work scheduling and priority;
- all writes to the main SQLite mail cache and filesystem vault; and
- the canonical shared operational/workspace settings in daemon-owned `QSettings`.

Message translation is the narrow presentation-only exception. The GUI owns its `translation`
`QSettings` group, dedicated translation cache, downloaded model data, provider networking, language
detection, and local inference. None of those surfaces participates in mail synchronization,
mutation ordering, or daemon recovery.

The GUI may ask the daemon to perform an application command. It does not decide how that command is
translated into cache, journal, JMAP, notification, or history operations.

### The GUI is a read-only main-cache client

The GUI opens the main mail SQLite database in read-only mode and uses it to construct active
presentation models. It must not write:

- JMAP cache objects;
- mailbox or search query windows;
- state tokens;
- optimistic projections;
- mutation lifecycle rows;
- history rows;
- delayed-send rows;
- background-job state;
- notification state; or
- shared operational/workspace settings.

The GUI submits shared changes over IPC. The daemon performs any required main-cache or shared
`QSettings` write and returns a typed result. Translation uses a separate GUI-owned SQLite database,
GUI-local settings group, and application-data model directory; those explicit surfaces do not grant
write access to the main cache.

This boundary should be enforced by distinct connection APIs and the build graph, not only by code
review. GUI connections use SQLite read-only mode and `PRAGMA query_only=ON`. Only daemon targets
link write-capable cache repositories and mutation services.

### The server remains authoritative

SQLite contains server-derived objects, query windows, indexes, optimistic projections, and useful
operational recovery state. It is the local source used by the GUI while it exists, but it is not the
only irreplaceable copy of the user's mail, contacts, calendar data, or drafts.

If the cache is missing or corrupt, the daemon may move it aside, create a new cache, and resynchronize
from the server. This reset may discard:

- Undo and Redo history;
- pending local projections;
- recent command-deduplication information;
- delayed-send timing metadata;
- cached notification state;
- incomplete background jobs; and
- local compose recovery state not yet saved to the server.

That loss must be made visible where relevant, but it is not profile destruction. Account settings
and credentials remain outside the cache.

### Cache commits precede invalidations

IPC may notify the GUI that cache state changed, but it never substitutes for the cache transaction.
The ordering is always:

```text
commit SQLite transaction
then publish cache invalidation
```

A cache invalidation may include domain names, account IDs, mailbox IDs, query-window keys, or object
IDs as hints. The GUI re-queries SQLite and never treats the notification itself as the object state.

### Data changes never clobber user intent

The daemon owns facts; the GUI owns presentation intent. Synchronization, projection, rejection,
query-window replacement, notification arrival, or any other cache change must never by itself:

- activate another tab or mailbox;
- select a newly inserted row;
- reinterpret a row number as a different object;
- replace the message, contact, or event currently being viewed;
- move the viewport merely because rows were inserted or removed elsewhere;
- steal focus;
- alter an editor cursor or selection; or
- overwrite newer editor content.

Presentation state uses stable logical identities. Row indexes and server positions are layout
coordinates, not selection identity.

When a model changes, the GUI preserves, as applicable:

- active tab identity;
- selected and current object IDs;
- multi-selection IDs;
- detail-view object ID;
- viewport anchor and offset;
- expansion state;
- focus and editor selection; and
- editor or compose revision.

Precise model changes are preferred over resets. When a reset is unavoidable, these identities are
snapshotted and restored. An object can be replaced only when explicit user action requests it or
when retaining it has become impossible, such as confirmed deletion or account removal.

Notification activation is an explicit user navigation request and is therefore distinct from
notification arrival.

## Process responsibilities

### JMAP and synchronization

The daemon owns:

- Session discovery and capability negotiation;
- credentials and token use;
- HTTP JMAP requests;
- JMAP over WebSocket;
- EventSource state-change connections;
- binary upload and download;
- transport fallback and retry policy;
- Mailbox, Email, Thread, ContactCard, AddressBook, Calendar, and CalendarEvent synchronization;
- watched-mailbox refresh;
- complete-offline mailbox mirroring;
- raw MIME and attachment vault population;
- search-index maintenance; and
- authoritative mailbox and search query windows.

No raw JMAP method name, request JSON, state token, bearer token, or transport URL is part of the GUI
command interface.

### Mutations and optimistic consistency

The daemon retains the existing model from
[OPTIMISTIC_CONSISTENCY.md](OPTIMISTIC_CONSISTENCY.md):

```text
confirmed server state + active mutation projection
```

For a stateful user command, the daemon normally:

1. validates the command against current daemon and cache state;
2. begins the appropriate mutation, operation-group, history, and projection transaction;
3. commits the projected local state;
4. advances the volatile invalidation epoch and replies that the command was accepted, including the
   resulting epoch, changed domains, and bounded affected-key hints;
5. publishes any separately coalesced cache invalidation required for daemon-originated observers;
6. dispatches the JMAP request; and
7. commits acceptance, rejection, or ambiguity through the existing consistency subsystem.

The local acceptance reply means that the daemon accepted responsibility for the command and
committed the normal local operation state. It is not a claim that the server has accepted it, nor a
power-loss guarantee stronger than the configured SQLite durability. For the originating GUI, the
accepted reply is also sufficient to schedule the corresponding cache read; it must not wait for a
second socket message merely to observe its own committed projection.

Known-invalid commands are rejected directly and do not create a projection.

### Undo and Redo

Undo and Redo are typed IPC commands, not local GUI stack operations.

The daemon remains the only owner of history ordering. The GUI renders the daemon-reported current
Undo and Redo labels and includes the observed history head in every request:

```text
UNDO(expected entry ID, expected history revision)
REDO(expected entry ID, expected history revision)
```

The daemon must reject the request unless that exact entry is still the current executable head of
the requested stack. Reasons include:

- another command changed the history branch;
- the entry moved or expired;
- the operation is already executing;
- history is blocked by an unknown or partial result;
- the entry is impossible to undo; or
- the expected history revision is stale.

The rejection is returned directly to the GUI as a typed command reply. The GUI presents it in an
error box and refreshes the history state. It must never silently undo whichever command happens to
be newest at processing time.

Once accepted, execution and any later network failure follow [UNDO_REDO.md](UNDO_REDO.md). A later
failure or conflict is delivered as a typed operation failure and reflected in the cache/history
state.

### Delayed send

Delayed send remains daemon-owned so its timer and notification action continue without the GUI.

The daemon stores enough local state for ordinary restart recovery, but this state is not treated as
irreplaceable profile data. If the cache is destroyed during the delay, the server-side draft
remains authoritative, though the local delayed-send schedule or Undo Send availability may be lost.

A send command references the saved server draft or the compose revision that the daemon has accepted
for saving. The daemon must not intentionally send an older known revision. Elaborate cross-storage
transaction guarantees are not required for catastrophic cache loss.

### Notifications and tray icon

The daemon owns both notifications and the tray icon. This is a core reason for the process split.
They must continue working when the GUI is absent.

The existing notification hardening remains authoritative. This design adds only these process rules:

- notification and tray failures must not stop synchronization;
- notification activation is routed through the daemon;
- activation starts or raises the single GUI process where necessary; and
- tray actions either execute daemon commands directly or activate the GUI.

The preferred implementation for a genuinely headless daemon is direct StatusNotifierItem and menu
integration over QtDBus. Using `QSystemTrayIcon` inside the daemon remains acceptable if its required
application and platform lifecycle are reliable. A third tray process is not part of the intended
architecture.

### Settings

All persisted Javelin settings use daemon-owned `QSettings` as their single source of truth. This
includes daemon behavior, GUI preferences, and persisted GUI workspace state where Javelin chooses to
retain it. The GUI deliberately retrieves the complete typed settings snapshot in bulk and does not
special-case separate storage authorities for operational and presentation settings.

The GUI owns the interactive settings lifecycle:

- constructing preference pages;
- holding unsaved edits;
- validating simple input for usability;
- deciding when Apply or OK is invoked; and
- presenting conflicts or errors.

The daemon owns storage and operational application:

- reading settings at startup;
- sending a settings snapshot to the GUI;
- validating settings that affect daemon behavior;
- atomically applying related settings as one logical update;
- persisting them to `QSettings`;
- updating running services after persistence; and
- reporting success or failure directly.

Settings are not duplicated into SQLite merely so that the GUI can read them. They cross IPC as
typed settings snapshots and updates.

The settings protocol uses a monotonic in-memory or persisted settings revision:

```text
GET_SETTINGS
    -> SETTINGS_SNAPSHOT(revision, typed settings)

UPDATE_SETTINGS(base revision, typed patch or aggregate replacement)
    -> SETTINGS_UPDATED(new revision)
    -> SETTINGS_REJECTED(current revision, reason)
```

A stale Preferences dialog cannot silently overwrite newer settings. The daemon rejects a stale base
revision and the GUI offers reload or manual reconciliation.

Settings that form one decision are changed together. Examples include an account connection record,
notification mailbox selection mode and members, or related compose defaults.

Daemon startup must not require a GUI. It reads all operational settings directly from its own
`QSettings` identity before starting coordinators.

Secrets may continue using the project's existing storage policy. The process split does not require
credentials to be moved into SQLite.

## IPC protocol

### Role

IPC is intentionally richer than a payload-free wake-up channel, but it remains narrow and typed.
It carries:

- command requests and direct replies;
- settings snapshots and changes;
- cache invalidations;
- activation requests;
- current daemon and account status;
- bounded progress or telemetry; and
- operation failures that require GUI presentation.

It does not carry complete cache query pages, displayed message bodies, attachment byte streams,
contact documents, calendar ranges, raw JMAP payloads, or remote service objects. Those remain in
SQLite or the vault and are read by the GUI. A bounded compose save command may carry the complete
current text or HTML body revision because that content originates in the GUI and must cross the
process boundary before the daemon can persist it. Large attachments use explicit staging rather
than ordinary IPC byte payloads.

### Representative messages

```text
GUI -> daemon
    HELLO(protocol version, GUI build identity)
    SUBMIT_COMMAND(command ID, typed command)
    REQUEST_MATERIALIZATION(request ID, scope ID, typed request)
    CANCEL_MATERIALIZATION_SCOPE(scope ID)
    GET_SETTINGS
    UPDATE_SETTINGS(base revision, typed settings update)
    CACHE_ACCESS_SUSPENDED(cache instance ID)
    GUI_READY_FOR_ACTIVATION
    PING

Daemon -> GUI
    READY(protocol version, cache schema version, daemon instance ID, cache instance ID,
          current invalidation epoch, settings revision)
    COMMAND_ACCEPTED(command ID, optional operation ID, cache epoch, changed domains,
                     optional affected keys)
    COMMAND_REJECTED(command ID, typed error)
    OPERATION_FAILED(operation ID, typed error)
    CACHE_CHANGED(epoch, changed domains, optional affected keys)
    CACHE_ACCESS_SUSPEND(cache instance ID, reason, optional target schema version)
    CACHE_ACCESS_RESUME(cache instance ID, cache schema version, current epoch)
    SETTINGS_SNAPSHOT(revision, typed settings)
    SETTINGS_UPDATED(revision)
    SETTINGS_REJECTED(revision, typed error)
    ACTIVATION_REQUESTED(route)
    STATUS_CHANGED(typed transient status)
    PONG
    SHUTTING_DOWN
```

The socket implementation uses a fixed 24-byte big-endian `JVIP` frame header:

- bytes 0-3: magic (`JVIP`);
- bytes 4-5: wire version (`1`);
- bytes 6-7: typed message kind;
- bytes 8-11: reserved flags (currently zero);
- bytes 12-15: payload byte length;
- bytes 16-23: request/reply correlation number.

The payload is encoded with Qt 6.6 `QDataStream` in big-endian mode, but only through the explicit
typed protocol codecs. It contains no Qt object pointers, C++ addresses, raw JMAP JSON, credentials,
or mutable SQLite values. The configured maximum frame size includes the 24-byte header; partial
frames are accumulated, while unsupported wire versions, unknown kinds, invalid enum/variant tags,
trailing payload bytes, and over-limit values are rejected before dispatch.

The connection also has explicit queue and coalescing policy. Command replies, settings replies,
cache-access barriers, and activation requests are lossless and ordered. Cache invalidations merge
compatible domains and bounded affected-key sets. Status and progress messages replace older queued
values for the same subject. Diagnostic telemetry may be dropped. A slow or suspended GUI must never
cause unbounded daemon memory growth, and coalescing must never discard a command result or a required
cache-reopen barrier.

### Command identity and retries

Every user command has a UUID.

For stateful commands, the daemon stores that UUID with the corresponding mutation, history, send,
or operation-group row where practical. This allows normal retry deduplication without maintaining a
separate permanent command inbox.

If the GUI loses the reply after the daemon committed a command, it may retry the same UUID after
reconnection. The daemon returns the existing admission result when it can correlate it.

Not every procedural request requires durable deduplication. Refresh and materialization requests are
safe to reissue. The design accepts that catastrophic loss of the cache can also lose command
correlation.

### Direct rejection versus later failure

A command reply answers whether the daemon accepted the command against its current local state.
Examples of immediate rejection include:

- stale Undo or Redo head;
- unsupported operation;
- missing current object;
- stale settings revision;
- malformed command;
- daemon shutting down; or
- no usable account configuration.

A command may later fail at the server. That failure updates the normal cache and operation state and
may also produce `OPERATION_FAILED` while the GUI is connected. The GUI never treats
`COMMAND_ACCEPTED` as server success.

### Locality

The socket is local to the user's desktop session. It must not listen on a network interface. The
endpoint and client require an existing private runtime directory, require the socket path to remain
inside that directory, and check same-user peer credentials on Linux and macOS when enabled. Both
directions use ordered writes with independently bounded frame and byte queues. Replies and barrier
events remain lossless; invalidations merge bounded domains and keys, while status/settings events
replace older queued values. A queue that cannot retain a lossless value disconnects with a classified
overflow rather than allocating without limit.

## Cache invalidation and refresh

### Volatile invalidation epochs

The daemon maintains an in-memory monotonic invalidation epoch and optional per-domain counters. The
counters need not be written to SQLite.

After a visible cache transaction commits, the daemon increments the appropriate counters and sends a
coalescible `CACHE_CHANGED` message, unless the same committed epoch is already carried by the direct
command reply to the originating GUI. Avoiding persistent generation rows reduces write
amplification.

The invalidation may name broad domains such as:

- mailbox tree;
- mail query windows;
- message metadata;
- message content;
- contacts;
- calendars;
- history;
- background jobs; or
- user-visible failures.

It may also include bounded affected keys, such as an account, mailbox, query-window key, or Email ID.
These are optimization hints. SQLite remains the source queried for the resulting rows.

### Startup race avoidance

The GUI subscribes before loading the database:

1. connect and complete the handshake;
2. record the daemon instance ID, cache instance ID, and current invalidation epoch;
3. establish its small fixed set of thread-owned read-only SQLite connections;
4. load the initial visible models using short fresh read snapshots;
5. process any invalidations queued since the handshake; and
6. repeat affected reads if the epoch advanced while loading.

A commit after the handshake either appears in the GUI's read snapshot or produces a later
invalidation. The GUI does not need persistent generation rows to close this race.

### Daemon startup and version compatibility

A usable GUI requires a responsive compatible daemon. The GUI does not enter a normal half-working
mode merely because old cache rows are readable without daemon coordination.

At startup, the GUI connects to the configured local endpoint and completes the version handshake and
bulk settings snapshot before constructing its operational workspace. If the daemon is absent,
unresponsive, or protocol-incompatible, the bootstrap path attempts the configured safe start or
restart mechanism. A version-mismatch restart is graceful: the daemon first stops admitting commands,
finishes or safely classifies active transactions and dispatched operations, closes the cache, and
only then exits. The GUI never kills a daemon during a cache transaction merely to accelerate startup.

If recovery fails, the GUI shows one blocking recovery surface with the concrete failure and retry or
exit actions. It does not expose mailbox commands, editors, or settings backed by an unknown daemon
state. Notification activation follows the same bootstrap path and preserves its requested route
until the replacement GUI reports readiness.

### Reconnection

On any socket disconnect, notification continuity is lost. An already-running GUI may keep its last
rendered state visible while it performs daemon recovery, but this is a recovery surface rather than
a supported interactive read-only operating mode. Daemon-dependent commands, navigation requiring
materialization, and settings changes remain unavailable until the handshake completes.

After reconnection:

- if the cache instance or schema changed, use the cache-access barrier before reopening connections;
- if the daemon instance changed, refresh active views whose SQLite `data_version` or relevant cache
  epoch may have advanced;
- if both the cache data version and relevant epoch are unchanged, retain the existing model and
  refresh only settings and daemon status; and
- always reload the canonical bulk settings snapshot.

Because only one GUI process exists, the reconnect path does not need durable per-client invalidation
history.

### Cache migration and replacement barrier

The daemon must not migrate, replace, move aside, or reset the cache while GUI connections or read
workers may still refer to the old database file. File replacement without coordination can leave the
GUI reading an old inode while the daemon writes a new cache at the same path.

The barrier is:

1. the daemon stops admitting cache-dependent commands and cancels or completes active cache work;
2. it sends `CACHE_ACCESS_SUSPEND` and stops publishing ordinary cache invalidations;
3. the GUI cancels outstanding reads, drains their results without installing them, closes every
   SQLite and search-index connection, and acknowledges `CACHE_ACCESS_SUSPENDED`;
4. the daemon completes the migration, reset, or file replacement transactionally where applicable;
5. it opens and validates the resulting cache, assigns a new cache instance ID when the underlying
   file was replaced, and sends `CACHE_ACCESS_RESUME`; and
6. the GUI recreates its read connections and reloads active presentation from the new cache.

A socket disconnect alone does not release the GUI process's database handles. If the GUI is
unresponsive, the daemon must either defer non-essential migration or request termination of the
verified peer GUI process and wait until its handles are actually released before proceeding. Cache
recovery must not trade correctness for a shorter startup delay.

### Read behaviour

GUI read transactions are short-lived and never cross an event-loop suspension. The GUI uses one
dedicated database worker or a very small fixed pool of persistent thread-owned read-only
connections. Each operation opens a fresh SQLite read snapshot on its worker connection; it does not
open and migrate a new connection for every invalidation.

Every presenter has a monotonically increasing refresh generation and a current scope identity. A
read result carries the generation, scope, and daemon epoch that caused it. The GUI discards the
result without touching the model if a newer refresh was scheduled, the view scope changed, cache
access was suspended, or a newer result was already installed. An older slow query must never roll a
model back over newer committed state.

A presenter:

1. captures the stable presentation identities it must preserve and advances its refresh generation;
2. reads the required query windows and object rows in a short worker snapshot;
3. closes every query and transaction;
4. computes the bounded model patch or replacement values off the GUI thread;
5. verifies the generation and scope again on the GUI thread;
6. applies only the required Qt model operations while selection-driven navigation callbacks are
   suppressed; and
7. restores selection, current object, viewport anchor and offset, focus, and editing state before
   publishing one coherent presentation-state change.

A model reset is a last resort. If the selected object was confirmed deleted, the GUI presents an
explicit unavailable or deleted state rather than silently selecting whichever object inherited its
former row number.

## Materialization requests

Not every missing cache item is a durable application command.

Requests such as these are transient IPC requests:

- ensure a mailbox window;
- ensure a search window;
- ensure message content;
- ensure an attachment is cached;
- ensure a contact detail record;
- ensure a calendar range; or
- prefetch the next page.

They carry a request ID and a GUI scope ID. A later request in the same scope may supersede an older
one, and closing the view may cancel undispatched work. Once network work is dispatched, completing
it and caching the result is harmless even if the original GUI scope disappeared.

Results are committed to SQLite or the vault and announced by cache invalidation. They are not
returned as object payloads over IPC.

This avoids filling SQLite with transient GUI requests and prevents rapid navigation from creating a
large durable command backlog.

Vault-backed content has an explicit lifetime contract. Once the GUI has resolved an attachment,
raw source, or rendered artifact for active use, it obtains either an open handle or a bounded daemon
lease that prevents eviction until the GUI releases it or disconnects. A database path lookup alone
must not race daemon eviction or replacement.

## Resource and scheduling policy

The daemon exists partly to release the expensive GUI and WebEngine process, so it must not recreate
GUI-scale working sets in the background.

The daemon keeps only bounded operational state in memory. In particular:

- disconnecting the GUI cancels undispatched materialization and prefetch scopes;
- decoded bodies, presentation models, rendered documents, and editor state are never retained by the
  daemon merely for faster future GUI startup;
- search sessions, sparse query windows, content leases, and completed operation records have explicit
  retention limits;
- per-account network concurrency and queued work are bounded;
- foreground user commands and visible materialization outrank prefetch, indexing, complete-offline
  synchronization, and maintenance; and
- lower-priority work yields at natural correctness-preserving batch boundaries.

Priority changes only admission order. The daemon never interrupts an active SQLite transaction or
abandons a partially applied consistency transition. A large operation must prepare expensive work
outside the transaction and divide logically independent commits into bounded batches; it must not
split one atomic mutation, projection, reconciliation, or state-token transition merely to improve
latency.

When no effective server, settings, timer, or job state changes, an idle daemon should produce no
SQLite writes, negligible CPU wake-ups, and bounded steady-state memory. These properties are measured
with the GUI disconnected as part of implementation acceptance.

## Infinite scrolling and sparse windows

[QUERY_WINDOWS.md](QUERY_WINDOWS.md) remains authoritative.

An uncached online-first mailbox is represented by bounded server query windows, not by applying SQL
offsets to whatever Email rows happen to be cached. The GUI presents the committed windows as one
virtualized list and has no page-number, previous-page, next-page, first-page, or last-page state.

Continuation works as follows:

```text
viewport approaches the end of the loaded prefix
    -> GUI keeps the already-installed rows and marks continuation in flight
    -> GUI requests one bounded next window, anchored after the final loaded Email
    -> daemon performs Email/query and required Email/get materialization
    -> daemon commits the complete window
    -> daemon sends CACHE_CHANGED for that window
    -> GUI appends its rows if the same list session still wants that continuation
```

A result arriving after the user changed mailbox, search, or sort may remain useful cached data but
cannot activate itself. Request generations and stable query identity prevent an obsolete completion
from replacing or extending the new list.

Initial refresh and continuation have separate presentation state. Refresh may retain useful existing
rows while the first authoritative replacement window is pending. Continuation leaves the viewport
unchanged and appends only after cache commit. A continuation error preserves all rows already loaded
and exposes an explicit retry affordance at the end of the list.

The GUI never implements infinite scrolling by repeatedly querying limits of 100, 200, 300, and so
on. Each request remains bounded. For mailboxes the next request uses the final representative as a
stable anchor; online search may consume one compatible prefetched window and otherwise falls back to
an anchored bounded request. If an anchored response advances `queryState`, the already-visible
prefix is retained only as a continuity snapshot and the combined presentation remains stale rather
than claiming one authoritative query generation.

During recovery from a daemon disconnect, an already-running GUI may keep installed cached windows
visible but cannot fabricate an uncached continuation range from partial object membership. It does
not present that snapshot as a fully operational daemon-free mode.

## Compose behaviour

The visible editor buffer belongs to the GUI. Draft persistence belongs to the normal JMAP draft
workflow owned by the daemon.

Each compose session has an increasing revision. A debounced save command carries the complete
bounded compose revision needed by the daemon, including envelope fields and the current text or HTML
body. The protocol does not require per-keystroke deltas. The daemon rejects an older revision that
arrives after a newer accepted revision, and a Send command names the exact accepted revision the user
intended to send.

Admission of a compose save means that the daemon has taken ownership of that revision for the normal
save workflow. Admission of Send requires that the named revision and attachment manifest are already
accepted; the daemon must not intentionally send an older body, omit a newly accepted attachment, or
substitute later GUI state.

Attachments are not streamed as ordinary framed IPC byte arrays. Selection creates a typed staging
request using a path, platform file handle, or equivalent local transfer token. The daemon copies the
bytes into its own immutable staging area or completes upload before acknowledging the attachment as
part of an accepted compose revision. It records enough identity, size, and content metadata to avoid
sending a different file if the original path is replaced or modified later. Failed or cancelled
staging never silently leaves a phantom attachment in the accepted manifest.

The system should be robust under ordinary process crashes, but it does not require a separate
power-loss-proof compose journal. If the GUI or cache is catastrophically lost before the newest
revision reaches the server, those recent edits may be lost. That is the same practical risk as an
editor autosave interval and is an accepted trade-off.

Before normal GUI shutdown, the latest revision should at least have been accepted by the daemon for
saving. Whether the GUI waits for server confirmation is a product and latency decision, not a
process-boundary invariant. Shutdown must not discard a revision merely because an older save is still
in flight; revision ordering and supersession are resolved by the daemon.

## Settings lifecycle

### Storage

The daemon initializes a stable organization and application identity and owns the canonical
`QSettings` instance. Settings remain intact when SQLite is removed or rebuilt.

The GUI does not directly open the canonical settings store. It obtains a typed snapshot over IPC and
submits typed updates.

This avoids:

- split settings authority;
- daemon and GUI observing different defaults;
- schema migration being tied to cache migration;
- account configuration being lost during cache repair; and
- the cache becoming essential merely because it contains preferences.

### Migration

Legacy settings migration is performed once by the daemon before it starts account coordinators or
reports readiness.

The migration should:

1. initialize the exact legacy `QSettings` identity;
2. read and normalize the complete current settings shape;
3. write the new daemon-owned shape;
4. verify it can be read back;
5. mark the settings schema version; and
6. remove obsolete keys only after successful verification.

There is no fallback reader after migration. An ambiguous migration fails visibly rather than
starting with empty accounts or invented defaults.

Cache schema migration is separate. Failure to migrate the cache may cause the cache to be moved
aside and rebuilt without deleting settings.

## Database concurrency

SQLite remains in WAL mode.

The daemon is the only writer, so all write scheduling remains in-process. A priority-aware daemon
write admission queue orders work before it asks SQLite for the writer lock. Foreground command
admission, Undo/Redo, compose saves required by Send, and visible materialization outrank prefetch,
indexing, complete-offline synchronization, and maintenance. The existing write coordinator remains
the final serialization mechanism once work is admitted; it is not by itself the scheduling policy.

An active transaction is never pre-empted. Background operations prepare network results, parsing,
index input, and filesystem work before admission and commit logically independent units in bounded
transactions. Correctness-critical changes that must be atomic remain one transaction even when they
are larger than the preferred latency budget. Scheduler priority may reduce queueing latency but may
never expose a half-projected mutation, mismatched query window, advanced state token without its
objects, or partially reconciled operation.

The GUI uses independent read-only connections opened through a dedicated read-only factory. GUI
connections use SQLite read-only mode and `PRAGMA query_only=ON`, never run migrations, and use a short
contention timeout. Read queries and transactions remain short so WAL checkpoints are not pinned
indefinitely. Transient contention retains the already-rendered model and schedules a bounded retry;
it does not blank the view or present a permanent storage failure.

No transaction may span:

- network I/O;
- filesystem I/O;
- parsing;
- coroutine suspension;
- signal delivery;
- GUI work; or
- notification publication.

A daemon cache write that fails because of temporary contention is retried according to the owning
operation without reordering dependent consistency transitions. A cache corruption or persistent
write failure can trigger a controlled cache reset only through the cache-access barrier after active
operations are stopped or safely classified.

## SSD write policy

The daemon is long-lived, so write amplification must be treated as a design concern.

### Do not persist transient activity

Do not write SQLite rows for:

- push pings or connection liveness;
- every progress percentage;
- every scheduler wake-up;
- volatile connection status;
- GUI materialization requests;
- IPC acknowledgement bookkeeping that already exists naturally in an operation row;
- generation counters used only to wake the current GUI; or
- repeated observations that do not change effective state.

These belong in memory and may be sent as transient IPC telemetry.

### Coalesce cache work

Where correctness permits:

- combine fetched object updates into one transaction;
- advance several related cache tables together;
- coalesce repeated push changes before requesting or committing a refresh;
- debounce derived search-index updates;
- avoid rewriting unchanged rows;
- store compact normalized relations rather than repeatedly replacing large JSON documents;
- checkpoint WAL based on size and idle opportunity rather than after every operation; and
- avoid durable high-frequency timers.

### Durability level

The cache may continue using SQLite WAL with `synchronous=NORMAL`. It should not switch the whole
cache to `FULL` merely to protect rare local-only intervals.

User-initiated operations are infrequent compared with synchronization and do not materially affect
SSD life. Small mutation, history, or deduplication rows may continue to be written when they simplify
ordinary crash recovery. They are not grounds for a separate essential database.

### Vault writes

Raw MIME, attachments, and other large immutable data remain in the filesystem vault rather than
SQLite BLOBs. Content-addressed storage and atomic installation prevent repeated writes of identical
objects where practical.

## Failure semantics

### GUI exits normally

The GUI submits any required final compose save and workspace settings updates, closes its read
connections, and exits. The daemon, tray, notifications, transports, and background work continue.

### GUI crashes

Commands already accepted by the daemon continue. Unsaved editor changes since the latest accepted
save may be lost. A replacement GUI reconstructs views from SQLite and settings from daemon IPC.

### Daemon exits normally

The daemon stops admitting new commands, announces shutdown where possible, closes transports, and
leaves the cache transactionally consistent. If the GUI remains open, it retains its rendered state
only as part of the daemon restart or failure surface; it does not continue as a nominally supported
cached read-only application.

### Daemon crashes

SQLite atomicity prevents partial transactions. Existing recovery converts abandoned in-flight
mutations to unknown and recovers history and delayed-send state where available.

The GUI detects socket loss and does not infer that an accepted server operation failed merely because
the daemon disappeared.

### Lost command reply

The GUI retries a stateful command using the same UUID only after reconnecting. The daemon deduplicates
through the associated operation state where possible. If it cannot establish whether the command was
accepted, it reports uncertainty rather than blindly generating a different command ID.

### Cache loss or corruption

The daemon stops admitting cache-dependent commands, invokes the cache-access barrier, safely
classifies active operations, moves the damaged cache aside where useful for diagnostics, creates and
validates a new database, and resynchronizes configured accounts using daemon-owned settings. It does
not replace the cache while GUI readers or daemon transactions still refer to the old file.

Undo history, local projections, delayed-send metadata, and similar incidental state may be lost.
The GUI tells the user about any meaningful consequence that can be detected. Javelin does not treat
cache loss as loss of account configuration.

### D-Bus or desktop shell failure

Synchronization continues. The notification subsystem follows its existing retry and recovery policy.
Tray integration is re-established when the desktop service returns. These failures do not restart or
invalidate JMAP coordinators.

### System suspend and resume

The daemon owns transport recovery, delayed-send timer reevaluation, notification scheduling, and
push-session restart. Delayed-send deadlines are persisted absolute UTC instants rather than durations
that restart after resume; while running, monotonic timers may be used to avoid ordinary wall-clock
adjustments, with the absolute deadline reevaluated after resume or restart. If a deadline elapsed
during suspend and the send has not already been
dispatched or cancelled, the daemon queues that exact accepted compose revision once as soon as the
required account and network state are usable. Suspend does not silently grant a new Undo Send window,
and resume recovery must deduplicate against a send dispatched immediately before suspension.

The GUI merely receives later status and cache invalidations.

## Single GUI instance and activation

Only one GUI process is supported.

When a second `javelin` invocation occurs, it normally connects to the daemon and requests activation
of the existing GUI. The GUI also owns a minimal activation-only local socket as a fallback while the
daemon is unavailable. That fallback carries presentation intents such as a desktop `mailto:` launch;
it does not expose application commands or cache state. A `mailto:` intent received during daemon
recovery remains queued in the GUI, the GUI starts the daemon when necessary, and a new compose tab is
opened only after the daemon session is ready. If no GUI exists, the new process becomes the GUI
instance and follows the same pending-intent startup path.

The daemon tracks one active GUI connection and one activation route queue during normal operation.
This removes the need for:

- per-GUI route claiming;
- multi-editor ownership rules;
- competing Undo menu state;
- concurrent GUI settings updates; and
- multiple workspace persistence namespaces.

Multiple windows inside the one GUI process remain a presentation choice.

## Build and dependency boundaries

The process split must be enforced by CMake targets.

A suitable target shape is:

```text
javelin_protocol
    typed IPC messages and framing

javelin_cache_read
    read-only query repositories and presentation value types

javelin_daemon_core
    application coordination, JMAP, synchronization, cache writes,
    mutation journal, history, settings and background work

javelind
    daemon bootstrap, socket server, notifications and tray integration

javelin_gui
    Widgets/WebEngine presentation and command client

javelin
    GUI bootstrap
```

`javelind` must not link WebEngine or construct any main-window widget. Direct QtDBus tray integration
allows it to remain a `QCoreApplication`; using `QSystemTrayIcon` may require a GUI application type
but still must not pull in the main GUI or WebEngine.

GUI controllers must stop depending directly on concrete daemon services such as
`MailMutationApplicationService`, `ComposeService`, `CalendarMutationEngine`, `UndoManager`, or
`WorkScheduler`.
They consume:

- read-only repositories;
- typed command and settings clients;
- cache invalidations; and
- transient daemon status interfaces.

## Implementation sequence

### 1. Define typed process-boundary interfaces in-process

Create:

- `ApplicationCommand` as a closed typed variant;
- `CommandReply` and typed application errors;
- `MaterializationRequest`;
- `CacheInvalidation`;
- cache-access suspend and resume barriers;
- bounded compose revision and attachment-staging commands;
- typed settings snapshots and updates; and
- transient daemon status values with explicit queue and coalescing policy.

Route existing GUI commands through an in-process implementation first. Do not start with socket
serialization.

### 2. Make the daemon side the sole cache writer

Introduce explicit daemon write and GUI read-only database factories. Move all mutating repositories
and cache transactions behind the daemon-side application interfaces. Add priority-aware admission in
front of the existing writer serialization and define bounded background transaction units without
splitting correctness-critical atomic changes.

Remove direct GUI writes and direct GUI mutation-service calls.

### 3. Separate daemon-safe and GUI-only targets

Move WebEngine scheme handling, the complete translation subsystem, navigation state, and
widget-oriented code out of operational service targets. Ensure the future daemon core links only the
dependencies it actually requires.

### 4. Move settings authority to daemon `QSettings`

Define the typed shared-settings schema, revision rules, migration, snapshot protocol, and update
protocol. Remove SQLite settings plans and stop GUI code from opening the canonical shared settings
store directly. Permit only `TranslationSettingsStore` to open the separate GUI-local translation
group.

### 5. Add volatile invalidation and transient telemetry interfaces

Replace direct service-to-widget signals with cache invalidations and status values. Add presenter
refresh generations, off-thread read and diff preparation, guarded model installation, stale-result
discard, and bounded IPC coalescing. Keep progress and connection health out of SQLite unless they are
meaningful durable job checkpoints.

### 6. Introduce the local socket

Serialize the already-tested typed interfaces over a framed, versioned, queue-bounded local protocol.
Preserve the same in-process and out-of-process semantics, including command-result ordering,
cache-access barriers, compose frame limits, and graceful protocol-version recovery.

### 7. Split the executables

Create `javelind` and make `javelin` a GUI-only process. Add daemon startup, graceful restart,
reconnection, singleton GUI activation, process-version checks, blocking startup failure UX, and
notification-route preservation across bootstrap.

### 8. Move tray and notification ownership

Run the existing hardened notification logic in the daemon. Implement tray ownership in the daemon,
preferably through direct StatusNotifierItem QtDBus integration if that keeps the process simpler and
headless.

### 9. Tune cache writes and daemon resource use

Measure WAL growth, write rates, foreground admission latency, CPU wake-ups, and daemon memory during:

- idle push operation with no GUI;
- a large mailbox refresh;
- complete-offline synchronization;
- search indexing;
- rapid pagination and navigation cancellation;
- notification-heavy mail arrival; and
- GUI disconnect and later restart.

Remove unchanged writes, coalesce transactions, bound retained working sets, and tune checkpoint and
batch policy based on measurements rather than adding a second database pre-emptively.

## Required tests

The split requires deterministic tests for:

- command acceptance carrying the committed cache epoch and direct rejection;
- stale Undo and Redo head rejection;
- lost command reply and same-UUID retry;
- command accepted before GUI exit;
- daemon crash before and after mutation dispatch;
- foreground command admission behind queued background work without interrupting an active
  transaction;
- startup read versus concurrent invalidation;
- two overlapping presenter reads completing out of order, with the stale result discarded;
- model replacement with selection callbacks suppressed until stable IDs are restored;
- reconnect with the same and a different daemon instance and with unchanged versus changed SQLite
  `data_version`;
- graceful recovery from a protocol-incompatible daemon;
- cache migration and cache-file replacement while a GUI is connected and actively reading;
- an unresponsive GUI during a required cache-access barrier;
- settings bulk snapshot, update, stale-revision rejection, and migration failure;
- one-GUI activation routing and notification-route preservation during daemon startup;
- bounded socket output when the GUI stops reading;
- transient materialization cancellation and supersession;
- vault eviction blocked by an active content handle or lease;
- a deep sparse-window jump whose result arrives after navigation elsewhere;
- retained rows remaining labelled as the installed page while another page is pending;
- compose revisions arriving out of order;
- Send rejection for an unaccepted body revision or incomplete attachment manifest;
- attachment source replacement after successful staging;
- suspend across a delayed-send deadline without duplicate send or renewed cancellation window;
- selection and viewport preservation across cache changes;
- tray and notification failure without sync failure;
- cache deletion followed by account resynchronization from daemon settings; and
- bounded idle write activity, CPU wake-ups, memory, and retained work after GUI disconnect.

Tests around optimistic mutations, query windows, Undo/Redo, delayed send, and notification handling
remain governed by their existing subsystem documents.

## Performance and UX acceptance criteria

The first out-of-process implementation is not complete merely because it is functionally correct. It
must also demonstrate that the process split does not introduce avoidable interaction latency or
presentation instability:

- no SQLite query, model diff, body processing, or unbounded collection operation runs on the GUI
  thread;
- no stale worker result can replace a newer installed presentation generation;
- command admission cannot wait behind more than the currently active correctness-critical
  transaction plus a bounded foreground scheduling interval;
- background work is divided at safe atomic boundaries so ordinary user commands remain responsive;
- cache-backed startup and visible cached navigation do not wait for account network availability
  after the daemon handshake succeeds;
- daemon absence, hangs, version mismatch, and cache replacement produce one coherent recovery flow,
  not a partially enabled application;
- IPC queues, retained materialization scopes, search sessions, and content leases have explicit
  memory bounds;
- an idle daemon with unchanged effective state performs no SQLite writes and negligible periodic
  work;
- closing the GUI releases WebEngine and presentation-heavy memory, and daemon steady-state memory is
  measured with representative accounts; and
- every model update preserves stable user intent or reports explicitly that the referenced object no
  longer exists.

These criteria never override transactionality, mutation causality, exact state-token advancement,
compose revision identity, or operation deduplication. When a latency target conflicts with a required
atomic transition, correctness wins and the transaction is measured and optimized rather than split
incorrectly.

## Design constraints on existing subsystems

The process split preserves these contracts:

- [ARCHITECTURE.md](ARCHITECTURE.md) governs responsibility layering;
- [DATABASE_ACCESS.md](DATABASE_ACCESS.md) governs connection and transaction lifetime;
- [OPTIMISTIC_CONSISTENCY.md](OPTIMISTIC_CONSISTENCY.md) governs mutation and refresh causality;
- [QUERY_WINDOWS.md](QUERY_WINDOWS.md) governs ordered pagination;
- [OFFLINE_MAIL_ARCHITECTURE.md](OFFLINE_MAIL_ARCHITECTURE.md) governs offline mirrors and vault
  policy; and
- [UNDO_REDO.md](UNDO_REDO.md) governs history execution and delayed send.

The additional process rule is:

> Work that must continue after the GUI exits belongs to daemon-owned services. GUI interaction uses
> typed IPC, visible state comes from read-only cache queries, and settings come from daemon-owned
> `QSettings`.

## Daemon packaging and startup

The Arch package installs both executables and a templated `javelind.service` user unit. The package
does not enable the unit: login-start behavior is a per-user choice and must not be changed by a
package transaction. A GUI launched from the configured system binary directory offers to enable and
start the unit when the user service manager reports that the unit is loaded. It also offers a
one-shot direct daemon start. Both paths wait for the private socket, complete the normal handshake,
load the settings snapshot and cache read connection, and only then leave the recovery surface.

The GUI retains the direct launch as a fallback for development builds, non-systemd sessions, and
installations without the packaged unit. The daemon translates service-manager termination signals
into its normal graceful shutdown path. The unit is tied to `graphical-session.target`; user lingering
is not enabled, because the daemon owns desktop-session tray and notification integration.

The architecture does not depend on immediately deciding:

- whether tray integration uses direct StatusNotifierItem QtDBus or `QSystemTrayIcon`;
- the exact framed IPC encoding;
- how much affected-key detail is included in cache invalidations;
- the exact cache reset user experience; or
- how aggressively obsolete cache windows and incidental history are retained.

These choices must not change the settled shape:

- one GUI process;
- one daemon operational authority;
- daemon-owned tray and notifications;
- daemon-owned `QSettings`;
- daemon-only SQLite writes;
- typed bidirectional IPC for commands and replies;
- read-only SQLite data access from the GUI;
- rebuildable, non-essential cache storage; and
- preservation of stable user intent across every cache refresh.
