# Daemon and GUI Process Architecture

## Status

This document describes a prospective process architecture for Javelin. It is a design for further
review and iteration, not an implementation plan or a statement that the split has already been
made.

The design separates Javelin into two long-lived roles:

- a small background daemon that owns coordination, JMAP, synchronization, mutation execution,
  notifications, the tray icon, and all authoritative cache maintenance; and
- an on-demand GUI process that renders SQLite-backed state, records durable user intent, and may
  exit completely whenever no window is needed.

The purpose of the split is not merely to reduce memory use. It creates a process boundary around
Javelin's existing correctness model: SQLite remains the local source of truth, while network and
mutation authority continue independently of widget and WebEngine lifetimes.

## Design summary

The architecture is:

```text
                         private local socket
                    liveness, wake-ups, generations
                 ┌──────────────────────────────────┐
                 │                                  │
┌────────────────▼──────────────┐   ┌───────────────▼────────────────┐
│ javelin                       │   │ javelind                       │
│                               │   │                                │
│ Qt Widgets and WebEngine      │   │ JMAP sessions and transports   │
│ active presentation models    │   │ synchronization and pagination │
│ read-only cache queries       │   │ mutation journal and history   │
│ durable intent insertion      │   │ SQLite cache writer            │
│ GUI workspace persistence     │   │ notification and tray services │
│                               │   │ background work scheduling     │
└────────────────┬──────────────┘   └───────────────┬────────────────┘
                 │                                  │
                 │ reads                     writes │
                 │ narrow intent inserts            │
                 └──────────────┬───────────────────┘
                                │
                     ┌──────────▼──────────┐
                     │ SQLite and vault    │
                     │ durable truth       │
                     └─────────────────────┘
```

The essential rule is:

> The GUI expresses durable user intent and renders durable database state. It never owns the
> execution, reconciliation, or lifetime of an application operation.

SQLite carries all application data. The local socket carries no mail, contacts, calendar objects,
mutation results, page rows, or command payloads. It exists only to establish process readiness,
wake the other process after a durable write, and tell a GUI that a newer committed database
generation exists.

The daemon may send a notification immediately after a relevant SQLite commit. The GUI then opens a
fresh SQLite snapshot and performs the same cache queries it would perform after startup. A missed
or coalesced socket notification cannot lose state because the state and its generation are already
durable.

## Goals

The design must preserve or strengthen Javelin's primary correctness properties:

- retrieve and display the correct mail as quickly as the available cache and network permit;
- maintain exact authoritative query windows and never infer ordered membership from object rows;
- keep confirmed server state, optimistic projections, mutation lifecycle, state tokens, and
  consistency generations atomic;
- treat ambiguous transport outcomes as unknown rather than success or failure;
- recover queued and in-flight work after process failure without silently duplicating mutations;
- continue synchronization, delayed send, notification discovery, and notification delivery while
  no GUI exists;
- keep notification activation routable even when it must start a new GUI process;
- preserve the process-wide Undo and Redo history independently of any window;
- allow the entire GUI, including WebEngine, rendered messages, inactive widgets, and transient view
  models, to leave memory when no window is open;
- avoid a second operation model, cache, or state machine in the GUI;
- avoid idle database polling; and
- keep the local database self-contained, fast, inspectable, and recoverable without introducing a
  separate database server.

## Non-goals

This design does not:

- replace SQLite with a client/server database;
- make IPC an application data API;
- remotely expose C++ objects, Qt models, JMAP protocol structures, or service interfaces;
- move optimistic mutation policy into the GUI;
- let the GUI perform JMAP calls as a fallback when the daemon is unavailable;
- make desktop notification or tray integration part of synchronization correctness;
- require every GUI-only preference, rendered artifact, or workspace detail to live in the shared
  core database;
- require the daemon to link WebEngine or construct any main-window widgets; or
- define a migration sequence from the current single-process architecture.

## Architectural invariants

The following invariants define the process boundary.

### SQLite is the authoritative data plane

All substantive state exchanged between the two processes is committed to SQLite or the filesystem
vault referenced by SQLite. The GUI never treats a socket message as proof of a mail, contact,
calendar, journal, history, or compose state.

The effective state rendered by the GUI remains:

```text
confirmed server state + active mutation projection
```

The daemon materializes that effective state according to
[OPTIMISTIC_CONSISTENCY.md](OPTIMISTIC_CONSISTENCY.md). The process split does not create another
optimistic layer.

### The daemon is the sole application authority

The daemon owns:

- interpretation and execution of durable application commands;
- mutation validation, projection, dispatch, response classification, and reconciliation;
- all JMAP state tokens and consistency-domain generations;
- cache and query-window materialization;
- notification eligibility and delivery state;
- Undo and Redo ordering and execution;
- background work priority and cancellation policy; and
- recovery of incomplete work after restart.

A GUI may request an operation. It does not decide how that operation is implemented after its
intent has been durably recorded.

### GUI writes are narrow and declarative

The GUI is not a general second database writer. Its connection policy permits only narrowly scoped
writes whose purpose is to record input originating in the GUI, principally immutable rows in a
durable intent inbox.

The GUI must not write:

- JMAP cache objects;
- mailbox or search query windows;
- sync state tokens;
- optimistic projections;
- mutation lifecycle rows;
- history stack transitions;
- notification discovery or delivery state;
- daemon generations; or
- server-derived account capability state.

The allowed write surface should be enforced at the database boundary, not left as a convention.
SQLite's authorizer mechanism, dedicated connection wrappers, and schema permissions expressed by
repository APIs can make unauthorized writes fail locally and loudly.

GUI-only workspace state may remain in a separate GUI-owned settings store. It must not be confused
with server-derived or mutation-authoritative state.

### IPC is never authoritative

A socket message may cause work to be scheduled, but it does not make the work durable. Both
processes follow the same ordering rule:

```text
commit authoritative SQLite transaction
then send optional wake-up or invalidation
```

Never send a wake-up first and rely on the receiver to wait for a later commit.

Socket loss can increase latency until reconnection. It cannot lose a command, cache change,
notification candidate, compose revision, or history transition.

### GUI lifetime is semantically irrelevant to background work

Closing or crashing the GUI must not:

