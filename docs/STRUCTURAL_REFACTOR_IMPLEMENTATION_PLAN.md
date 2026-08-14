# Javelin structural refactor implementation plan

## Authority and purpose

This document describes a long-term structural refactor of Javelin-Mail. It does not propose a rewrite and does not change the fundamental architecture documented in `ARCHITECTURE.md`.

The refactor has one central objective:

> Make Javelin's source tree, CMake dependency graph, classes, IPC contracts, and tests express the architecture that the project already intends.

The following architectural decisions remain authoritative throughout the programme:

* `javelind` owns JMAP, synchronization, writable cache access, authentication, background work, canonical operational settings, and stateful application commands.
* `javelin` owns presentation and reads committed cache state.
* SQLite remains the immediate local data plane.
* The JMAP server remains the recoverable source of truth.
* IPC carries typed intent, bounded results, coordination, and invalidation rather than mirroring the application object graph.
* Persistent mutations use the optimistic-consistency subsystem.
* The GUI does not gain an in-memory source of truth.
* Query windows remain bounded.
* Collapsed message-list coverage remains distinct from complete Thread membership and child Email coverage.
* Raw MIME and offline bodies remain filesystem-vault data rather than SQLite BLOB data.
* KDE/Qt Widgets remains the presentation architecture.

This plan changes ownership and dependency structure, not product semantics.

## Completion definition

The structural refactor is complete when all of the following are true:

* every production `.cpp` file has one clear owning target;
* the root CMake file primarily assembles modules rather than enumerating the whole application;
* architectural dependency violations are prevented by target visibility and linking rather than source-text regex checks;
* `javelin_jmap` is no longer one umbrella containing transport, storage, synchronization, and every JMAP product domain;
* `MailApplicationService` and `JmapCore` no longer exist as broad aggregation points;
* local cache readers, remote protocol clients, synchronization/mutation engines, application services, and GUI controllers are visibly distinct layers;
* `MainWindow` is primarily a shell/composition object rather than the owner of mail feature policy;
* adding an application operation does not normally require editing central switchboards unrelated to its domain;
* the IPC schema has explicit, reviewable wire contracts rather than depending implicitly on C++ aggregate member order;
* tests link the same production modules used by the executables rather than rebuilding arbitrary production `.cpp` files into omnibus test executables;
* mail, contacts, calendars, compose, and Sieve retain their current correctness, recovery, offline, and optimistic-consistency semantics; and
* Debug, sanitizer, static-analysis, packaging, and full functional tests pass from a clean checkout.

## Migration rules

Every phase must leave Javelin usable and releasable.

Do not perform a large directory reshuffle first. Move code only when its new ownership is established.

Do not maintain permanent parallel implementations. Transitional facades are acceptable only while callers are being migrated within a bounded phase.

Do not combine architectural migration with behavioural redesign unless the existing behaviour prevents the boundary from being expressed correctly.

Do not weaken optimistic-consistency transactions, cache fencing, mutation recovery, query-window authority, or daemon/GUI ownership to simplify refactoring.

Prefer extracting an existing coherent responsibility over inventing a generic abstraction.

Do not introduce a dependency-injection framework or service locator. Composition roots may know many concrete services; ordinary classes should receive only the capabilities they actually use.

Do not impose arbitrary source-line limits. Large classes should shrink because responsibilities move, not because code is mechanically split.

Released SQLite migrations must preserve their historical meaning.

During the current Thread materialization stabilization work, avoid simultaneously restructuring the mail synchronization path. Its new deterministic tests should become the safety contract for the later refactor.

---

# Target architecture

The desired conceptual dependency flow is:

```text
                           ┌────────────────────┐
                           │ domain/value types │
                           └─────────┬──────────┘
                                     │
                 ┌───────────────────┼───────────────────┐
                 ▼                   ▼                   ▼
          IPC contracts       cache/storage        JMAP protocol
                 │                   │                   │
                 │                   └────────┬──────────┘
                 │                            ▼
                 │                  sync/mutation engines
                 │                            │
                 └──────────────┬─────────────┘
                                ▼
                      daemon application services
                                │
                                ▼
                              javelind
```

The GUI side becomes:

