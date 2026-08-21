# Javelin Mail development roadmap

## Purpose and authority

This document is the forward-looking product and engineering roadmap for the current Javelin
codebase. It is not a build guide or architecture specification. Build-environment setup and
validation commands live in [DEVELOPMENT.md](DEVELOPMENT.md). Current invariants are defined by
[ARCHITECTURE.md](ARCHITECTURE.md),
[DAEMON_GUI_ARCHITECTURE.md](DAEMON_GUI_ARCHITECTURE.md), and the focused subsystem documents linked
from them.

The original greenfield plan has been completed far enough that its single-process staging model and
initial repository sketch are no longer useful guidance. Javelin now ships as a split `javelin` GUI
and `javelind` daemon, with the daemon as the sole operational authority and SQLite writer.

## Product direction

Javelin is a modern desktop JMAP client with a conventional mail-client interaction model and a
strictly typed, cache-backed implementation. It should remain:

- JMAP-only rather than accumulating protocol compatibility layers;
- responsive under large mailboxes and long-running synchronization;
- useful with the GUI closed, through daemon-owned push, notifications, delayed send, and tray
  controls;
- online-first by default, with explicit complete-offline mailbox mirrors;
- conservative about user intent: refreshes must not move selection, replace the viewed object, steal
  focus, or overwrite newer editor state;
- capable across mail, submission, contacts, calendars, and Sieve when the server advertises the
  corresponding capabilities; and
- maintainable through narrow typed boundaries rather than feature-specific shortcuts.

## Current baseline

The current runtime and build graph provide:

- separate `javelin` and `javelind` executables connected by the framed local `JVIP` protocol;
- a GUI restricted to typed commands and read-only cache access;
- daemon-owned JMAP transports, synchronization, mutations, settings, notifications, tray,
  background tasks, Undo/Redo, and delayed send;
- HTTP JMAP plus RFC 8887 WebSocket method transport and push, with safe HTTP/EventSource fallback;
- authoritative mailbox and search query windows, optimistic mutation projection, and stable-ID UI
  restoration;
- explicit complete-offline mailboxes backed by a filesystem MIME vault and rebuildable FTS5 search
  indexes;
- HTML and plain-text message rendering, controlled remote content, optional translation, and local
  language detection;
- rich/plain compose, drafts, attachments, delayed send, reply/forward, and server submission;
- JSContact address books, contacts, groups, photos, import/export, duplicate detection, and sharing
  operations;
- JMAP Calendars support against draft-ietf-jmap-calendars-26 and JSCalendar; and
- deterministic Catch2 coverage across protocol, cache, synchronization, application, process, and
  presentation layers.

The daemon/GUI implementation history and remaining release validation are recorded in
[DAEMON_GUI_IMPLEMENTATION_PLAN.md](DAEMON_GUI_IMPLEMENTATION_PLAN.md).

## Non-negotiable engineering rules

- Target the CI-tested minimum stack directly: Qt 6.10+, KDE Frameworks 6.27+, KDE PIM 6.8+, and C++23.
- Keep wire JSON and JMAP method names inside `javelin_jmap`.
- Use Glaze for typed protocol serialization and QCoro for asynchronous Qt networking.
- Keep the GUI operationally read-only; all persistent commands and settings updates cross IPC.
- Keep the daemon free of Widgets and WebEngine.
- Preserve the optimistic-consistency, query-window, database-transaction, and user-intent invariants.
- Do not add alternate mail protocols, callback-based networking, dual config readers, or permanent
  migration adapters.
- Prefer deleting obsolete paths and reducing special cases over layering new abstractions around
  them.
- Keep foreground work responsive without weakening transaction atomicity or server ambiguity rules.

## Repository shape

```text
src/protocol/   Process-boundary values, codecs, framing, local socket transport
src/app/        GUI/daemon composition, application ports, coordination, tasks, history
src/jmap/       Typed JMAP, cache repositories, sync, mail, contacts, calendars, submission
src/gui/        Qt Widgets/WebEngine presentation and interaction controllers
tests/          Protocol, application, JMAP/cache, process, and GUI policy tests
docs/           Architecture, subsystem invariants, implementation plans, diagnostics
specs/          Vendored normative protocol texts
res/            Desktop integration, icons, translation and language assets
```

The important boundaries are CMake targets rather than directory names: `src/app` contains both
composition roots, but `javelin_daemon_core` and the GUI bootstrap link disjoint operational
surfaces.

## Roadmap

### 1. Close the split-process release gate

The architecture is implemented; the remaining work is release validation and tuning rather than a
new process migration.

- Complete the crash/reconnect matrix for daemon failure before admission, after local commit, during
  remote dispatch, and during cache replacement.
- Run optimized and sanitizer suites regularly, not only the Debug suite.
- Measure cached GUI startup, common command admission, rapid navigation, daemon idle RSS/CPU wakeups,
  WAL growth, and memory released when the GUI exits.
- Investigate regressions in scheduling, batching, indexes, invalidation detail, and model diffs before
  considering architectural exceptions.
- Verify installed service startup, singleton activation, tray actions, notification activation, and
  upgrades from representative existing profiles.