- cancel a mutation or refresh;
- suppress a notification;
- discard a delayed send;
- lose an already queued compose revision;
- alter an Undo or Redo entry;
- close an account push subscription; or
- prevent journal recovery.

Conversely, restarting the GUI must not cause any network request merely because a window was
reconstructed. It renders the committed cache first and requests missing materialization through a
durable intent only when required.

### Data changes never clobber user intent

The daemon owns facts; the GUI owns navigation, selection, focus, viewport, and editing intent. A
cache commit, generation poke, synchronization result, optimistic reconciliation, notification, or
background materialization must never by itself:

- activate another tab or mailbox;
- select a newly inserted object;
- reinterpret a selected row number as a different object;
- replace the message, contact, or event currently being viewed;
- move the user's viewport merely because rows were inserted or removed elsewhere;
- steal focus or alter an editor cursor or selection; or
- overwrite a newer local compose or edit revision.

All durable and in-memory presentation state uses stable logical identities. Row indexes and query
positions are transient layout coordinates, not user intent. When a model changes, the GUI
reconciles around the selected object IDs, current object ID, multi-selection IDs, stable viewport
anchor, active tab identity, and relevant editor revision.

Precise row insertion, removal, movement, and data-change notifications are preferred over model
resets. When a reset is unavoidable, the GUI snapshots and restores stable selection, current item,
scroll anchor, expansion state, focus, and other applicable presentation state. It must not select
the object that happens to occupy the former row index.

The exact intent can change only through an explicit user action or when it has become impossible to
honour, such as permanent deletion of the selected object, loss of access, or account removal. Such
fallback is deterministic and domain-specific: preserve every surviving selected identity, retain
the current detail where possible, otherwise choose a documented neighbouring object or clear the
view. Accidental model reconstruction is never a navigation policy.

This invariant is local to the GUI and is not weakened by process separation. The daemon publishes
committed data changes; it never publishes presentation commands. Notification activation is an
explicit user navigation request and is therefore distinct from notification arrival.

## Process responsibilities

### The daemon

The daemon is the owner of all work that must survive without a window.

### JMAP and transport

The daemon owns:

- Session discovery and capability negotiation;
- credential and token use;
- HTTP JMAP requests;
- RFC 8887 JMAP WebSocket connections;
- EventSource state-change connections;
- binary upload and download operations;
- transport fallback decisions; and
- classification of dispatched, undispatched, definitive, and ambiguous outcomes.

No JMAP method name, state token, transport URL, bearer token, or protocol response crosses the IPC
boundary as application data.

### Synchronization and cache materialization

The daemon owns all account coordinators and every cache write described by the existing
architecture. This includes:

- account bootstrap;
- Mailbox, Email, Thread, ContactCard, AddressBook, Calendar, and CalendarEvent synchronization;
- watched-mailbox and push-driven refresh;
- full-offline mailbox mirroring;
- authoritative mailbox and search query-window materialization;
- raw MIME and attachment vault population;
- search-index maintenance;
- query-window invalidation and restoration;
- consistency fencing and optimistic rebase; and
- cache eviction policy.

Foreground materialization requested by an open GUI has priority over preemptible background mirror
or indexing work. The GUI expresses that priority in the durable request; the daemon remains the
single scheduler that decides how work is interleaved.

### Mutations and optimistic consistency

The daemon owns the mutation journal and all typed adapters. Consuming a user intent, creating the
corresponding journal or operation-group rows, and materializing its optimistic projection are one
SQLite transaction.

After that transaction commits, the operation proceeds independently of the originating GUI. The
daemon performs network dispatch, accepts or rejects individual object outcomes, preserves
ambiguity, fences stale refreshes, and writes the resulting effective state.

The process split must not weaken any invariant in
[OPTIMISTIC_CONSISTENCY.md](OPTIMISTIC_CONSISTENCY.md).

### Undo, Redo, and delayed send

The daemon owns the durable history described by [UNDO_REDO.md](UNDO_REDO.md). It decides the current
Undo and Redo heads, labels, availability, expiry, blocking state, and execution order.

Undo and Redo from the GUI are ordinary durable intents. The GUI does not pop a local stack or
predict the inverse operation.

Delayed send remains entirely daemon-owned. The deadline, dispatch transition, cancellation race,
history expiry, transport ambiguity, and reconciliation continue after the GUI exits. Notification
actions may cancel an eligible delayed send without first starting the main GUI.

### Notifications

The daemon owns notification discovery, the persistent notification outbox, publication, delivery
tracking, replacement, action handling, and retry policy.

A cache reconciliation transaction may create notification-outbox rows and advance the relevant
visible database generation atomically. Only after commit may the daemon publish a desktop
notification. Failure of D-Bus, the notification service, or the tray host must not roll back cache
state or interrupt account synchronization.

Notification activation is represented by a durable UI route containing stable account, mailbox,
thread, Email, contact, or calendar identifiers. The daemon records the route before starting or
waking the GUI. The GUI reads and resolves the route from SQLite. Neither command-line arguments nor
IPC payloads need to carry application objects.

Route delivery is at least once. Resolving the same stable route more than once must be harmless.
The GUI records completion through a narrow durable input, allowing the daemon to retire or retain
the route according to policy.

### Tray icon

In the two-process design, the daemon owns the tray icon because it remains present when the main GUI
is absent. Tray construction and failure are isolated from synchronization policy even though they
share a process.

The daemon must continue headlessly if:

- no StatusNotifier host exists;
- the session bus is unavailable;
- tray registration fails; or
- the desktop shell restarts.

A future third `javelin-tray` process could isolate desktop integration further, but the correctness
model does not require it. The two-process boundary is designed so that such a split would remain a
presentation change rather than a cache or mutation change.

### Database lifecycle

The daemon owns schema creation and migration. It reaches a well-defined `ready` state only after:

- the database is opened;
- any migration transaction has completed or rolled back;
- crash recovery has classified incomplete journal and history rows;
- the command inbox is available; and
- its local socket is accepting connections.

The GUI must not open application models against a schema the daemon is currently migrating.

### The GUI

The GUI exists only while a visible application surface is needed.

### Presentation

The GUI owns:

