# Developer Options Dialog Design

## Status

Proposed design. This document defines the user experience, data ownership, asynchronous execution,
cache accounting, destructive-operation semantics, and an implementation sequence for a Developer
Options dialog.

## Goals

The dialog should make Javelin's local state inspectable and repairable without requiring direct
SQLite queries or filesystem surgery.

The initial implementation must:

- list every cached mail account and mailbox, including raw JMAP identifiers;
- expose all mailbox metadata and relevant derived cache/synchronization state currently stored;
- calculate mailbox cache usage without blocking the GUI thread;
- report SQLite-backed metadata separately from raw message-body storage;
- clear either part independently and safely;
- list other rebuildable caches, beginning with translated-text results;
- allow those caches to be measured and cleared;
- preserve the GUI/daemon boundary: the GUI requests operations, while the owning process performs
  them;
- explain shared and estimated storage rather than presenting misleading exact totals;
- leave room for additional developer diagnostics and repair actions.

## Non-goals

The first version should not provide:

- a general SQL console;
- a filesystem browser embedded in the application;
- editing of raw cached records;
- direct construction of JMAP calls in the GUI;
- deletion of credentials, account configuration, drafts, downloaded translation models, or other
  user-owned data under the label "clear cache";
- an automatic network refresh after every inspection operation;
- an exact attribution of physical SQLite pages to individual mailboxes, because the current schema
  stores all mailboxes in shared tables and indexes.

## Entry Point and Lifetime

Add **Developer Options…** to the Settings menu. It should be available in normal builds rather than
behind a command-line switch: these operations are useful for diagnosis and support, and hiding them
would make recovery harder.

Use a standalone, resizable `QDialog`, not a page in `PreferencesDialog`. Preferences are applied as
configuration, whereas this dialog performs immediate inspection and maintenance operations. The
window should remember its size, splitter position, selected page, and table column widths.

The dialog may remain open while work runs. Closing it must not cancel destructive work after commit
has started. Read-only scans may be cancelled when the last interested UI closes.

## Top-level Layout

Use a `KPageDialog` or equivalent sidebar navigation with these pages:

1. **Mailbox Caches**
2. **Other Caches**
3. **Diagnostics**

The default page is Mailbox Caches.

A persistent footer contains:

- the current operation and progress;
- a Cancel button for cancellable scans;
- a link/button to open the existing Task Center when an operation has become daemon-managed work;
- Close.

Do not use a modal progress dialog. Long scans should leave the rest of the Developer Options dialog
usable where doing so is safe.

A representative Mailbox Caches layout is:

```text
┌ Developer Options ─────────────────────────────────────────────────────────┐
│ Mailbox Caches                                                             │
│ Other Caches     ┌──────────────────────┬─────────────────────────────────┐ │
│ Diagnostics      │ Mailbox              │ Inbox                           │ │
│                  │  Fastmail            │ Identity                        │ │
│                  │   Inbox     18 MiB…   │  Account ID  c145…       Copy   │ │
│                  │   Archive     —       │  Mailbox ID  a9f…        Copy   │ │
│                  │   Sent        —       │  Role        inbox              │ │
│                  │  Stalwart            │                                 │ │
│                  │   Inbox       —       │ Stored state                    │ │
│                  │                      │  Windows     4                   │ │
│                  │                      │  Offline     Complete            │ │
│                  │                      │                                 │ │
│                  │                      │ Cache usage                     │ │
│                  │                      │  SQLite     18 MiB estimated     │ │
│                  │                      │             [Clear SQLite…]      │ │
│                  │                      │  Bodies    612 MiB logical       │ │
│                  │                      │            438 MiB reclaimable   │ │
│                  │                      │             [Clear bodies…]      │ │
│                  └──────────────────────┴─────────────────────────────────┘ │
│ Calculating Inbox bodies…  2,431 / 3,842 files        [Cancel] [Close]     │
└────────────────────────────────────────────────────────────────────────────┘
```

The sketch is illustrative rather than a requirement to use card-like visual containers. Prefer
standard KDE group boxes, separators, tables, and form layouts over decorative rounded panels.