```text
          IPC client ports                read-only cache
                 │                              │
                 └──────────────┬───────────────┘
                                ▼
                      presentation controllers
                                │
                                ▼
                         widgets / MainWindow
                                │
                                ▼
                              javelin
```

The terminology should have consistent meaning:

* **Value/domain type** — policy-neutral data.
* **Protocol client** — knows JMAP methods, wire semantics, capability limits, and server responses.
* **Repository** — knows persistence.
* **Reader** — exposes a read-only local projection.
* **Sync engine** — turns authoritative remote state into coherent local state.
* **Mutation engine** — owns projection, dispatch, acceptance/rejection/unknown reconciliation.
* **Application service** — interprets user/application intent and coordinates domain operations.
* **Port** — narrow interface crossing an application/process boundary.
* **Controller** — presentation coordination and user interaction.
* **Widget/view/model** — Qt presentation.

The generic name `Service` should not mean all of these at once.

---

# Phase dependency order

```text
0. Freeze the behavioural baseline
        |
1. Rebuild the CMake/module graph
        |
2. Normalize storage infrastructure and readers
        |
3. Restructure IPC contracts and transport
        |
4. Retire JmapCore
        |
5. Decompose MailApplicationService
        |
6. Normalize contacts/calendar/Sieve architecture
        |
7. Thin GUI composition and MainWindow
        |
8. Decompose large feature widgets
        |
9. Rebuild the test graph
        |
10. Final tree, naming, documentation and release gate
```

Phases 2 and 3 may overlap after phase 1.

Phases 4–6 should remain sequential around the mail synchronization path because they touch closely related ownership boundaries.

GUI refactoring can begin independently on presentation-only components, but `MainWindow` should not be comprehensively restructured until the application ports have stabilized.

---

# Phase 1: rebuild the CMake and module graph

## Purpose

Make architecture enforceable by the compiler and linker before moving large amounts of code.

This phase deliberately avoids major behavioural refactoring.

## Work

### 1. Split the root CMake file

Create subsystem CMake files, for example:

```text
src/protocol/CMakeLists.txt
src/storage/CMakeLists.txt
src/jmap/CMakeLists.txt
src/app/CMakeLists.txt
src/gui/CMakeLists.txt
tests/CMakeLists.txt
```

The root should define project-wide options/dependencies and add major subdirectories.

### 2. Establish unique implementation ownership

Every production `.cpp` must belong to exactly one production library or executable.

Resolve current overlap around cache/render implementations and calendar GUI sources.

Remove `javelin_calendar_gui` if it has no meaningful independent consumer, or make it the actual owner of those sources and link it from `javelin_gui`.

### 3. Establish initial module targets

Do not immediately create the final fine-grained graph. Introduce enough modules to make ownership meaningful:

```text
javelin_protocol_contract
javelin_protocol_local

javelin_storage_core
javelin_storage_read
javelin_storage_write

javelin_jmap_protocol
javelin_jmap_sync

javelin_daemon_app
javelin_gui_core
javelin_translation
```

Further domain modules can be extracted later.

### 4. Remove textual architecture policing where module visibility supersedes it

The current CMake source scanning has been useful transitional scaffolding.

Replace checks such as forbidden GUI includes with:

* private include directories;
* target-specific public headers;
* dependency direction;
* linker-level target isolation.

Retain small explicit checks only for invariants CMake cannot express naturally.

### 5. Separate process bootstrap from reusable application code

`gui_main.cpp` and `daemon_main.cpp` remain minimal executable composition entry points.

`GuiServices` and `DaemonServices` remain temporarily intact.

## Exit gate

* No production `.cpp` has multiple production owners.
* GUI cannot link transport/sync/write modules.
* Daemon-safe modules cannot link Widgets/WebEngine.
* Root CMake is substantially reduced.
* Current tests and application behaviour are unchanged.
* Existing regex boundary rules have either disappeared or have a documented reason to remain.

---

# Phase 2: normalize storage infrastructure and read surfaces

## Purpose

Preserve SQLite as the data plane while removing broad cache utility objects and schema change magnets.

## 2A: database infrastructure

Extract from `Database.cpp`:

