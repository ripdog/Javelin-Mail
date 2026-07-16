# Javelin Mail Development Plan

## Product Direction

Javelin Mail is a permanent-running desktop JMAP email client built with Qt Widgets, Qt 6, and C++20. The application should feel conventional in the Thunderbird sense, but the implementation should be aggressively modern:

- JMAP is the only mail protocol.
- The local SQL cache is the primary working set.
- Memory use is a design constraint, not a later optimization pass.
- The GUI consumes typed application objects, never raw wire JSON.
- Background sync and notifications remain active even when the main window is hidden.
- The client is online-first, not a full-offline local mirror.
- The tray/background lifecycle is a primary product mode, not secondary polish.

The specs in [`specs/rfc8620.txt`](/home/ripdog/CLionProjects/Javelin-Mail/specs/rfc8620.txt) and [`specs/rfc8621.txt`](/home/ripdog/CLionProjects/Javelin-Mail/specs/rfc8621.txt) are the protocol reference.

## Non-Negotiable Technical Decisions

- Use Qt 6, C++20, widgets, QCoro, and `QNetworkAccessManager`.
- Parse protocol JSON with `glaze` into typed structures.
- Use `QSqlDatabase` for persistent sync state, query results, message metadata, body fragments, and pending client actions.
- Keep the JMAP implementation in a self-contained internal library.
- Test the JMAP library with Catch2 fetched through CMake `FetchContent`.
- Wire in `clang-format`, `clang-tidy`, clazy, and AddressSanitizer early.
- Do not add legacy compatibility layers, alternate protocol adapters, or "temporary" callback-based networking paths.
- Do not add greenfield compatibility bloat such as dual config keys, alternate schema readers, or helpers that accept both old and new persisted shapes.

## Target Repository Structure

This is the intended structure after the first major refactor:

```text
.
├── AGENTS.md
├── CMakeLists.txt
├── cmake/
│   ├── Dependencies.cmake
│   ├── Sanitizers.cmake
│   ├── StaticAnalysis.cmake
│   └── Warnings.cmake
├── docs/
│   ├── DEVELOPMENT_PLAN.md
│   ├── COMPOSE_AND_SEND_PLAN.md
│   ├── ARCHITECTURE.md
│   ├── DATABASE.md
│   └── JMAP_NOTES.md
├── resources/
│   ├── icons/
│   ├── styles/
│   ├── html/
│   └── translations/
├── src/
│   ├── app/
│   │   ├── main.cpp
│   │   ├── ApplicationBootstrap.cpp
│   │   ├── ApplicationBootstrap.h
│   │   ├── ProcessServices.cpp
│   │   └── ProcessServices.h
│   ├── jmap/
│   │   ├── api/
│   │   ├── cache/
│   │   ├── domain/
│   │   ├── mail/
│   │   ├── mime/
│   │   ├── sync/
│   │   ├── submission/
│   │   └── util/
│   ├── gui/
│   │   ├── shell/
│   │   ├── mailboxes/
│   │   ├── messagelist/
│   │   ├── messageview/
│   │   ├── compose/
│   │   ├── settings/
│   │   └── tray/
│   └── shared/
│       ├── types/
│       └── logging/
├── tests/
│   ├── jmap/
│   ├── cache/
│   └── fixtures/
└── specs/
```

## Architectural Boundaries

### 1. `src/jmap`

This is the internal library. It owns:

- JMAP session discovery and capabilities.
- Authentication and token refresh.
- Request/response transport.
- Typed JSON serialization and deserialization.
- Sync state tracking.
- Local cache schema and migrations.
- Domain objects for accounts, mailboxes, threads, emails, identities, submissions, and notifications.
- Background long-poll coordination.
- Commands such as fetch, refresh, mark read, move, delete, archive, submit, and download attachment.

This library must not depend on widgets, WebEngine, or presentation-specific classes.

### 2. `src/gui`

This is the widgets application shell. It owns:

- Window layout and docking behavior.
- Models and delegates for mailbox and message presentation.
- Message rendering widgets.
- Compose widgets.
- Tray icon, menus, and window restoration.
- User preferences UI.

The GUI should talk to the JMAP library through typed objects and service interfaces such as:

- `AccountService`
- `MailboxService`
- `MessageQueryService`
- `MessageViewService`
- `SyncService`
- `NotificationService`
- `ComposeService`

Method names are illustrative; the rule is that the GUI must ask for domain operations, not craft protocol calls.