# Mailbox Caches Page

## Overview

The page uses a horizontal splitter:

- left: account/mailbox tree and storage summary columns;
- right: detailed properties and maintenance controls for the selected mailbox.

### Mailbox tree

Use a `QAbstractItemModel`-backed `QTreeView`, grouped as:

```text
Account display name / login address
  Inbox
  Archive
  Projects
  …
```

The account header should show both the user-recognizable connection/account name and the JMAP
account ID in its details, but not use the ID as the primary label.

Recommended columns:

| Column | Meaning |
| --- | --- |
| Mailbox | Display name, role icon, and hierarchy |
| SQLite | Estimated mailbox-attributable SQLite data |
| Bodies | Logical raw-message bytes available for this mailbox |
| Reclaimable | Raw-message bytes expected to become physically reclaimable if this mailbox's body cache is cleared |
| Offline | Offline mirror state |
| Measured | Time of the most recent measurement |

On first open, sizes show **Not calculated**. Selecting a mailbox starts its measurement. A toolbar
provides:

- **Calculate selected**;
- **Calculate all**;
- **Cancel calculation**;
- **Refresh metadata**;
- a text filter matching mailbox name, role, account name, and raw IDs.

`Calculate all` should process mailboxes sequentially by default. Parallel directory walks create
unnecessary random I/O and make the desktop less responsive. A single low-priority I/O worker is
sufficient initially.

Rows update progressively as each result arrives. The dialog must never wait for the full account
scan before displaying partial results.

## Mailbox detail panel

Use three vertically stacked sections: Identity, Stored State, and Cache Usage. The details are
read-only. Every ID and path row should have a Copy action in its context menu.

### Identity and server metadata

Display all fields from the cached `mailboxes` row:

- account display name;
- login/connection identity;
- JMAP account ID;
- mailbox name;
- JMAP mailbox ID;
- parent mailbox name and raw parent mailbox ID;
- role;
- sort order;
- subscribed state;
- total email count;
- unread email count;
- total thread count;
- unread thread count;
- cached mailbox state value;
- rights, expanded into individual booleans;
- raw `rights_json` in an expandable monospace row.

The rights section should expose every value represented by `MailboxRights`, including rights that
are false. A developer inspecting permissions should not have to infer omitted values.

### Derived synchronization and materialization state

Display related state that is not part of the base mailbox object:

- current account Mailbox state token and Email state token;
- number of cached `email_mailboxes` memberships;
- number of mailbox query windows and window items;
- query-window coverage/materialization states;
- oldest and newest cached message timestamps;
- observed/foreground status, when available from the mailbox interest registry;
- full-offline selection (`desired`);
- full-offline status;
- query and email state tokens recorded by the offline mirror;
- expected and completed item counts;
- completed and estimated bytes;
- current and completed generations;
- anchor email ID;
- last offline-mirror error and update time;
- vault reference count;
- pending, failed, and completed vault projection job counts;
- indexed-document count, where the search index can report it;
- active optimistic mutation count touching the mailbox;
- latest cache-invalidation generation relevant to the mailbox, if exposed by the consistency
  subsystem.

Values that are not present should render as an em dash, not an empty string or zero.

Add **Copy mailbox report**. It copies a structured plaintext or JSON report for the selected mailbox.
Raw IDs are included because this is an explicitly developer-facing action. Message subjects,
addresses, body content, credentials, OAuth tokens, and attachment names are never included.

## Cache usage presentation

The Cache Usage section has two independent rows.

### SQLite cache

Display:

- **Estimated mailbox data**: logical payload bytes attributed to mailbox list windows,
  memberships, offline enumeration, and message metadata that is exclusively retained by this
  mailbox;
- **Shared message metadata**: logical bytes also needed by another mailbox, search result, draft,
  notification, submission, or active mutation;
- row counts by major table in an expandable breakdown;
- **Clear SQLite cache…**.