```text
storage/sqlite/
    DatabaseConnection.*
    ReadOnlyDatabaseConnection.*
    DatabaseTransaction.*
    DatabaseWriteScope.*
    ConnectionFactory.*
    MigrationRunner.*
```

The exact file split may differ, but connection lifetime and migration definition must stop being one unit.

## 2B: migrations

Move migration declarations into an ordered migration module:

```text
storage/migrations/
    Migration001Initial.cpp
    ...
    Migration047ThreadMembership.cpp
```

A registry provides the ordered immutable sequence to `MigrationRunner`.

Rules:

* historical migration SQL is not cosmetically rewritten;
* migration tests prove clean creation and upgrade from selected historical versions;
* current schema creation still exercises the real migration chain unless a separately verified bootstrap snapshot is deliberately introduced later.

## 2C: split `QueryService`

Replace the broad facade progressively with focused read interfaces, for example:

```text
MailboxTreeReader
MailboxMessageReader
QueryWindowReader
ThreadReader
MailSearchReader
MailTagReader
MailboxStatisticsReader
ContactAddressReader
```

These remain lightweight views over SQLite.

Do not introduce caching inside these readers.

### Transitional strategy

Initially implement new readers using extracted SQL from `QueryService`.

Migrate callers domain by domain.

Delete the corresponding `QueryService` method immediately once the final caller moves.

Eventually remove `QueryService`.

## 2D: distinguish read/write repositories clearly

Where one repository currently serves both daemon and GUI usages, either:

* expose an explicit read interface implemented by both writable and read-only repository variants; or
* separate `FooReader` and `FooRepository`.

Do not give the GUI a type whose public API contains mutation methods even if its underlying connection rejects writes.

## Exit gate

* migration infrastructure is separate from connection infrastructure;
* no giant migration definition remains in `Database.cpp`;
* `QueryService` is gone or reduced to no meaningful responsibility;
* GUI read dependencies express only read capabilities;
* no additional in-memory state has been introduced;
* cache performance is no worse than baseline.

---

# Phase 3: restructure IPC contracts and local transport

## Purpose

Keep the existing IPC architecture while eliminating the central action switchboard and implicit aggregate wire schema.

## 3A: divide contract values by domain

Replace the single broad `ProcessBoundary.h` with shared primitives plus domain contracts:

```text
protocol/
    ProtocolTypes.h
    HandshakeContract.h
    SettingsContract.h
    CacheContract.h
    ActivationContract.h

    actions/
        AccountActions.h
        MailActions.h
        ComposeActions.h
        ContactActions.h
        CalendarActions.h
        SieveActions.h
        IdentityActions.h
        HistoryActions.h
        WorkActions.h
        DeveloperActions.h
```

Keep one stable transport envelope around these types.

## 3B: introduce typed action descriptors

Each remote operation should define, in one obvious place:

* numeric action identity;
* request type;
* result type;
* synchronous/asynchronous admission semantics;
* replay policy;
* affected cache/consistency domains;
* payload constraints where exceptional.

Conceptually:

```cpp
struct SetTagAction
{
    static constexpr auto id = ...;
    using Request = SetTagRequest;
    using Result = SetTagResult;
    static constexpr auto replay = ReplayPolicy::Never;
    static constexpr auto domains =
        domains(MailQueryWindows, MessageMetadata, History);
};
```

The exact metaprogramming mechanism should remain simple.

## 3C: derive common plumbing

Use those descriptors to remove duplicated knowledge from:

* `RemoteActionKind`;
* `RemoteApplicationPorts`;
* `DaemonRemoteActionDispatcher`;
* replay-policy switches;
* changed-domain switches.

Domain-specific application invocation still needs explicit code. Do not create opaque reflection magic that makes control flow impossible to inspect.

## 3D: make the wire schema explicit

Replace generic Boost.PFR aggregate serialization for process-boundary values.

Preferred approach:

* named fields;
* explicit schema versions;
* deterministic bounded encoding;
* Glaze or another already-supported serialization mechanism where suitable.

The build handshake may still reject incompatible versions. The goal is not indefinite backward compatibility.

The goal is that changing a struct's source member order cannot silently change the IPC format.