- windows, dialogs, menus, tabs, and selection;
- Qt item models for currently visible data;
- WebEngine message rendering;
- compose editor widgets;
- transient view state such as scroll positions and expansion;
- confirmation and error presentation; and
- GUI-only workspace persistence.

It hydrates visible models from SQLite and releases them when views close. It may retain lightweight
tab descriptors, but inactive widgets and WebEngine instances are not required to survive process
exit.

### Cache reads

The GUI reads the same authoritative tables and query windows that the current single-process GUI
reads. It does not receive row data through IPC.

Read transactions are short-lived. A GUI model refresh:

1. opens or begins a fresh read snapshot;
2. reads the applicable committed generation and the view data;
3. finishes all queries and releases the snapshot; and
4. replaces or incrementally updates its presentation model.

No active query or read transaction crosses an event-loop suspension. Long-lived snapshots would
prevent WAL checkpoint progress and could leave a view causally behind a generation it has already
observed.

### Durable user intent

A user action is considered accepted locally only when the GUI has durably appended its intent to
SQLite. After that commit, the GUI fires and forgets. It does not wait for or query a mutation result
through IPC.

If the local append fails because storage is unavailable, corrupt, incompatible, or persistently
busy, the GUI must report a local persistence failure. Fire-and-forget begins after durable local
acceptance; it does not mean silently discarding an action that never reached SQLite.

The append must not block the GUI thread. SQLite's cross-process writer lock serializes the narrow
GUI insert with daemon transactions. Daemon transactions remain short, contain no suspension or
network work, and release the writer lock promptly.

### Rendering operation state

The GUI does not maintain an operation status client. Pending, accepted, rejected, unknown,
conflicted, expired, and blocked states are ordinary durable database state.

A mailbox row, history menu, send indicator, or error surface derives its presentation from:

- the effective cache projection;
- durable journal and history views intended for presentation;
- durable user-feedback or error records; and
- committed domain generations.

The GUI may correlate a transient click with an intent UUID for immediate interaction feedback, but
that UUID does not create a second lifecycle. Once the intent append commits, SQLite is the only
source of the operation's meaning.

### Missing data

When the cache lacks a page, message body, attachment, contact detail, expanded calendar range, or
online search result, the GUI records an `Ensure...` intent. The daemon materializes the requested
working set and advances the relevant generation. The GUI continues showing any valid cached state
until the daemon's commit poke causes a new query.

The GUI never performs direct JMAP access as a latency fallback.

### Compose

The visible editor buffer is transient GUI state, but its recoverable compose session is durable.
Debounced compose revisions are recorded as versioned intents. Each revision has a monotonically
increasing session revision, so delayed processing cannot replace newer content.

Before the GUI is allowed to discard a compose window or exit, its latest revision must have been
committed to the durable inbox. It does not need to wait for server draft synchronization.

A send intent references a durable compose revision. The daemon cannot submit an older revision by
accident, and the GUI may exit immediately after the send intent has been durably accepted.

### GUI-only facilities

Features whose state is not part of background correctness may remain GUI-local. Examples include
WebEngine rendered artifacts, transient HTML processing, and optional translation presentation.
If such a feature needs a persistent rebuildable cache, it may use a separate GUI-owned store rather
than broadening the GUI write permissions on the authoritative JMAP cache.

## Durable intent inbox

The intent inbox is the application command boundary between the GUI and daemon.

### Intent properties

Each intent has at least:

- a globally unique intent ID;
- an origin process or client instance ID for diagnostics;
- a typed intent kind;
- a payload schema version;
- a typed serialized payload;
- a creation timestamp; and
- a priority class where foreground materialization must preempt background work.

The payload is a closed set of typed application intents. It is not arbitrary SQL, raw JMAP JSON,
a C++ closure, or a remotely invoked service method.

Examples include:

- move, copy, keyword, delete, and restore mail commands;
- Undo and Redo;
- save compose revision and send compose session;
- cancel delayed send;
- create, update, move, or destroy contact and calendar objects;
- ensure mailbox window, search window, message content, attachment, or calendar range;
- refresh account;
- update account or synchronization settings; and
- acknowledge a durable UI route or user-visible failure.

### Append semantics

The GUI appends an intent in one transaction and commits before sending a wake-up. A unique
constraint on the intent ID makes retries idempotent.

The GUI does not modify the intent after commit. Corrections are new intents with their own IDs and
explicit relationship to the earlier command where required.

### Consumption semantics

The daemon scans unconsumed intents in durable order, applying application scheduling policy where
priority permits reordering of independent work.

For a stateful mutation, one daemon transaction:

1. reads and validates the intent against current durable state;
2. creates the application operation group and typed mutation records;
3. materializes the optimistic projection;
4. records that the intent has been consumed and links it to the operation group; and
5. advances the applicable visible generations.

The transaction either commits all of these effects or none of them.

A command that is definitively invalid before projection is still consumed exactly once and creates
a durable user-visible failure record. The GUI will observe that record as part of normal database
refresh; it does not ask the daemon for a command response.

Procedural `Ensure...` intents may coalesce with equivalent pending or in-flight work. Coalescing is
daemon policy and does not change the durability of the user's request.

### Recovery and deduplication

The daemon examines the inbox at startup and after every GUI wake-up. An intent committed before a
GUI crash remains processable. An intent already consumed cannot create a second operation after a
daemon restart.

The inbox records durable local acceptance, not server success. Server ambiguity belongs to the
mutation journal and remains governed by existing reconciliation rules.

### Retention

Consumed intents are retained only as long as required for crash recovery, diagnostics, and
correlation with durable operations. Retention is bounded. Journal, history, error, and audit state
must not rely on retaining the entire command payload indefinitely.

## Database generations

SQLite commits are discovered through generation pokes rather than polling.

### Generation model

The daemon maintains a monotonic global visible generation and domain generations such as:

```text
mail
mailbox tree
message content
contacts
calendars
journal and user feedback
history
compose
account configuration
UI routes
```

The exact set is a schema decision, but generations have these properties:

- they are stored in SQLite;
- the applicable values advance in the same transaction as the visible state change;
- they do not advance before the data commit;
- they never decrease or reset during an ordinary daemon restart; and
- they describe committed view invalidation, not every internal bookkeeping write.