The primary label must say **estimated**. SQLite stores multiple mailboxes in shared B-trees and
indexes, and deleting rows usually converts pages to free pages rather than shrinking the database
file. Pretending that a mailbox owns an exact number of physical SQLite bytes would be incorrect.

The dialog separately shows global SQLite file statistics on the Other Caches page, including actual
allocated bytes, WAL bytes, page count, free-list pages, and potentially reclaimable bytes.

The estimate should be deterministic and useful for comparison. Calculate it from the byte lengths
of mailbox-attributable values plus a documented per-row/per-index overhead model. Keep the formula
in one cache-accounting component and version it so tests can assert stable results. Do not issue a
large number of per-message queries; use set-oriented aggregate SQL.

### Mail body cache

Display:

- **Logical mailbox bodies**: sum of distinct raw MIME objects represented in the mailbox;
- **Shared bodies**: bytes also retained by another mailbox or retention reason;
- **Reclaimable bodies**: bytes that would have no remaining retention owner after the requested
  clear;
- **Allocated filesystem bytes**: actual allocated blocks found during the filesystem scan;
- missing object count;
- orphan projection count;
- active lease count;
- **Clear body cache…**.

The vault stores content-addressed objects and hard-linked mailbox projections. Therefore mailbox
logical totals do not add up to the vault's physical total. The UI should explain this in a tooltip
and in the confirmation dialog.

The scan should deduplicate files by `(device, inode)` and use allocated blocks where the platform
provides them. The database metadata remains useful for ownership and expected-size calculations,
while filesystem stats verify what is actually present. A mismatch is diagnostic information, not a
reason to silently substitute one value for the other.

# Clearing a Mailbox Cache

## General rules

All mailbox clear operations are daemon-owned commands. The GUI must not delete from the primary
SQLite database or mail vault directly.

Each command must:

1. identify the target by typed `(accountId, mailboxId)`;
2. revalidate that the mailbox still exists;
3. acquire a mailbox-scoped maintenance lock;
4. prevent new materialization/download work for the target from starting;
5. wait for or cooperatively cancel target work that has not committed;
6. preserve active optimistic mutations and their confirmed bases;
7. commit database changes transactionally;
8. perform filesystem changes through the vault projection/garbage-collection machinery;
9. publish cache invalidation only after the database transaction commits;
10. schedule a new measurement and report the measured outcome.

A stale refresh or download started before the clear must not repopulate the cache after the command
completes. The operation therefore needs the same generation-fence discipline used by normal cache
materialization.

## Clear SQLite cache

The action means "discard rebuildable message-list and message-metadata materialization for this
mailbox". It does not delete the mailbox object, account configuration, user settings, or active
mutations.

The daemon-side repository should perform one explicit mailbox-reset transaction rather than a
series of ad hoc table deletes. The reset should:

- delete mailbox query windows and their items;
- invalidate or remove cached confirmed membership for the target mailbox;
- reset offline enumeration checkpoints and progress while preserving the user's offline-selection
  preference;
- remove message metadata and dependent normalized rows only when the retention graph proves that
  no other mailbox/window/search/draft/submission/notification/mutation still needs the email;
- invalidate affected search-index documents for asynchronous rebuild;
- preserve the `mailboxes` row and its server-provided metadata so the mailbox remains visible;
- preserve account and mailbox state needed to process active optimistic mutations safely;
- mark the mailbox for a fresh query when it is next observed.

When the mailbox is currently visible, refresh it after the reset. Otherwise leave it empty and
refresh on next observation; clearing a cache should not unexpectedly trigger a large network job
for an inactive mailbox.

The completion result reports logical bytes/rows discarded and global SQLite free pages gained. It
must not claim that the database file shrank. Database compaction is a separate global action.

## Clear body cache

The action removes raw MIME retention attributable to the selected mailbox. It should:

- remove mailbox projection links;
- remove or downgrade vault retention only where the target mailbox is the owning retention reason;
- preserve objects retained by another mailbox, draft, submission, explicit download, or active
  reader lease;
- queue garbage collection for newly unretained objects;
- remove corresponding search-index content when no body remains available;
- leave server message metadata intact.