## 3E: split `SocketTransport.cpp`

Separate:

```text
SocketFrameCodec.*
SocketSecurity.*
LocalDaemonServer.*
LocalDaemonClient.*
LocalActivationServer.*
LocalActivationClient.*
```

Framing should not know application actions.

The endpoint should not know payload field encoding.

The client should not implement domain application policy.

## 3F: strengthen protocol tests

Add:

* malformed frame tests;
* truncated payload tests;
* oversized collection tests;
* invalid enum/action tests;
* unknown version tests;
* replay tests;
* duplicate command tests;
* asynchronous disconnect tests;
* fuzz/property tests for frame and payload decoders.

## Exit gate

Adding a new action normally requires:

1. its typed descriptor/request/result;
2. its domain handler;
3. the GUI-side port/controller call.

It must not require editing several unrelated central policy switches.

`ProcessBoundary.h`, `SocketTransport.cpp`, and `DaemonRemoteActionDispatcher.cpp` should no longer be major change magnets.

---

# Phase 4: retire `JmapCore`

## Purpose

Remove the historical all-purpose facade between application code and newer focused JMAP infrastructure.

## Target capabilities

Introduce narrow daemon-side protocol/sync capabilities such as:

```text
SessionClient
MailQueryClient
MessageContentClient
ResourceClient

MailQueryMaterializer
EmailMutationEngine
MailboxMutationEngine
```

Names may change; responsibility separation matters more.

## Work

### 1. Session operations

Move session refresh/discovery ownership to `SessionClient` plus the existing session repository.

`AccountRuntimeManager`, introduced later, will consume this capability.

### 2. Query operations

Move mailbox/search/full-window network querying into `MailQueryClient`.

Move authoritative cache-window materialization into `MailQueryMaterializer`.

A protocol query result must not itself become GUI state. Materialization remains an explicit consistency transition.

### 3. Message content

Extract content retrieval and resource download from `JmapCore`.

Separate:

* locating/fetching JMAP Email body/blob data;
* MIME/raw-source storage;
* filesystem vault work;
* application request orchestration.

### 4. Email mutations

Move queue/submit/reconcile behavior onto the existing mutation-journal/optimistic-consistency foundation.

Prefer an explicit `EmailMutationEngine` over convenience methods such as:

```text
queueArchiveEmail
queueDeleteEmail
queueMoveEmail
queueMarkEmailRead
...
```

Application policy should construct exact domain mutations. The engine should execute exact mutations.

### 5. Mailbox mutations

Do the equivalent for subscription/create/destroy and reconciliation.

### 6. Delete the facade

Once all callers have migrated, remove `JmapCore`.

Do not retain it as a convenience wrapper.

## Exit gate

No class exposes an API combining session discovery, queries, body downloads, email mutations, and mailbox lifecycle.

Daemon application services depend on narrow capabilities.

All optimistic mutation semantics and ambiguous-transport recovery remain unchanged.

---

# Phase 5: decompose `MailApplicationService`

## Purpose

Remove the largest daemon-side application aggregation point.

## Target services

The preferred structure is approximately:

### `AccountRuntimeManager`

Owns:

* per-account synchronization coordinator lifecycle;
* application of account configuration;
* session refresh;
* authentication pause/recovery;
* network reachability;
* current account status.

### `MailQueryApplicationService`

Owns:

* mailbox observation;
* mailbox query-window demand;
* search-window demand and retirement;
* Thread materialization demand;
* cooperation with `WorkScheduler`;
* publication of query/materialization cache changes.

### `MailMutationApplicationService`

Owns:

* stable selection expansion;
* authoritative selection materialization when required;
* message mutations;
* mailbox mutations;
* tag definition/deletion workflows;
* mutation submission/reconciliation;
* mail history command creation.

### `MessageContentApplicationService`

Owns:

* body materialization;
* attachment requests;
* source requests;
* content-related work scheduling/invalidation.

### `MailNotificationService`

Owns:

* notification discovery bookkeeping;
* outbox claims;
* delivery acknowledgement/release;
* publication inputs.

Contacts, calendars and Sieve migrate to their own services in phase 6.

## Migration

Move one coherent responsibility at a time.