A transaction may update several domain generations and one global generation. Internal writes that
cannot affect any GUI query do not need to wake the GUI.

### Generation authority

The generation stored in SQLite is authoritative. A generation included in IPC is only an
invalidation hint and a way to detect that the GUI's last rendered snapshot is stale.

The GUI records the highest generation it has fully rendered for each relevant domain. After a
poke, it opens a fresh SQLite snapshot, compares domain generations, and executes only the necessary
queries.

A domain mask may accompany the IPC generation to avoid unnecessary reads. The mask is advisory.
If it conflicts with SQLite, SQLite wins.

### Commit publication

After a successful transaction that changes GUI-visible state, the daemon publishes the newest
global generation to every connected GUI:

```text
commit data and generations
COMMIT succeeds
coalesce pending invalidation for each GUI
write newest generation to socket
```

The daemon never blocks a database transaction waiting for a GUI socket. Socket publication happens
after the write lock has been released.

## Local IPC

### Purpose

IPC provides low-latency process coordination without becoming a second source of application
state. It replaces idle database polling.

A persistent Unix-domain socket is the preferred Linux mechanism. `QLocalServer` and
`QLocalSocket` provide integration with the Qt event loop and sufficient latency. Shared-memory
futexes, `eventfd`, WAL file watches, and a database server are unnecessary for the required
semantics.

### Message set

The protocol is deliberately small. Conceptually it contains:

```text
GUI -> daemon
    HELLO(protocol version, GUI build identity)
    PING
    COMMANDS_AVAILABLE
    GUI_READY_FOR_ACTIVATION

Daemon -> GUI
    READY(protocol version, schema version, daemon instance, current generation)
    PONG
    DATABASE_CHANGED(current generation, optional domain mask)
    ACTIVATION_AVAILABLE
    SHUTTING_DOWN
```

`COMMANDS_AVAILABLE` and `ACTIVATION_AVAILABLE` are payload-free wake-ups. The receiver reads the
actual intents or routes from SQLite.

Build and schema identities are coordination metadata, not application data. They prevent an
incompatible process pair from operating on the same database.

### Security

The socket lives in the user's runtime directory with permissions restricting it to that user. The
daemon verifies peer credentials where the platform supports it. A stale socket path is not proof
of daemon liveness; only a successful protocol handshake is.

No passwords, tokens, raw messages, recipient lists, attachment contents, or JMAP payloads traverse
the socket.

### Latency

The socket exists to make a committed change visible on the next GUI event-loop opportunity without
a polling interval. Local stream delivery is expected to be negligible compared with model queries,
rendering, and process scheduling. The design does not promise a hard scheduler-level sub-millisecond
bound, but it introduces no intentional delay, timer, or idle busywork between commit and
invalidation.

### Coalescing and backpressure

A GUI does not need to render every intermediate commit. If generations 101, 102, and 103 commit
before it refreshes, receiving generation 103 is sufficient.

The daemon therefore keeps at most the newest pending invalidation for each slow GUI. It must not
build an unbounded socket queue. When the socket becomes writable, it sends the newest generation.

The GUI similarly coalesces refresh requests. It performs a fresh query for the latest observed
generation rather than scheduling one model reload per socket frame.

### Lost connections

A connected local stream provides ordered delivery while healthy. If it disconnects, both sides
know that notification continuity has been lost.

The GUI does not poll SQLite while disconnected. It may continue displaying its last committed
snapshot, but it marks daemon-dependent freshness and mutations unavailable. On reconnect, the
`READY` handshake contains the daemon's current generation, causing a refresh if the GUI is behind.

Commands already committed to SQLite remain safe despite socket loss. The daemon scans the inbox on
startup and after reconnect even if the original wake-up was never delivered.

## Race-free startup and reconnection

The GUI must not miss a commit made between loading SQLite and subscribing for invalidations.

The startup sequence is:

1. connect to the daemon socket and complete the version handshake;
2. receive the daemon's current committed generation;
3. open a fresh SQLite snapshot and read the stored generation with the initial visible data;
4. finish the read snapshot;
5. process any newer invalidation already queued on the socket; and
6. repeat the affected queries if the rendered generation is behind the newest observed
   generation.

Because the GUI subscribes before reading, any commit after the handshake either appears in its
SQLite snapshot or produces a queued invalidation. A coalesced invalidation still carries a
generation at least as new as every omitted intermediate commit.

After daemon restart, the daemon instance ID changes. The GUI discards assumptions about socket
continuity, performs the same handshake, reopens its SQLite connections if required, and refreshes
from the current committed generations.

## Database concurrency

SQLite remains in WAL mode. Independent GUI read connections do not block daemon writes, provided
read transactions and queries are short-lived.

The main database has one SQLite writer at a time across both processes. The daemon remains the only
writer of authoritative application state, while GUI writes are narrow intent appends. This avoids
a separate command database and permits atomic consumption of an intent with journal and projection
creation.

A separate command database would create a cross-database atomicity problem: a command could be
marked consumed without its projection transaction, or a projection could commit without consuming
the command. Keeping the inbox in the authoritative database lets SQLite enforce the transition.

The in-process write coordinator described by [DATABASE_ACCESS.md](DATABASE_ACCESS.md) continues to
serialize daemon threads. Cross-process contention is handled by SQLite's writer lock and bounded
busy handling. Neither process may hold a write transaction across network, filesystem, parsing, UI,
or coroutine suspension.

A GUI intent append that cannot acquire and commit the writer lock within the local acceptance
policy fails visibly rather than being silently deferred in memory.

## GUI refresh behavior

A database poke does not require rebuilding every open model.

The GUI has a small refresh coordinator that compares committed domain generations and asks active
presenters to re-query SQLite. It contains no synchronization or mutation logic.

Examples:

- a mailbox-tree generation change reloads counts and hierarchy;
- a mail-window generation change reloads affected active mailbox or search windows;
- a message-content generation change reloads the currently selected message only if its stable ID
  is affected;
- a history generation change updates Edit menu labels and availability;
- a journal or feedback generation change updates pending markers and presents new durable errors;
- a contacts change reloads the active contacts model but does not construct it while no Contacts
  tab exists; and