Objects with active leases are reported as deferred and collected when the final lease is released.
A clear operation can therefore complete successfully with a small deferred byte count.

### Offline mirror interaction

Clearing bodies from a mailbox configured for complete offline storage would otherwise trigger an
immediate redownload. The confirmation offers three choices:

- **Disable offline storage and clear** (default);
- **Clear and allow redownload**;
- Cancel.

The first choice updates the existing offline-selection preference through its normal application
command before clearing. The second is useful for repairing suspected corruption while retaining the
offline policy.

## Confirmation and feedback

The confirmation dialog names the account and mailbox, shows the latest calculated amounts, and
states whether values are shared or estimated. Never use a generic "Are you sure?" prompt.

Example:

```text
Clear cached bodies for Inbox?

612 MiB of message bodies are visible in this mailbox. About 438 MiB is expected to become
reclaimable; the remainder is shared with other mailboxes.

Inbox is configured for complete offline storage.
```

After completion, show an inline result such as:

```text
Cleared 3,842 mailbox projections. 436 MiB was reclaimed; 2.1 MiB is deferred while in use.
```

Failures remain visible in the page until dismissed and include a Copy Details action.

# Other Caches Page

Use a table with one row per cache/store and these columns:

| Cache | Location/owner | Size | Items | Status | Action |
| --- | --- | --- | --- | --- | --- |

Each row declares whether it is rebuildable, persistent user data, or mixed. Only rebuildable data
gets a Clear button.

## Translation result cache

Initial supported non-mail cache:

- path: the `TranslationCache` SQLite database;
- main database, WAL, and SHM allocated sizes;
- translated row count;
- oldest and newest entry times when available;
- provider/language-pair counts in an expandable breakdown;
- **Clear translated text cache…**.

Clearing suspends new translation-cache reads/writes, waits for active cache operations, closes the
owning SQLite connection, replaces or truncates the cache on a worker thread, migrates a clean
cache, and resumes the service. In-flight translation network/model work may finish, but its result
must not write through an obsolete cache generation.

Downloaded local translation models are not cache entries. They are deliberately downloaded user
data and already have management UI. Show their total size as **Installed translation models** with
an **Open Translation Settings** action, not a generic Clear action.

## Primary SQLite store

Show global facts:

- database path;
- schema version and latest migration;
- main file logical and allocated size;
- WAL and SHM size;
- page size and page count;
- free-list page count and estimated reclaimable bytes;
- connection/read-only status;
- cache instance UUID;
- **Checkpoint WAL**;
- **Compact database…**;
- **Run quick integrity check**.

`Compact database` is a daemon maintenance operation. It uses the existing `CacheAccessBarrier`,
pauses database participants, checkpoints, runs `VACUUM`, validates the reopened connection, and
resumes participants. It should be disabled while a mutation is in an ambiguous or non-interruptible
commit phase. This operation is not run automatically after per-mailbox clearing.

A future **Reset all local cache…** action may atomically switch to a new cache instance using
`CacheLocationProvider::replaceCacheInstance()`. It is deliberately out of scope for the first
version because it requires a complete resync, explicit account/session recovery semantics, and
careful cleanup of the old instance.

## Mail vault

Show global vault statistics:

- content objects;
- logical bytes;
- allocated bytes;
- retained versus evictable objects;
- active leases;
- mailbox projections;
- pending and failed projection jobs;
- orphan files and missing files found by the most recent verification;
- **Collect unretained bodies**;
- **Verify vault**;
- **Retry failed projections**.

Do not provide an unconditional "delete vault directory" button. The vault has database-owned
references and must be modified through its service.

## Search indexes

Search indexes are rebuildable and stored per account. Show each account index as a child row:

- path;
- allocated bytes;
- indexed document count;
- last successful update if available;
- **Clear index**;
- **Rebuild index**.

Clearing must close the account's index connection before removing/replacing the index database.
Rebuild is daemon-scheduled, preemptible maintenance work and should appear in Task Center.

## Draft assets