### 3. `src/app`

This is bootstrap and process orchestration:

- Application startup and shutdown.
- Dependency graph construction.
- Logging and diagnostics initialization.
- Database initialization and migration startup.
- Tray/background mode lifecycle.
- Restoring or recreating GUI state after the window has been torn down.

This layer is allowed to wire together JMAP and GUI modules, but business logic should stay out of it.

## Core Runtime Model

Use a single-process architecture first. Split the process into two logical halves:

- A lightweight always-on service core containing database access, sync orchestration, long-polling, and notification emission.
- A detachable GUI shell containing windows, WebEngine views, and heavy message presentation state.

When minimized to tray or explicitly hidden:

- Keep the service core alive.
- Destroy or aggressively quiesce heavyweight GUI widgets when safe.
- Release WebEngine pages, message DOM state, large pixmaps, and large message bodies.
- Persist UI restoration state so the shell can be recreated without losing the user’s place.

Do not attempt a separate helper process until the single-process design is stable and memory profiles justify the added complexity.

## Data Model And Cache Strategy

The local database should be authoritative for the client working set. Target tables should include at least:

- `accounts`
- `sessions`
- `mailboxes`
- `threads`
- `emails`
- `email_mailboxes`
- `email_keywords`
- `email_addresses`
- `email_body_values`
- `email_parts`
- `identities`
- `submissions`
- `sync_state`
- `mutation_journal`
- `notifications`
- `settings`
- `schema_migrations`

Design rules:

- Persist server IDs and relevant JMAP state tokens per account and object type.
- Treat `blobId` as a first-class identifier for body parts and attachment metadata.
- Separate lightweight list metadata from large body payloads.
- Store normalized address lists rather than duplicating display strings everywhere.
- Model pending local actions explicitly so transient offline or retry behavior is recoverable.
- Version schema migrations from the start.
- Prefer append/update reconciliation over destructive cache invalidation.
- Design the cache around a synced working set and durable client state, not around a complete offline replica of the user’s corpus.
- Cache canonical text and HTML body payloads locally.
- Persist MIME structure and part metadata for all message parts, even when the binary payload is not cached.
- Do not cache ordinary downloadable attachment payloads by default.
- Allow a narrow cache exception for inline render-required parts, such as images referenced by the HTML body through `cid:` or equivalent message-local references.
- Keep render-required inline part caching aggressively scoped and evictable.

Memory policy:

- Message list models should hold IDs and small summaries only.
- Full message bodies should be loaded on selection and released when no longer needed.
- Avoid retaining decoded MIME trees globally.
- Avoid retaining HTML strings, rendered DOM state, or attachment thumbnails outside active views.
- Query windows should be page-based and reload from SQL cheaply.

## Network And Sync Design

Implement transport around `QNetworkAccessManager` plus QCoro coroutines:

- Session bootstrap.
- Batched method calls.
- Error mapping from transport/protocol failures into typed domain errors.
- Long-poll loop with clean cancellation and backoff policy.
- Incremental sync using state tokens.

Expected subsystems:

- `SessionClient`
- `MethodCaller`
- `LongPollWorker`
- `SyncPlanner`
- `SyncExecutor`
- `DownloadManager`
- `UploadManager`

Design requirements:

- One authenticated account context per configured account.
- Capability negotiation is mandatory. Feature use must be gated by the advertised JMAP capabilities and account capabilities.
- All JMAP calls flow through typed request/response structs.
- Long-poll updates should enqueue narrowly scoped refresh work rather than full cache rebuilds.
- Rate limit retries and treat auth failures distinctly from connectivity failures.
- Persist sync cursors so restart does not force unnecessary full refreshes.
- OAuth 2 token refresh and secret storage must be designed in from the start.

## Message Rendering Design

### Plain Text

- Use a lightweight text view path for text-only mail.
- Preserve quoting, flowed text, monospace regions, and link detection.
- Avoid pushing plain text through the HTML renderer unless required for a deliberate rich-text presentation feature.

### HTML

Use Qt WebEngine for HTML mail with a dedicated message-view pipeline:

- Treat all HTML mail as hostile content by default.
- Load sanitized stored HTML, not live remote content.
- Keep canonical stored HTML separate from the derived render document used by WebEngine.
- Intercept and block remote requests by default.
- Allow per-message remote content enablement without forcing a full WebEngine reload if technically possible.
- Inject dark-mode CSS and content policy helpers after document load.
- Keep CSS/script injection localized to the view layer rather than mutating stored source.
- Track language detection and translation state separately from original content.
- Treat sanitization, remote-resource policy, dark-mode adaptation, and translation hooks as core architecture, not late UI polish.
- Rewrite inline message-local references in the derived render document to internal application URLs instead of embedding arbitrary fetched payloads directly into canonical content.

Subsystems to plan:

- `HtmlMessageDocumentBuilder`
- `RemoteContentPolicy`
- `WebEngineRequestInterceptor`
- `DarkModeStyleInjector`
- `LanguageDetectionService`
- `TranslationService`

Acceptance direction:

- Remote images and trackers are blocked by default.
- Enabling remote content is explicit and reversible.
- Dark mode remains readable across common mail HTML.
- Translation can be offered without corrupting stored original content.
- Inline images referenced by the message body render through controlled local resolution without requiring general attachment caching.

## UI Structure

Default layout is a three-column desktop mail client:

- Left: mailbox/account tree.
- Center: message list.
- Right: selected message view.

Alternative layouts to support later:

- Stacked list over message view.
- Detached message viewer window.
- Restorable pane sizes and visibility.
- Aggressive release of heavyweight hidden-window UI state while preserving fast restoration.

Planned GUI modules:

- `MainWindow`
- `MailboxTreeModel`
- `MailboxTreeView`
- `MessageListModel`
- `MessageListView`
- `MessageListDelegate`
- `MessageViewContainer`
- `PlainTextMessageView`
- `HtmlMessageView`
- `MessageWindow`
- `TrayController`
- `NotificationCenter`
- `SettingsDialog`

Model/view rules:

- Use custom `QAbstractItemModel` implementations backed by typed cache queries.
- Do not use `QTreeWidget`, `QTableWidget`, or other item-based convenience widgets for large mail data.
- Keep sorting, filtering, and paging close to the database/query layer.
- Expose stable IDs from models so selection survives refreshes.
- Time-based grouping is a first-class list feature. Group by the effective list timestamp and keep buckets stable across refreshes and pagination.
- Support recent-day grouping and coarser grouping for older periods without coupling the grouping logic to a single view layout.

## Compose And Submission

This app is intended to be comprehensive, so composition cannot be treated as a late optional extra. Plan for:

- Draft creation and autosave.
- Identity selection.
- Reply, reply-all, forward.
- Attachment upload.
- Submission progress and failure recovery.
- Outbox/pending actions backed by the local database.

Keep compose state typed and persistent enough that window closure or process restart does not silently lose user work.

## Authentication And Secret Storage

Authentication should be treated as architecture, not setup glue:

- Prefer OAuth 2 flows supported by the target JMAP server.
- Store refresh/access tokens via OS-backed secret storage where available.
- Keep account metadata and secret material clearly separated.
- Design explicit re-auth and token-expiry recovery flows.
- Do not leak secrets into logs, crash output, or diagnostic views.

## Threading And Database Ownership

`QSqlDatabase` requires deliberate thread ownership rules:

- Define database connection lifetime and thread affinity early.
- Do not share a single connection object across threads.
- Keep SQL access behind repositories/services with explicit execution context.
- Make cross-thread work submission visible in the design rather than hidden in utility helpers.

If background sync and GUI both need DB access, give them clear connection management rules from the start.

## Observability And Diagnostics

JMAP sync and cache bugs are difficult to reason about without good diagnostics. Plan for:

- Structured logging categories for auth, transport, sync, cache, rendering, and notifications.
- Correlation IDs or request tracing for JMAP method calls where practical.
- Sync counters and timing metrics for key operations.
- A developer-facing diagnostics view or log export path.
- Careful redaction of personal data and secrets.

## Privacy And External Services

Language detection and translation need explicit privacy design:

- Local-only detection is preferred where practical.
- Any remote translation service must be opt-in and clearly disclosed.
- Original message content must remain intact and separately accessible.
- Translation state should be transient or separately stored, not mixed into canonical cached content.

## Static Analysis, Tooling, And CI

Stage in tooling immediately:

- `.clang-format`
- `.clang-tidy`
- CMake options for ASan and UBSan in debug builds where practical
- clazy target or documented invocation
- warnings-as-errors policy for project code
- `CMAKE_EXPORT_COMPILE_COMMANDS`

Expected developer workflows:

- Format before commit.
- Run targeted Catch2 tests for touched JMAP/cache code.
- Run `clang-tidy` on modified translation units.
- Run clazy before large Qt-heavy changes land.
- Use ASan during integration-heavy work such as message rendering, SQL lifetime changes, and background teardown/rebuild logic.

## Multi-Stage Delivery Plan

### Stage 0: Repository Rebase And Build Scaffold

Goals:

- Replace the starter single-window sample structure.
- Introduce target directories and CMake modules.
- Add foundational analysis and warning configuration.

Tasks:

- [x] Move the current executable entrypoint into `src/app`.
- [x] Split targets into at least `javelin_jmap`, `javelin_gui`, and `Javelin-Mail`.
- [x] Add `cmake/Warnings.cmake`, `cmake/StaticAnalysis.cmake`, and `cmake/Sanitizers.cmake`.
- [x] Add `.clang-format` and `.clang-tidy`.
- [x] Add CMake presets for debug, ASan, and release.
- [x] Fetch Catch2 with `FetchContent`.
- [x] Add an initial `tests/jmap` Catch2 target and verify it runs in the debug preset.

Exit criteria:

- Clean configure/build on a supported Qt 6 toolchain.
- Static analysis hooks are documented and callable.
- Repository structure reflects the intended architecture.

### Stage 1: Domain Types And Transport Foundation

Goals:

- Create typed JMAP request/response infrastructure.
- Establish account/session bootstrap.

Tasks:

- Define domain and protocol structs for session discovery, capabilities, accounts, mailboxes, threads, emails, identities, and errors.
- Implement glaze adapters and protocol tests using stored JSON fixtures.
- Build a coroutine-based transport wrapper over `QNetworkAccessManager`.
- Add typed error surfaces for network, auth, and protocol failures.
- Define capability negotiation surfaces and fail-fast unsupported-feature handling.
- Define auth/session abstractions with token refresh and secret-storage integration points.

Completed so far:

- [x] Add typed session, account, and capability structs for the first session-discovery slice.
- [x] Centralize capability validation and fail-fast requirement checks in the JMAP library.
- [x] Add Glaze-backed session parsing and fixture-based tests for the session bootstrap shape.
- [x] Add typed auth, transport, and protocol error surfaces plus token/secret-store abstractions.
- [x] Add a coroutine-based session discovery client over a testable transport interface using `QNetworkAccessManager` and QCoro.
- [x] Add typed mailbox, thread, email, and identity entities with fixture-based parsing tests.
- [x] Add typed JMAP request/response envelope parsing for invocation tuples and batched method calls.
- [x] Add a reusable batched method caller with typed envelope serialization, auth refresh, and response parsing.

Exit criteria:

- The library can perform session discovery and parse responses into typed objects.
- JSON fixture tests pass.
- No GUI code knows about wire JSON.
- Capability checks are enforced centrally rather than scattered across features.

### Stage 2: Database Schema, Migrations, And Cache Services

Goals:

- Establish the local cache as the durable state layer.

Tasks:

- Define schema versioning and migration runner.
- Implement repositories for accounts, mailboxes, threads, emails, bodies, and sync state.
- Add blob and attachment storage strategy keyed by `blobId`.
- Define MIME-part metadata storage and a policy that excludes ordinary attachment payload caching.
- Add query APIs shaped for mailbox tree and message list consumption.
- Decide WAL, pragma, and index strategy for a long-running desktop client.
- Define thread ownership and connection policy for database access.

Completed so far:

- [x] Add a SQLite cache connection boundary with per-thread named connections and explicit close semantics.
- [x] Add transactional schema versioning and an initial migration runner for the cache schema.
- [x] Apply the initial WAL/pragma policy at open time and cover fresh-create plus reopen behavior with tests.
- [x] Add the first cache repository for persisted sync state tokens with deterministic round-trip tests.
- [x] Add a mailbox cache repository with replacement semantics and parent-scoped mailbox queries.
- [x] Add a cached session bootstrap repository that round-trips typed session and account metadata.
- [x] Add a thread cache repository that preserves ordered email membership per thread.
- [x] Add an email summary cache repository that normalizes mailbox, keyword, and address rows.
- [x] Add MIME-part metadata and canonical body-value repositories without general attachment payload caching.
- [x] Add SQL-backed mailbox-tree and paged message-list query APIs shaped for GUI consumption.
- [x] Add an explicit thread-scoped database connection factory so cache access keeps thread ownership visible in the API.
- [x] Lock in the initial long-running SQLite policy with WAL pragmas and query-plan regression tests for mailbox tree and message list indexes.