- a calendar change reloads only visible ranges.

Affected IDs or mailbox IDs remain in SQLite change journals or invalidation tables when finer
selection is needed. They are not required in IPC. The GUI may query changes since its last domain
generation and fall back to a complete active-model reload if that bounded change history has been
pruned.

This is a cache presentation concern, not a second synchronization layer.

A refresh coordinator never applies a database generation by replacing user-navigation state. It
first records the active tab, stable selected and current object IDs, multi-selection, viewport
anchor, and any editor revision. It then reconciles the affected model and restores those identities
against the new committed rows. A new message inserted above the viewport may change row numbers and
mailbox totals, but it does not change the message being read or add that new row to the selection.

If the selected object remains known but temporarily falls outside a newly materialized sparse
window, the detail view remains attached to that object. The GUI requests an anchored window around
the selected or visible object rather than silently selecting an object from the new positional
page. Only confirmed disappearance of the selected object invokes the domain's deterministic
fallback policy.

## Process and failure semantics

### GUI exits normally

The GUI commits any required workspace state and latest compose revisions, closes its read
connections, and exits. The daemon and all background operations continue.

No explicit transfer of active operations is necessary because the GUI never owned them.

### GUI crashes

Already committed intents remain in the inbox. Uncommitted editor keystrokes since the latest
durable compose revision may be lost, just as in any editor between autosaves, but no committed
mutation, delayed send, journal transition, or notification state is affected.

A replacement GUI reconstructs its views from SQLite.

### Daemon exits normally

The daemon stops accepting new wake-ups, publishes `SHUTTING_DOWN` where possible, closes transports,
and leaves all queued or ambiguous work durable. The GUI may remain as a read-only cached viewer but
must disable daemon-dependent mutations and fresh materialization.

### Daemon crashes

SQLite transaction atomicity prevents partially committed cache or journal transitions. On restart,
existing recovery rules convert abandoned `in_flight` mutations to `unknown`, recover executing
history entries, inspect delayed sends, process unconsumed intents, and resume push synchronization.

A connected GUI detects socket loss immediately. It must not infer that an operation failed merely
because the daemon disappeared.

### Socket poke is lost

If the socket disconnects before a poke is delivered, the database commit remains authoritative.
Reconnection reveals the current generation. A wake-up for a queued command is likewise optional;
the daemon scans the durable inbox on startup and reconnect.

### GUI loses local command acknowledgement

The local SQLite commit is the acknowledgement. If the GUI process crashes after commit but before
updating its own transient presentation, the command remains queued. Repeating the same intent ID is
idempotent.

### Server mutation outcome is ambiguous

Nothing about the process split changes the rule: the daemon records `unknown`, preserves the
projection, and performs typed reconciliation. The GUI renders the resulting durable state and does
not ask IPC to resolve it.

### Database migration or version mismatch

The daemon performs migrations before readiness. The handshake reports protocol and schema
versions. An incompatible GUI refuses mutations and does not attempt to interpret an unsupported
schema.

Application upgrades should replace daemon and GUI as one product version. Compatibility shims that
accept old and new database or IPC shapes indefinitely are outside the design.

### D-Bus or desktop shell failure

Synchronization and journal processing continue. Notification-outbox entries follow their durable
retry or failure policy. Tray registration may be recreated when the desktop service returns. These
failures never make the daemon restart its JMAP coordinators or discard cache changes.

### System suspend and resume

The daemon owns transport recovery, clock-sensitive delayed-send reevaluation, and push-session
restart. The GUI, if present, receives a later generation poke or reconnects and renders SQLite. It
does not independently infer missed server state from elapsed wall time.

## Notification and activation flow

A representative new-mail flow is:

```text
JMAP push arrives at daemon
    -> daemon reconciles Email, Mailbox, query windows, projections, and state tokens
    -> one SQLite transaction creates any notification-outbox rows
       and advances visible generations
    -> COMMIT
    -> daemon pokes connected GUI processes with newest generation
    -> daemon publishes desktop notification from durable outbox
```

Activation when no GUI exists is:

```text
user activates notification
    -> daemon persists stable UI route
    -> daemon starts or activates GUI
    -> GUI completes handshake
    -> GUI reads pending route and cache from SQLite
    -> GUI renders cached target immediately where possible
    -> GUI queues EnsureMailboxWindow or EnsureMessageContent only when required
    -> daemon materializes missing state and pokes GUI
    -> GUI resolves route and durably acknowledges completion
```

Notification publication is not delayed waiting for a GUI, and GUI startup is not required for
notification actions that the daemon can complete directly, such as eligible Undo Send.

## Mutation flow

A representative move operation is:

```text
user chooses Archive
    -> GUI appends typed MoveEmails intent and commits
    -> GUI sends payload-free COMMANDS_AVAILABLE wake-up
    -> daemon consumes intent
    -> one transaction appends mutation/history state, projects membership,
       consumes intent, and advances generations
    -> COMMIT
    -> daemon pokes GUI
    -> GUI re-queries effective mailbox window from SQLite
    -> daemon dispatches JMAP mutation
    -> daemon atomically accepts, rejects, or preserves unknown outcome
    -> COMMIT and poke if visible state changed
```

The GUI never receives a mutation response. The visible projection and durable failure surfaces are
the response.

If the GUI exits immediately after its intent commit, every later step is unchanged.

## Pagination and content flow

An uncached online-first mailbox is represented by sparse authoritative query windows, not by
applying SQL offsets to whatever Email objects happen to be cached. A direct jump to position 400
therefore does not fetch positions 0 through 399 and does not fabricate a page from partial mailbox
membership.

A representative deep jump is:

```text
user requests position 400
    -> GUI records position 400 as its current desired window
    -> GUI queries the matching SQLite query-window key
    -> window is absent: retain the previous visible page where useful, show foreground loading,
       and append EnsureMailboxWindow(position=400, limit, sort, collapse policy)
    -> GUI sends payload-free COMMANDS_AVAILABLE wake-up
    -> daemon performs a direct positional Email/query
    -> daemon obtains the returned position, total, queryState, and ordered representative IDs
    -> daemon materializes every required Email and Thread object
    -> daemon atomically commits a complete query window and advances its generation
    -> daemon pokes the GUI
    -> GUI installs the page only if position 400 is still the current desired window
```