`draft-assets` contains staged inline images and attachments associated with compose sessions. It is
persistent working data, not a general cache. Do not offer **Clear all**.

Offer an optional **Find orphaned draft assets** diagnostic. Only assets that have no live compose
session and exceed a conservative age threshold may be removed, with a list shown before deletion.

# Diagnostics Page

The Diagnostics page combines safe inspection and narrowly scoped repair actions.

## Recommended first-version functions

### Copy diagnostic report

Produce a structured report containing:

- Javelin version and build type;
- Qt/KDE versions;
- GUI and daemon process versions/protocol version;
- cache instance UUID and schema version;
- database/vault/index/translation-cache sizes;
- configured connection labels and account capabilities;
- mailbox counts and background-job summary;
- push transport state;
- failed maintenance/projection counts;
- the result of the latest integrity checks.

By default, hash or omit account IDs, mailbox IDs, email addresses, paths containing the username,
and server URLs. Provide an explicit **Include raw identifiers** checkbox because the dialog is often
used to prepare bug reports, but never include credentials, access/refresh tokens, cookies, message
content, subjects, contact data, or attachment names.

### Integrity checks

Offer independently runnable checks:

- SQLite `PRAGMA quick_check`;
- foreign-key check;
- mailbox/query-window referential consistency;
- optimistic-mutation projection consistency;
- vault database-to-filesystem consistency;
- hard-link/projection consistency;
- search-index readability;
- translation-cache readability.

Checks should return typed findings with severity, object kind, sanitized summary, and optional raw
identifier. Do not make repairs automatically. Each repair action must be explicit and described.

### Background work

Show a compact summary with **Open Task Center**. Useful additions are:

- retry failed vault projections;
- replay local maintenance;
- rebuild a selected search index;
- copy a selected job's checkpoint/error details.

Avoid duplicating the full Task Center in this dialog.

## Useful later additions

These are valuable extensions but not required for the initial implementation:

- **Force mailbox refresh** using the normal account-refresh/materialization ports;
- **Rebuild selected mailbox cache**, combining SQLite clear with an observed foreground refresh;
- **Copy raw mailbox record** as JSON;
- account capability and session endpoint viewer;
- state-token history and last push-change summary;
- optimistic journal viewer with mutation lifecycle, affected objects, and generation fences;
- notification outbox viewer;
- cache-invalidation event viewer;
- transport diagnostics and reconnect action;
- local-data migration status and retry;
- a sanitized recent-log export with category filtering;
- filesystem free-space and offline-download budget information;
- a read-only schema/migration viewer.

Do not add a raw SQL editor, raw JMAP request editor, arbitrary file deletion, or token viewer. Those
features create a large accidental-data-loss and secret-exposure surface and bypass Javelin's typed
boundaries.

# Architecture

## Ownership

Introduce two application-facing ports:

```cpp
class DeveloperDiagnosticsReader
{
  public:
    virtual ~DeveloperDiagnosticsReader() = default;
    virtual QCoro::Task<Result<DeveloperSnapshot>> snapshot() = 0;
    virtual QCoro::Task<Result<MailboxCacheMeasurement>>
    measureMailbox(MailboxCacheTarget target, MeasurementOptions options) = 0;
    virtual QCoro::Task<Result<GlobalCacheMeasurement>> measureGlobalCaches() = 0;
};

class DeveloperMaintenancePort
{
  public:
    virtual ~DeveloperMaintenancePort() = default;
    virtual QCoro::Task<Result<MailboxClearResult>>
    clearMailboxCache(ClearMailboxCacheCommand command) = 0;
    virtual QCoro::Task<Result<CacheMaintenanceResult>>
    maintainCache(CacheMaintenanceCommand command) = 0;
};
```

The types must be strongly typed structs, not JSON dictionaries passed through the application.
Serialization across `ProcessBoundary` is an implementation detail of the remote ports.

### Daemon-owned operations

The daemon owns:

- primary SQLite inspection requiring a coherent write-side view;
- mailbox SQLite clearing;
- mail-vault measurement and clearing;
- search-index measurement, clearing, and rebuilding;
- WAL checkpoint, compaction, and primary database integrity checks;
- maintenance/projection actions.