See [DAEMON_GUI_IMPLEMENTATION_PLAN.md](DAEMON_GUI_IMPLEMENTATION_PLAN.md) and
[DIAGNOSTICS.md](DIAGNOSTICS.md).

### 2. Finish calendar correctness and product completeness

Calendar work remains correctness-first because recurrence and synchronization bugs can silently alter
or hide user data.

- Complete occurrence-versus-series editing and detached recurrence handling.
- Finish incremental Calendar and CalendarEvent synchronization, including robust
  `cannotCalculateChanges` recovery.
- Bound cached occurrence windows and coordinate overlapping visible-range requests.
- Complete month-view interaction, reminder behavior, calendar management, permissions, and
  interoperability fixtures.
- Keep support pinned to the reviewed draft until a later JMAP Calendars revision receives an
  explicit protocol migration.

See [CALENDAR_IMPLEMENTATION_PLAN.md](CALENDAR_IMPLEMENTATION_PLAN.md).

### 3. Harden compose, submission, and history workflows

The compose surface is functional, but send-related paths deserve continued adversarial testing.

- Verify draft revision fencing and attachment-manifest identity under reconnect and delayed send.
- Exercise large attachments, failed staging, source-file replacement, retry, and daemon restart.
- Keep Undo Send cancellation available from daemon-owned notifications and history only while the
  deferred submission is genuinely cancellable.
- Expand compatibility fixtures for HTML normalization, inline images, reply quoting, forwarding, and
  unusual MIME structures.
- Ensure every new stateful operation has a deterministic inverse or an explicit impossible/expired
  history entry.

See [COMPOSE_AND_SEND_PLAN.md](COMPOSE_AND_SEND_PLAN.md) and [UNDO_REDO.md](UNDO_REDO.md).

### 4. Continue mail UX, offline, and search refinement

- Prefer precise model changes over resets and expand stable-selection tests for every remaining list
  and tree surface.
- Keep query-window and full-offline paths unified so background materialization is immediately useful
  to the visible UI.
- Improve Task Center visibility and cancellation for expensive full-sync, indexing, and maintenance
  jobs without surfacing routine internal housekeeping.
- Continue profiling very large mailboxes, sparse jumps, local/server search merging, MIME parsing,
  and vault eviction.
- Preserve foreground priority and the post-foreground quiet period for all new background jobs.
- Build cross-server Move/Copy and portable mail export on one shared exact-scope/raw-RFC-5322
  materialization layer rather than duplicating mailbox enumeration or whole-message download paths.

See [OFFLINE_MAIL_ARCHITECTURE.md](OFFLINE_MAIL_ARCHITECTURE.md),
[QUERY_WINDOWS.md](QUERY_WINDOWS.md), [MAILBOX_REFRESH_PLAN.md](MAILBOX_REFRESH_PLAN.md),
[CROSS_SERVER_MAIL_TRANSFER_IMPLEMENTATION_PLAN.md](CROSS_SERVER_MAIL_TRANSFER_IMPLEMENTATION_PLAN.md),
[MAIL_EXPORT_IMPLEMENTATION_PLAN.md](MAIL_EXPORT_IMPLEMENTATION_PLAN.md), and the incremental
[UX and KDE integration checklist](UX_KDE_INTEGRATION_CHECKLIST.md).

### 5. Packaging, platform support, and profile security

Linux with a systemd user session and StatusNotifierItem host is the current first-class platform.
CMake should remain portable, but other platforms should not be advertised as supported until their
lifecycle, notification, tray, and peer-authentication paths are exercised.

- Keep the Arch package and ordinary CMake install paths reproducible.
- Add continuous packaging/install smoke tests, including service discovery from the installed
  prefix.
- Document and test profile/cache recovery and the distinction between replaceable cache data and
  daemon-owned settings.
- Keep long-lived account credentials in the existing KWallet-backed credential store and verify its
  installed/runtime behavior before treating broad end-user distribution as complete.
- Add release metadata, a top-level licence file, screenshots, and contribution templates when the
  repository is prepared for wider public consumption.

### 6. Quality and maintainability

- Keep the Debug preset, Catch2 suite, format check, clang-tidy, clazy, ASan, and UBSan operational.
- Add tests at the narrowest stable boundary: pure policy first, then repository/service, then process
  integration only where the behavior crosses a process boundary.
- Reject direct cache writes outside coordinated transactions and optimistic adapters.
- Keep documentation synchronized when ownership or invariants change.
- Regularly remove dead migration code, stale plans, duplicate orchestration, and GUI knowledge of
  operational policy.

## Definition of a release-ready change

A feature or refactor is ready when:

- its ownership matches the process and module boundaries;
- it does not introduce a second source of visible or optimistic state;
- transport ambiguity, partial success, cancellation, restart, and stale completion are classified;
- cache changes cannot clobber stable user intent;
- background work is bounded and yields to foreground work;
- deterministic tests cover success and the meaningful failure/recovery paths;
- diagnostics contain actionable context without credentials or message content; and
- affected architecture and user documentation are updated in the same change.