A result that arrives after the user has navigated elsewhere remains a useful cached window but
cannot activate itself or replace the current page. The GUI's desired-window identity is
presentation state, not a daemon operation lifecycle.

The first uncached positional jump can only mean “the server results occupying position 400 when the
query executes.” If remote changes occur before the server evaluates that request, Javelin cannot
identify the objects that formerly occupied an uncached position. This is an unavoidable property of
positional queries. Once the returned stable IDs are known, subsequent data changes must preserve
those identities rather than preserve their old numeric offsets.

For example, after the page at position 400 is shown, new mail inserted above it may move the
selected Email from server position 403 to 406. The GUI continues displaying and selecting that
Email. If the sparse window needs authoritative renewal, the GUI requests an anchored query around
the selected Email or a stable visible-row anchor, with an anchor offset preserving its viewport
placement. The server-returned position updates the pager; it does not redefine the selection. A
plain repeat of positional offset 400 is appropriate only for a fresh explicit jump, not for
maintaining an already visible view through mailbox changes.

A complete previously displayed window may remain as a continuity snapshot while an anchored
refresh is pending. Once marked stale it is not proof of current server position, total, or ordered
membership and cannot be used to infer uncached adjacent pages. It is retained only to avoid a blank
or disruptive UI. The new authoritative window replaces its data by stable identity when committed.

If no object is selected, the top visible representative is the preferred viewport anchor. If that
anchor was deleted, the GUI uses the nearest surviving visible identity and then the domain's
deterministic fallback. Insertions and removals never cause the object inheriting an old row number
to inherit selection.

The daemon may coalesce duplicate requests for the same account, query identity, position or anchor,
limit, and sort. Foreground materialization outranks background mirroring and prefetch. Once a JMAP
request has been dispatched, GUI navigation does not need to cancel it for correctness; an obsolete
result simply becomes a cached window that the GUI does not install.

If the daemon is unavailable, the GUI may display an already materialized window as a read-only
snapshot. It cannot satisfy an uncached jump, and it must not simulate position 400 from a partial
set of cached Email rows.

Complete-offline mailboxes use the same GUI contract, but the daemon resolves the requested window
from authoritative effective SQLite membership without network access.

List materialization and message-content materialization remain separate. A newly loaded page may
show cached summary data immediately. Selecting an Email whose body or raw source is absent appends
an `EnsureMessageContent` intent; the selected identity and list viewport remain unchanged while the
content is fetched. Message bodies, attachments, online searches, contact details, and calendar
ranges otherwise use the same commit-then-poke pattern. Data is materialized into SQLite or the vault
and never returned as an IPC response.

## Settings ownership and migration

The process split makes settings ownership part of the correctness boundary. A setting belongs to
the process whose behavior it controls; it does not move into shared storage merely because both
processes can technically read it.

### Classification rule

A setting is daemon-owned when changing it can affect work that must remain correct after the GUI
exits. Daemon-owned settings are typed durable database state and are changed through durable
intents. A setting is GUI-owned when it affects only presentation or interactive editing. GUI-owned
settings remain in a GUI settings store and are never required for background correctness.

Representative ownership is:

| Setting or state | Owner and storage | Reason |
| --- | --- | --- |
| configured connection identity, display name, login identifier, enabled state, explicit Session URL, and discovered account association | daemon-owned typed SQLite tables | required to start coordinators, label notifications, and reconnect without a GUI |
| account credential reference | daemon-owned SQLite reference; secret in platform credential storage | daemon authenticates, but SQLite and IPC must not contain the secret |
| complete-offline mailbox selection and future background-download policy | daemon-owned typed SQLite tables | controls persistent mirroring and background scheduling |
| notification mailbox policy, including explicit “none” versus default Inbox behavior | daemon-owned typed SQLite tables | notification eligibility must be identical with or without a GUI |
| Undo Send delay and other send-dispatch policy | daemon-owned typed SQLite settings | delayed submission must survive GUI exit and restart |
| per-account pause/disable policy and user-configurable retry constraints | daemon-owned typed SQLite settings | controls background transport and synchronization |
| authentication-paused revision, migration state, retry state, and applied configuration revision | daemon-owned operational SQLite state, not a preference | recovery and suppression decisions must survive restart |
| tray behavior that is implemented by the daemon | daemon-owned typed SQLite settings | the tray exists when the GUI does not |
| window geometry, active tabs, selected object identities, scroll anchors, splitters, and search presentation | GUI-owned settings | presentation state has no daemon semantics |
| message colors, HTML appearance, calendar color overrides, font and density choices | GUI-owned settings | visual rendering only |
| remote-content allowlists | GUI-owned settings | consulted only by the message renderer; the daemon does not render HTML |
| attachment save directory and “always ask” behavior | GUI-owned settings | interactive file selection only |
| translation enablement, target language, and automatic sender/domain choices | GUI-owned settings while translation remains a view operation | no translation work is required after the GUI exits |
| translation provider secret override | GUI credential-store entry, not SQLite or plain settings | secret used only by the GUI-owned translation service |
| composer editor defaults | GUI-owned settings unless they alter daemon send semantics | editor presentation is local; durable submitted content is already in the compose session |

The classification follows behavior rather than current class placement. Moving a class between
libraries does not change ownership. If a future feature makes a currently visual setting affect
background processing, that setting must migrate to daemon-owned typed state before the daemon may
act on it.

### Daemon settings schema

Daemon-owned settings should not use one untyped `settings(key, value)` table. They require typed
schemas, constraints, explicit defaults, and normal database migrations. Representative logical
records are:

```text
configured_connections
    connection_id
    revision
    display_name
    login_identifier
    explicit_session_url
    credential_reference
    enabled

configured_account_associations
    connection_id
    account_id

mailbox_offline_policy
    account_id
    mailbox_id
    mode

mailbox_notification_policy
    account_id
    selection_mode

mailbox_notification_members
    account_id
    mailbox_id

daemon_preferences
    revision
    undo_send_delay_seconds
    tray_policy
```