Add corresponding `RemoteActionKind` values and typed codec support. The GUI adapters use
`RemoteActionClient`; widgets never submit boundary commands directly.

### GUI-owned operations

The GUI owns:

- translated-text cache maintenance, because `TranslationCache` currently belongs to `GuiServices`;
- downloaded translation model information and navigation to translation settings;
- presentation state and aggregation of GUI-owned and daemon-owned results.

Longer term, translation cache access should move behind a serial worker-owned service so normal
translation lookup, measurement, and clearing share one connection owner and generation. The first
implementation may instead suspend the GUI cache on its owning thread, perform filesystem/SQLite
work on a worker, and reopen it on the owning thread.

## Service decomposition

Recommended non-GUI classes:

- `DeveloperDiagnosticsService`: orchestrates snapshots and checks;
- `MailboxCacheAccounting`: set-oriented SQL accounting and retention graph queries;
- `MailboxCacheMaintenance`: transactional mailbox reset operations;
- `VaultAccounting`: inode-aware filesystem measurement and database reconciliation;
- `GlobalCacheMaintenance`: checkpoint/compact/integrity operations;
- `DeveloperOptionsModel`: GUI model combining asynchronous results;
- `DeveloperOptionsDialog`: widgets only.

Repository/cache code should provide narrow operations used by these services. Do not place SQL or
filesystem traversal in the dialog.

# Threading and Responsiveness

## Worker execution

All expensive operations run off the GUI thread:

- recursive file stats;
- inode deduplication;
- aggregate accounting queries over large mail tables;
- integrity checks;
- cache deletion and garbage collection;
- checkpoint/VACUUM;
- search-index enumeration;
- translation-cache measurement/clearing.

Use a dedicated low-priority serial I/O executor or `QThreadPool` with a maximum thread count of one
for scans. Database workers open a connection on the worker thread; a `QSqlDatabase` connection is
never moved across threads.

Write operations use the existing database write scopes and transactions. Opening an additional
connection does not replace the application's write coordination.

## Cancellation

Read-only measurements accept a cooperative cancellation token and check it:

- between mailboxes;
- between directory batches;
- between major aggregate queries.

Cancellation returns partial results marked incomplete. Never discard already measured mailbox
rows.

Destructive operations have two phases:

1. cancellable preparation/measurement;
2. non-cancellable commit once the confirmation has been accepted and the maintenance lock is held.

The UI changes Cancel to **Finishing…** during commit rather than pretending cancellation succeeded.

## Progress

Progress is based on concrete units where possible:

- mailboxes measured / total mailboxes;
- files visited;
- objects/projections processed;
- SQLite phases completed;
- bytes reclaimed.

When a total is unknown, use an indeterminate bar and a meaningful phase label. Avoid rapidly
changing per-file labels.

# Consistency, Safety, and Privacy

## Consistency

- Measurement results include the primary database `data_version` and a cache generation. If either
  changes before display, mark the result **Changed since measurement** rather than silently showing
  it as current.
- Clear commands return the post-commit generation. The GUI invalidates and remeasures the affected
  rows.
- Per-mailbox accounting treats active mutations as retention roots.
- Filesystem deletion is idempotent. Missing files become findings, not fatal errors that leave the
  database transaction half-applied.
- Stale background jobs cannot commit into a cleared generation.
- Global operations use `CacheAccessBarrier`; mailbox-local operations use a narrower maintenance
  lock so unrelated accounts remain responsive.

## Error handling

Return typed error codes for:

- target disappeared;
- database busy/contention;
- active non-interruptible operation;
- filesystem permission or I/O failure;
- insufficient free space for `VACUUM`;
- integrity failure;
- cancellation;
- partial/deferred reclamation.

User-facing text resolves account/mailbox names and includes raw IDs only in expandable details.

## Privacy

The dialog intentionally exposes raw JMAP IDs and local paths to the local user. Clipboard/export
operations still require care because they are likely to be pasted into public bug reports.