For each extraction:

1. move its state ownership;
2. move implementation;
3. give existing callers the new dependency;
4. leave a temporary forwarding method on `MailApplicationService` only if necessary;
5. remove forwarding as soon as the caller set is migrated;
6. run focused tests;
7. run the normal full suite.

Move associated member state with behaviour. Do not create helper classes that still manipulate state owned by `MailApplicationService`.

For example, search-window request tracking belongs entirely to the query service after extraction.

Contact refresh job sets belong entirely to the contact application service.

## Account synchronization cleanup

Rename stale `LongPollService` terminology.

`AccountSyncCoordinator` is not fundamentally a long-poll component now that WebSocket/EventSource are transport choices.

Move it into an account/synchronization module with naming reflecting its actual role.

## Final step

Delete `MailApplicationService`.

Do not replace it with `ApplicationService` containing the same dependencies.

## Exit gate

No daemon application class is simultaneously responsible for mail queries, mutations, contacts, calendars, Sieve, session lifecycle, and notifications.

Each extracted service can be component-tested without constructing the entire daemon.

---

# Phase 6: normalize contacts, calendar, and Sieve

## Purpose

Give Javelin's major domains a recognizable common layering without forcing them into one generic framework.

## Calendar

Replace the current broad `CalendarService` responsibilities with:

```text
CalendarReader
CalendarProtocolClient
CalendarSyncEngine
CalendarMutationEngine
CalendarApplicationService
```

### `CalendarProtocolClient`

Knows:

* Calendar/get/set;
* CalendarEvent/get/query/changes/set;
* JMAP capability and batch limits;
* wire-format serialization and errors.

### `CalendarSyncEngine`

Knows:

* incremental state advancement;
* full recovery;
* occurrence/range materialization;
* cache reconciliation.

### `CalendarMutationEngine`

Knows:

* optimistic projection;
* mutation journal;
* server acceptance/rejection/unknown;
* projection rebase;
* authoritative recovery.

### `CalendarApplicationService`

Knows:

* rights;
* user command policy;
* visible/default/subscribed operations;
* RSVP workflows;
* history integration.

## Contacts

Split `ContactService` similarly:

```text
ContactProtocolClient
ContactSyncEngine
ContactMutationEngine
ContactMediaService
ContactApplicationService
```

Pure preparation/transform functions remain free functions or narrow policy components where appropriate.

Group membership logic should stay domain-level rather than becoming GUI policy.

## Sieve

Sieve is smaller and should not be over-engineered.

Separate protocol/mutation mechanics from user/history orchestration only where doing so materially narrows dependencies.

## Symmetry rule

Do not create generic `JmapObjectSyncEngine<T>` machinery merely because contacts and calendars share `/changes`.

Share small protocol-neutral primitives where semantics truly match.

Keep domain-specific consistency rules explicit.

## Exit gate

A developer unfamiliar with a feature can reliably determine whether a piece of logic belongs in protocol, storage, sync/mutation, application, or GUI code.

---

# Phase 7: thin GUI composition and `MainWindow`

## Purpose

Make `MainWindow` the KDE application shell instead of the central owner of mail UI behaviour.

## Target `MainWindow` responsibilities

Ultimately:

* construct the top-level shell;
* expose stable shell surfaces;
* install KXMLGUI actions;
* host workspace/tab content;
* coordinate application shutdown.

It should not implement feature policy itself.

## Extract ownership

### `MailWorkspaceController`

Own:

* mailbox/search tab lifecycle;
* active mail tab;
* list-session binding;
* mailbox activation;
* search activation;
* mail tab restoration;
* mail refresh routing.

### `QuickFilterController`

Own:

* quick-filter widgets;
* state;
* pinning;
* tag-menu integration;
* filter criteria construction;
* continuity behavior.

### `MailActionController`

Build on the existing command/action policy work and own:

* message-action availability;
* star/junk/read/tag action presentation;
* focused mail context routing.

### `AuthenticationPromptCoordinator`

Own:

* authentication-required account set;
* connection deduplication;
* prompt queue;
* reauthentication dialog sequencing.

### `ThemeController`

Own:

* dark mode action;
* palette refresh;
* palette-dependent icon updates.