The exact schema remains subject to iteration, but several semantic distinctions are mandatory.
Notification policy must not infer meaning from a missing settings key: “use the default Inbox,” “use
this explicit set,” and “notify for no mailboxes” are distinct durable states. Likewise, absence of
an offline-policy row means online-first; it must not be confused with a mailbox whose full mirror is
paused, incomplete, or awaiting removal.

Server-derived capabilities, JMAP state tokens, synchronization progress, and transport health are
not settings even when displayed in Preferences. They remain daemon-owned operational state in
their existing typed repositories.

### Reading settings

The daemon reads its complete effective configuration from SQLite during startup before opening
network sessions. It does not require a running GUI or read GUI-owned `QSettings` during ordinary
operation.

The GUI reads daemon-owned settings from SQLite to populate Preferences. It may retain an editable
local draft while the dialog is open, but that draft is not authoritative. Database invalidations
must not overwrite unsaved controls. If the committed settings revision changes while the user is
editing, the dialog marks the draft as based on an older revision and requires an explicit merge,
reload, or retry rather than silently replacing either side.

GUI-only settings remain local to the GUI process. They should be stored under a stable GUI
application identity so restarting only the daemon cannot change their path or defaults.

### Changing settings

The GUI changes daemon-owned settings through typed durable intents, not direct updates to canonical
settings rows. Examples include:

```text
UpdateConfiguredConnection
SetOfflineMailboxPolicy
SetNotificationMailboxPolicy
SetUndoSendDelay
RemoveConfiguredAccount
```

An intent contains the setting-specific values and the base revision the user edited. The daemon:

1. validates the current committed revision and the typed values;
2. performs any required credential-reference preparation;
3. atomically updates the canonical settings, consumes the intent, and advances the settings
   generation;
4. commits;
5. applies the new committed configuration to affected coordinators; and
6. sends the ordinary database-generation poke.

A stale base revision is not resolved by last-writer-wins. The daemon leaves the newer committed
configuration intact and records durable conflict feedback. This prevents an old Preferences window
from silently restoring an account URL, notification selection, or offline policy that changed in
another GUI session or during recovery.

Related settings that form one user decision are committed together. For example, changing an
account login identifier, Session URL, credential reference, and revision is one configuration
transition. Notification selection mode and its member set are one transition. The daemon must not
briefly run a mixture assembled from several independently committed GUI writes.

Simple syntactic checks may run in the GUI for immediate usability, but daemon validation remains
authoritative. No correctness-sensitive validation result is carried as an IPC reply; acceptance,
conflict, or failure is durable database state observed through the normal generation mechanism.

The daemon applies side effects only after the settings transaction commits. If it crashes before
restarting an account coordinator or scheduling a newly selected offline mailbox, startup reads the
new canonical settings and performs the missing effect. Conversely, an uncommitted settings edit
cannot partially reconfigure live background work.

Account removal is not a scalar preference update. It is a destructive durable operation whose
policy covers credentials, cached account data, pending mutations, delayed sends, notifications,
history, compose sessions, and recovery. Its database transitions must follow a separately defined
atomic or staged removal lifecycle.

### Secrets

Secrets never appear in:

- canonical settings tables;
- the durable intent payload;
- IPC messages;
- diagnostic generation events; or
- ordinary logs.

The GUI writes a new secret to the platform credential store under a stable generated reference and
queues only that reference. The daemon resolves it under the same user identity. Replacing or
removing an account eventually removes superseded credential entries according to a recoverable
cleanup policy.

Interactive authentication may require GUI presentation, but transport ownership and durable account
activation remain in the daemon. The database can represent an account as requiring credentials or
interactive reauthentication without storing secret material.

### Settings generations

Daemon-owned settings participate in the same commit-then-poke model as other state. A global
settings generation and, where useful, per-domain or per-account revisions allow:

- the GUI to refresh only affected Preferences pages;
- the tray to update labels or policy;
- coordinators to determine whether their applied configuration is current; and
- reconnecting processes to detect missed changes without polling.

The poke carries no setting values. The GUI and daemon read the committed typed rows. The daemon does
not need IPC notification for its own transaction; it schedules post-commit reconfiguration directly
and verifies the applied revision during startup and recovery.

### Legacy `QSettings` migration

The current monolithic application stores several daemon-owned values in `QSettings`, including
configured accounts, API keys, cached-account associations, complete-offline mailbox selections,
notification mailbox selections, Undo Send delay, and authentication-pause revisions. These cannot
remain split between two independently running processes.

The daemon owns the one-time migration and performs it before advertising `READY` or starting account
coordinators. The daemon must initialize the same organization and application identity used by the
legacy process before reading the old settings file.

Migration follows these rules:

1. Apply the normal SQLite schema migration first.
2. Check a versioned durable legacy-import record in SQLite.
3. If import is required, read and normalize the complete legacy daemon-owned configuration as one
   typed snapshot.
4. Store account secrets in the platform credential store under deterministic or durably recorded
   references and verify they can be read back.
5. In one SQLite transaction, insert canonical non-secret settings and credential references,
   preserve explicit-default distinctions, import operational pause state, and commit the
   legacy-import version.
6. Re-read the committed configuration and verify its invariants before daemon readiness.
7. Remove obsolete daemon-owned legacy keys, especially plaintext secrets, only after the canonical
   import is proven durable. GUI-owned groups remain untouched.

Credential storage and SQLite cannot share one transaction. Crash safety therefore comes from
idempotent ordering: write or replace the credential-store entry first using the same stable
reference, then commit the SQLite reference. A crash before the database commit may leave an
unreferenced credential entry, which is safe and can be reused or cleaned later. The reverse order is
forbidden because it could commit a configuration referring to a secret that was never stored.

If legacy daemon-owned configuration exists but cannot be migrated unambiguously or its credentials
cannot be secured, the daemon enters an explicit migration-failed state. It must not silently start
with defaults, empty accounts, default-Inbox notification policy, or disabled offline mirroring.
The GUI reads and presents the durable migration failure.

After the import marker commits, SQLite is the only source of truth for those settings. Neither
process keeps a fallback reader for the old keys, and later changes to the legacy file are ignored.
Downgrading to a build that expects the old setting shape is outside this design.

### Database durability consequence