- raw identifiers are opt-in for whole diagnostic reports;
- credentials and message/contact content are never queryable from this UI;
- no developer operation logs message IDs at routine log levels unless profiling/debug logging is
  explicitly enabled;
- copied paths may optionally replace the home directory with `~`.

# Testing

## Unit tests

Add deterministic tests for:

- SQLite accounting with one mailbox;
- shared emails across two mailboxes;
- an email retained by search/draft/submission/notification/mutation roots;
- logical versus reclaimable body bytes;
- inode/hard-link deduplication;
- missing and orphan vault files;
- mailbox reset preserving the mailbox row and unrelated mailboxes;
- mailbox reset preserving active optimistic mutations;
- offline-selection handling for both clear choices;
- body clear with active leases and deferred collection;
- stale materialization generation rejected after clear;
- translation cache generation preventing post-clear writeback;
- cancellation returning partial measurements;
- diagnostic-report sanitization.

## Integration tests

Use a temporary cache root and scripted services to verify:

- the GUI never blocks while a deliberately slow scan runs;
- progressive mailbox results appear;
- close/reopen of the dialog does not duplicate destructive work;
- daemon disconnect during measurement produces a recoverable error;
- daemon restart after a committed clear leaves a consistent cache;
- Task Center reflects long maintenance/rebuild jobs;
- global compaction correctly suspends and resumes GUI/daemon cache participants.

## UI tests

Verify:

- IDs and long paths are selectable and copyable;
- sizes use IEC units consistently;
- shared/estimated labels remain visible and are not tooltip-only;
- destructive actions are keyboard accessible but cannot be triggered by Enter on mere row
  selection;
- filter and selection survive progressive model updates;
- no zero value is substituted for an unknown measurement.

# Implementation Sequence

## Phase 1: Read-only mailbox inspection

1. Define typed snapshot/accounting result structures and codecs.
2. Add daemon `DeveloperDiagnosticsService` and remote reader port.
3. Implement base mailbox metadata and derived-state queries.
4. Implement asynchronous per-mailbox SQLite estimates and vault scans.
5. Add the Mailbox Caches page with progressive results and copyable reports.
6. Add accounting and cancellation tests.

This phase is useful without any destructive action and validates the accounting semantics first.

## Phase 2: Safe mailbox clearing

1. Add mailbox-scoped maintenance/generation locking.
2. Add the explicit mailbox-reset repository operation.
3. Add vault retention/projection clear operations and deferred lease handling.
4. Wire confirmation, progress, invalidation, and remeasurement.
5. Add optimistic-consistency and stale-work tests.

## Phase 3: Other caches

1. Add translated-text cache measurement and generation-safe clear.
2. Add global database/vault/index measurements.
3. Add search-index clear/rebuild.
4. Add vault verification and garbage collection.
5. Add WAL checkpoint, quick check, and compaction using `CacheAccessBarrier`.

## Phase 4: Diagnostics

1. Add sanitized diagnostic report generation.
2. Add typed integrity findings.
3. Add Task Center links and repair actions.
4. Add selected later diagnostics based on actual support/debugging needs.

# Decisions and Recommendations

The design makes the following deliberate choices:

- **Separate dialog, not Preferences page.** Operations are immediate and long-running.
- **Always available.** Recovery tooling should not require an environment variable.
- **Daemon owns mail cache maintenance.** The GUI remains a cache consumer and command issuer.
- **SQLite mailbox size is explicitly estimated.** Exact physical attribution is undefined in the
  shared database.
- **Body storage shows logical, shared, and reclaimable bytes.** A single number is misleading with
  content-addressed hard-linked storage.
- **No automatic database vacuum after clearing.** Clearing should be quick; compaction is explicit.
- **Offline-policy conflict is resolved in the confirmation.** The default disables offline storage
  before clearing to avoid immediate redownload.
- **No raw SQL/JMAP consoles.** Typed diagnostics provide the useful information without bypassing
  architecture or exposing secrets.
- **Read-only inspection ships first.** It establishes trustworthy accounting before destructive
  controls are enabled.