### Existing controllers

Continue moving complete ownership into existing:

* `MessageCommandController`;
* `MessageContentController`;
* `MessageNavigationController`;
* `MessageSelectionController`;
* `ComposeTabController`;
* `ContactsTabController`;
* `CalendarTabController`.

If a controller only forwards calls while `MainWindow` still owns all its state, finish the extraction.

## Actions

Move toward feature-owned action groups.

`MainWindow` may register actions with KXMLGUI, but feature controllers should own:

* semantic state;
* enable/disable policy;
* trigger handling.

Avoid replacing dozens of constructor arguments with one `GuiServices&`.

Constructor shrinkage must happen because `MainWindow` needs fewer capabilities.

## Exit gate

`MainWindow` no longer directly owns quick-filter policy, authentication prompt state, message mutation policy, or individual feature workflows.

A change to mail filtering or contact behaviour normally does not touch `MainWindow.cpp`.

---

# Phase 8: decompose large feature widgets

## 8A: contacts

Break `ContactsManagerWidget` into at least:

```text
ContactsBrowser
ContactDetailsView
ContactEditor
ContactGroupController
AddressBookController
ContactPhotoController
```

Use Qt models for collections rather than rebuilding item-widget state where practical.

`ContactsManagerWidget` may remain as a feature composition widget.

## 8B: compose

Extract from `ComposeTabWidget`:

```text
ComposeRecipientController
ComposeIdentityController
SignatureController
AttachmentController
InlineImageController
ComposeAutosaveController
```

Keep short-lived editable document/widget state in the GUI.

Do not move canonical persisted draft state back into the GUI.

Heavy image processing remains asynchronous.

## 8C: message view

Split:

```text
MessageBodyPresenter
MessageAttachmentPanel
MessageBannerCoordinator
RemoteContentController
MessageTranslationController
```

The selected message snapshot remains one coherent local read.

Do not asynchronously enrich individual message fields that should have been part of the read model.

## 8D: preferences

Split the large `PreferencesDialog` into page controllers/widgets:

```text
AccountsPage
MailboxSyncPage
AppearancePage
RemoteContentPage
TranslationPage
AttachmentsPage
```

`PreferencesDialog` remains responsible for KConfig dialog integration and applying one coherent settings edit.

Translation remains the explicit GUI-local settings/cache subsystem already documented.

## Exit gate

Feature widgets primarily render state and collect user input.

Long-running work, multi-step command workflows, and feature policy are in controllers/services.

---

# Phase 9: rebuild the test graph

## Purpose

Make tests validate the actual module architecture instead of compensating for weak module boundaries.

## Work

### 1. Stop compiling arbitrary production `.cpp` files into broad test executables

Tests should normally link production libraries.

Small test-only helpers may still be compiled directly.

### 2. Create layer-focused test targets

For example:

```text
javelin_protocol_tests
javelin_storage_tests
javelin_jmap_protocol_tests
javelin_jmap_sync_tests
javelin_daemon_app_tests
javelin_gui_policy_tests
javelin_gui_widget_tests
javelin_integration_tests
```

Do not split merely for naming symmetry; use targets that improve build isolation and dependency clarity.

### 3. Retain high-value behavioural tests

Especially:

* optimistic projection/rejection/unknown/restart;
* Thread materialization;
* sparse-cache synchronization;
* cache clear/reload;
* notification outbox;
* delayed send;
* Undo/Redo;
* GUI selection stability;
* daemon disconnect/reconnect;
* read-only GUI cache enforcement.

### 4. Remove implementation-detail tests

If a refactor can change an internal helper without changing any visible contract, tests should generally not prevent it.

Prefer domain/application outcomes and persisted-state assertions.

### 5. Add architecture tests

CI should verify:

* target dependency direction;
* unique production source ownership;
* GUI has no writable database/JMAP dependencies;
* daemon has no Widgets/WebEngine;
* IPC contracts serialize deterministically;
* the production executables contain only expected layers.

## Exit gate

Changing one subsystem recompiles and runs a substantially narrower test set during normal development.

The full suite remains available as the final gate.

---

# Phase 10: final source tree and naming cleanup