Once configuration lives in SQLite, the main database is not a disposable cache file. It already
contains non-rebuildable or recovery-critical state such as optimistic mutations, history, delayed
sends, compose sessions, notification outbox entries, and local user intent; daemon settings make
that distinction even clearer.

A user-facing “clear cache” or repair operation must delete only rebuildable server materialization
and vault data. It must preserve configuration, credential references, intents, journals, history,
compose state, notification state, and settings migrations unless the user explicitly requests a
full profile reset. Wholesale deletion of the database is account/profile destruction, not cache
maintenance.

Auxiliary tools must use the same typed configuration repositories and credential references, or
act through durable intents. They must not continue reading legacy account credentials from
`QSettings` after migration.

## Memory and lifetime consequences

The daemon retains only state needed for continuous correctness and responsiveness:

- live account coordinators and transport sessions;
- bounded active synchronization work;
- notification, history, delayed-send, and journal coordination;
- SQLite connections and small scheduling structures; and
- tray integration.

It does not retain:

- WebEngine profiles or render processes;
- message-view documents;
- inactive tab widgets;
- full contact documents for list presentation;
- calendar cell widgets;
- compose editors; or
- GUI image and backing-store allocations.

The GUI can exit when the last window is closed. Reopening it reconstructs only currently visible
working sets from SQLite. This obtains the large memory saving without teaching a live monolithic
process to partially destroy and recreate a deeply connected widget tree.

Memory policy remains subordinate to correctness. The daemon may keep a bounded working set when it
materially improves push handling, foreground cache response, mutation reconciliation, or
notification reliability. Data already durable in SQLite should otherwise be released when no
active operation requires it.

## Why not a database server

A client/server database would provide built-in notification mechanisms such as `LISTEN/NOTIFY`, but
would add a service lifecycle, authentication, packaging, upgrade, backup, repair, and idle-memory
burden to solve a notification problem already handled by a few bytes on a local socket.

SQLite already provides the required durability, atomicity, WAL concurrency, local inspectability,
and crash behavior. The missing cross-process wake-up is deliberately supplied by IPC without
moving application data into that channel.

## Why not poll SQLite

Polling `PRAGMA data_version`, generation rows, or application tables would create permanent idle
work and impose a latency/energy trade-off. A long interval feels sluggish; a short interval wakes
the process needlessly.

Generations are queried after event-driven socket pokes and during startup or reconnect only. They
are not polled during an intact daemon connection.

## Why not watch the WAL

Filesystem notification on the WAL observes SQLite implementation activity rather than semantic
commits. Events may coalesce, checkpoints change file behavior, and an observer cannot safely infer
which application data is now visible.

The daemon already knows exactly when a relevant transaction commits and can publish the committed
generation directly.

## Why not rich IPC

A rich request/response service would duplicate application concepts on both sides of the process
boundary:

- operation handles and result lifecycles;
- remote cache models;
- reconnect and replay behavior;
- object serialization and compatibility;
- partial result delivery; and
- ambiguity handling distinct from the durable journal.

The durable database already solves these problems. Restricting IPC to process coordination keeps
one operation model and one source of truth.

## Multiple GUI processes

The storage and invalidation model can support more than one GUI process:

- each is an independent SQLite reader;
- each has its own socket connection and last-rendered generations;
- intent UUIDs prevent duplicate local commands; and
- the daemon broadcasts or coalesces invalidations per connection.

Product policy may still enforce one main GUI instance to avoid confusing workspace and activation
semantics. That policy is independent of mutation and cache correctness.

If multiple GUIs are allowed, notification routes and GUI-only workspace records need an explicit
claim or audience model. The daemon remains the only operation authority.

## Observability

Diagnostics should make the process boundary inspectable without logging message content or
credentials.

Useful events include:

- daemon instance and protocol handshake;
- GUI connect and disconnect;
- intent ID, kind, append, consumption, and deduplication;
- transaction generation and changed domains;
- coalesced invalidation delivery;
- daemon readiness and migration failures;
- durable notification route creation and completion; and
- reconnect generation catch-up.

Logs must distinguish:

- intent committed locally;
- mutation projected;
- request dispatched;
- server result accepted or rejected;
- transport result unknown; and
- GUI generation rendered.

A socket poke must never be logged as if it were the underlying cache or mutation event.

## Design constraints on existing subsystems

The process split preserves the current architectural contracts:

- [ARCHITECTURE.md](ARCHITECTURE.md) remains the responsibility layering inside the daemon and GUI;
- [DATABASE_ACCESS.md](DATABASE_ACCESS.md) continues to govern connection and transaction lifetime;
- [OPTIMISTIC_CONSISTENCY.md](OPTIMISTIC_CONSISTENCY.md) remains authoritative for mutation and
  refresh causality;
- [QUERY_WINDOWS.md](QUERY_WINDOWS.md) remains authoritative for ordered pagination and cache
  coverage;
- [OFFLINE_MAIL_ARCHITECTURE.md](OFFLINE_MAIL_ARCHITECTURE.md) remains authoritative for offline
  mirrors and vault policy; and
- [UNDO_REDO.md](UNDO_REDO.md) remains authoritative for history execution and delayed send.

The new boundary adds one further rule to all of them:

> Any work that must remain correct after the last GUI window disappears belongs to daemon-owned
> services and durable storage, never to widget lifetime or GUI-only callbacks.

## Matters reserved for further design review

The core design does not depend on resolving every product-level choice immediately. Further review
should settle:

- whether the tray remains in `javelind` or eventually becomes a third minimal frontend;
- whether the daemon is started by the GUI, desktop autostart, a user service manager, or socket
  activation;
- whether Javelin permits multiple simultaneous main GUI processes;
- the exact set and retention policy of per-domain change journals used for incremental model
  refresh;
- the exact boundary between core authoritative SQLite and GUI-only persistence;
- credential-store references and interactive authentication handoff;
- whether the GUI remains available as a cached read-only viewer while the daemon is unavailable;
  and
- the framed encoding of the very small versioned IPC protocol.

These choices may affect integration and presentation. They must not alter the central invariants:
SQLite carries durable application truth and intent, the daemon owns operation execution, and IPC
carries only process coordination and committed-generation invalidation.