Exit criteria:

- Fresh database creation works.
- Schema migrations are repeatable.
- Cache repositories are unit-tested and can round-trip representative data.

### Stage 3: Incremental Sync Engine

Goals:

- Build minimal-refresh sync rather than naive full reloads.

Tasks:

- Implement sync planners keyed by JMAP state tokens.
- Reconcile mailbox and email updates into SQL.
- Add pending-action merging rules so local changes survive concurrent server updates.
- Add resumable long-poll loop with cancellation and structured backoff.

Completed so far:

- [x] Add a typed sync planner that maps persisted sync-state tokens to initial-fetch versus incremental-changes work.
- [x] Add typed `Mailbox/*` and `Email/*` sync method payloads so incremental sync code can avoid raw method-argument JSON.
- [x] Reconcile mailbox and email change batches into SQL with targeted upsert/delete operations and persisted new state tokens.
- [x] Add typed pending email patch actions and merge rules so local mailbox/keyword edits survive concurrent server refreshes.
- [x] Add a resumable long-poll worker with explicit cancellation and structured retry backoff.
- [x] Add a minimal live bootstrap path that discovers a real session, caches accounts and mailboxes, and hydrates an initial mailbox window for end-to-end testing against a live server.

Exit criteria:

- Initial sync populates the cache.
- Long-poll updates only touch changed entities.
- Restart resumes from persisted state without a forced full refresh unless required.

### Stage 4: Mailbox And Message Query Models

Goals:

- Provide scalable read paths for the GUI.

Tasks:

- Implement mailbox tree queries from SQL.
- Implement paged message list queries by mailbox and sort order.
- Expose stable selection keys and incremental refresh notifications.
- Keep list presentation data compact.
- Implement time-based grouping for recent and older messages in a way that survives pagination and refresh.

Completed so far:

- [x] Add SQL-backed mailbox tree and paged message list queries with compact row shapes for GUI models.
- [x] Add stable mailbox/message selection keys and diff-based incremental refresh notifications for cache-backed query snapshots.
- [x] Add deterministic time-based message grouping with day buckets for recent mail and month buckets for older mail.

Exit criteria:

- Large folders do not require full in-memory message materialization.
- Switching folders and restoring selections operate from cache efficiently.
- Group headers remain stable and predictable while the underlying query window updates.

### Stage 5: Main Window And Three-Pane Shell

Goals:

- Replace the placeholder UI with a durable shell.

Tasks:

- Build `MainWindow`, mailbox pane, message list pane, and message view container.
- Add splitters, layout persistence, and account selection.
- Connect GUI models to cache-backed services.
- Add placeholder empty/error/loading states that do not depend on ad hoc widget mutation.

Completed so far:

- [x] Replace the starter shell with `QAbstractItemModel`-backed mailbox/message panes wired to cache query services and basic empty-state handling.
- [x] Add cached account selection plumbing so the main shell can switch mailbox and message queries between persisted JMAP accounts.
- [x] Persist main-window geometry, pane sizes, and selected account so the three-pane shell restores its last layout cleanly.
- [x] Replace the placeholder right pane with a dedicated message-view container that tracks account, mailbox, and message selection states explicitly.
- [x] Add a basic preferences dialog for manual testing so session URL, login email, and API key can be entered from the running app.
- [x] Add a menu-driven refresh action that uses saved connection settings to populate the cache from a live JMAP server without blocking the GUI thread.
- [x] Add mailbox-scoped live fetch on mailbox selection so non-Inbox folders can be exercised against a real server without restarting the app.

Exit criteria:

- The app can browse synced mail from the local cache.
- The shell remains responsive during background refresh.

### Stage 6: Message Viewing Pipeline

Goals:

- Render text and HTML mail safely with low steady-state memory use.

Tasks:

- Make mailbox message lists thread-backed, using collapsed thread queries and thread-member fetches rather than flat per-message windows.
- Implement plain-text view and HTML view switching.
- Add HTML sanitization/storage pipeline.
- Build a derived render-document pipeline that rewrites `cid:` and similar inline references to internal application URLs.
- Add remote-content blocking and allow-list enablement.
- Add an explicit script-removal and inline-event-handler stripping pass before any document-side interactivity is allowed.
- Add dark-mode CSS injection.
- Add attachment metadata display and download/open flows.
- Implement on-demand fetch for normal attachments and narrow caching for render-required inline parts only.
- Investigate language detection and translation hooks.