Only after ownership is stable should the physical source tree be normalized.

A possible final shape is:

```text
src/
    domain/

    protocol/
        contract/
        local/

    storage/
        sqlite/
        migrations/
        mail/
        contacts/
        calendar/
        compose/

    jmap/
        protocol/
        auth/
        mail/
        contacts/
        calendar/
        identity/
        submission/
        sieve/
        sync/

    app/
        account/
        mail/
        contacts/
        calendar/
        compose/
        sieve/
        history/
        work/
        notifications/

    gui/
        shell/
        mail/
        messageview/
        compose/
        contacts/
        calendar/
        settings/
        translation/
        widgets/

    daemon/
        DaemonServices.*
        DaemonProcess.*
        main.cpp

    desktop/
        notifications/
        tray/
```

This is illustrative rather than mandatory.

The important improvement is that `src/app` no longer contains GUI-specific infrastructure and `src/jmap` no longer means everything related to storage and synchronization.

## Naming cleanup

Rename types only where the old name materially misrepresents responsibility.

Candidates include:

* `LongPollService` → account/state synchronization terminology;
* broad `FooService` names → `FooProtocolClient`, `FooSyncEngine`, `FooMutationEngine`, `FooApplicationService`, etc.;
* read-only implementations consistently ending in `Reader`;
* writable persistence consistently ending in `Repository`.

Avoid mass renaming purely for aesthetics.

## Documentation

Update:

* `ARCHITECTURE.md`;
* `DATABASE_ACCESS.md`;
* optimistic-consistency documentation;
* daemon/GUI documentation;
* development/build instructions.

Completed historical implementation plans can remain as historical records but should clearly point to the current architecture.

---

# Final release gate

Before declaring the structural programme complete:

1. Run the full Debug build/test suite.
2. Run sanitizers.
3. Run clang-tidy/clazy as appropriate.
4. Run formatting checks.
5. Build `javelin` and `javelind` from a clean tree.
6. Build packaging targets used for release.
7. Exercise daemon restart and GUI reconnect.
8. Exercise cache replacement/clear and mailbox recovery.
9. Exercise offline mailbox synchronization across restart.
10. Exercise collapsed Thread loading and interactive Thread opening.
11. Exercise mutation success, rejection, ambiguous transport failure, and restart recovery.
12. Exercise contacts and calendar delta/full recovery.
13. Exercise send, scheduled send, draft recovery, and Undo Send.
14. Exercise notification activation with the GUI closed.
15. Compare RSS, idle CPU, first-render latency, and representative query latency against phase 0.
16. Inspect the final executable dependency graph.
17. Inspect the final target/source dependency graph.
18. Perform a separate regression review specifically looking for newly introduced cross-layer shortcuts.

No meaningful performance regression is acceptable merely because the source structure is cleaner.

---

# Preferred commit strategy

The programme should consist of many independently understandable commits rather than one architecture branch landing as a monolith.

A typical extraction should look like:

```text
1. Add the new interface/module with tests.
2. Move one existing responsibility behind it.
3. Migrate callers.
4. Delete the obsolete path.
5. Tighten the build dependency so the old path cannot return.
```

Where possible, the tightening commit should immediately follow the migration.

Avoid commits whose only purpose is moving hundreds of files before their ownership changes.

Architectural progress should be monotonic: once the GUI loses access to a daemon implementation layer, it should never regain it as a convenience.

---

# Expected payoff

The primary measure of success is not smaller files. It is **locality of change**.

After the refactor, implementing a new mail operation should normally resemble:

```text
typed command/value
       |
application handler
       |
existing mutation/query capability
       |
IPC registration
       |
GUI controller/action
```

A calendar protocol change should mostly remain in calendar protocol/sync code.

A GUI filtering change should not require touching daemon orchestration.

A new IPC operation should not require modifying several giant central switch statements.

A storage schema migration should not require editing a two-thousand-line database implementation.

The project should become easier to reason about specifically because fewer components are permitted to know about each other.

The final architecture is therefore intentionally conservative: **keep Javelin's current process, persistence, consistency, protocol, and presentation model; remove the historical aggregation points that accumulated while those architectures were being built.**