Completed so far:

- [x] Add a typed cache-backed message-view snapshot service that loads selected-message headers, canonical body values, and attachment metadata from the local store.
- [x] Wire the message-view container to cached selected-message data with explicit plain-text and HTML-source switching plus lightweight attachment metadata display.
- [x] Add an on-demand `Email/get` content fetch path that caches selected-message body sections and attachment metadata when the local store does not have them yet.

Exit criteria:

- Plain-text and HTML mail render through separate paths.
- Remote content is blocked by default.
- Leaving a message releases heavyweight view state.

### Stage 7: Notifications, Tray, And Background Lifecycle

Goals:

- Make the app useful when permanently running.

Tasks:

- Add tray icon, menu, and notification presentation.
- Keep sync core alive when the main window is hidden.
- Destroy or suspend heavyweight UI resources when appropriate.
- Recreate the window from persisted state cleanly.

Exit criteria:

- New mail notifications work while the window is hidden.
- Background mode does not retain full GUI state unnecessarily.

### Stage 8: Server Search And Discovery

Goals:

- Use JMAP server-side search for corpus-wide discovery instead of building local full-text indexing.

Status:

- Implemented with generic paged tabs for both mailbox and search contexts.
- Search tabs persist their cached page locally for immediate restore, then refresh from the server on launch.

Tasks:

- Implement typed search query building for the supported JMAP mail query features.
- Integrate remote search results into the existing message list/view flow.
- Keep search result windows cache-aware without turning the cache into a full offline mirror.
- Define failure and partial-results behavior for slow or unavailable network conditions.

Exit criteria:

- Users can search their full mailbox corpus through the server.
- Search integrates cleanly with paging, selection, and message grouping.

### Stage 9: Compose, Drafts, And Submission

Goals:

- Deliver full client functionality, not just read-only browsing.

Tasks:

- Add compose window and draft persistence.
- Implement reply/reply-all/forward flows.
- Implement attachment upload pipeline.
- Add pending submission recovery and user-visible retry states.

Exit criteria:

- Drafts survive restarts.
- Submission errors are recoverable and observable.

### Stage 10: Filtering, Navigation, And Productivity Features

Goals:

- Move toward a comprehensive daily-driver client.

Tasks:

- Add unread, flagged, attachment, and sender filters.
- Add keyboard navigation and command palette style actions where useful.
- Add multi-account polish and settings coverage.

Exit criteria:

- Common navigation and triage flows are fast from cache.

### Stage 11: Hardening And Memory Profiling

Goals:

- Prove the app is stable as a permanent background client.

Tasks:

- Add long-run sync tests and teardown/rebuild tests.
- Measure steady-state memory with window shown, hidden, and recreated.
- Audit SQL query counts for common mailbox and message operations.
- Audit WebEngine lifetime and verify message switches do not leak memory.
- Review clazy, clang-tidy, and sanitizer findings regularly until clean.

Exit criteria:

- Hidden/tray mode memory profile is acceptably low.
- The client survives long-poll, reconnect, and window recreation cycles.


## Risks To Manage Early

- WebEngine memory behavior may dominate hidden-window footprint if views are not explicitly torn down.
- Remote-content enablement without a full refresh may require careful WebEngine injection and request interception design.
- JMAP incremental sync correctness depends on precise state-token handling; careless shortcuts here will create hard-to-debug cache divergence.
- SQL query design can easily become the bottleneck if list models depend on chatty per-row lookups.
- Compose/draft persistence becomes expensive to retrofit if read-only assumptions leak into the cache schema.
- Capability handling will become error-prone if each feature performs its own ad hoc checks.
- Token refresh and secret storage are expensive to retrofit once account flows have been scattered through the UI.

## Documentation To Add After Stage 1

Create and maintain these docs once the corresponding code exists:

- `docs/ARCHITECTURE.md`: module boundaries, process lifecycle, service graph.
- `docs/ARCHITECTURE_DECISIONS.md`: key irreversible design choices and their rationale.
- `docs/DATABASE.md`: schema, indices, migration policy, cache eviction rules.
- `docs/JMAP_NOTES.md`: supported method groups, sync strategy, known server assumptions.
- `docs/RENDERING.md`: HTML sanitization, remote-content policy, dark mode injection, translation flow.
