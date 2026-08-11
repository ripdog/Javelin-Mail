# Thread Materialization Implementation Plan

## Status

This document is the accepted implementation plan for replacing unbounded collapsed-thread fan-out
with a split foreground/background materialization model. Implementation is in progress. It records
the product decisions agreed before implementation and is updated as phases land.

Phase 0 is complete. Deterministic `maxObjectsInGet = 2` fixtures cover both collapsed-page
implementations: canonical watched-mailbox refresh and the shared continuation/server-search page
path. They originally reproduced the oversized nested-result-reference failure with two
representatives whose Threads contain five Emails; Phase 3 evolved them to assert the bounded
representative-only foreground request and committed sparse window.

The stable product baselines are covered by production-path tests for representative order and
window totals, mailbox-scoped expansion, whole-conversation server-search expansion, quick-filter
selection continuity, notification activation/navigation policy, complete-offline browsing,
collapsed-thread action selection and resolution, and mail Undo/Redo. Later phases should evolve
these assertions rather than removing the protected behavior.

Phase 1 is complete. Migration 47 replaces `threads.email_ids_json` with ordered
`thread_email_members` rows and records membership freshness, global member count, and associated
Thread state on `threads`. `ThreadRepository` now atomically replaces membership, preserves stale
snapshots, resolves reverse membership, reports missing child Emails and coverage, and only reports
a mailbox-local member count when coverage proves it. Operational Thread membership readers in
`QueryService` now use the normalized table, including ordered expansion and list aggregation.

Phase 2 is complete. Message-list rows now carry independent optional mailbox-local and global
Thread counts. Representative Email data is the sole source for row subject, preview, sender, date,
unread, flagged, attachment, tags, and similar state. Mailbox counts remain unknown until complete
Thread or complete-offline mailbox coverage proves them, while a known multi-message global Thread
still enables expansion. Accessibility and reply controls avoid presenting the global count as an
exact mailbox-local count, and authoritative query-window representatives remain stable as newer
children are hydrated.

Phase 3 is complete. Canonical mailbox bootstrap, continuation pages, and server-search pages now
finish after a collapsed `Email/query` and bounded representative `Email/get`. Their query limit is
clamped to the negotiated `maxObjectsInGet`; no foreground request depends on `Thread/get` or child
Email fan-out. Representatives, active-projection rebasing, and exact complete query-window state
commit atomically, so sparse cached children are neither required for rendering nor mistaken for
authoritative mailbox membership.

Phase 4 is complete. A daemon-owned `ThreadMaterializationCoordinator` now derives targets from
committed mailbox and search windows, coalesces duplicate Thread ids per account, exposes exact
interactive ensure/priority escalation, and restores incomplete targets from durable window and
Thread freshness state after restart. Its queue uses a scheduler-owned transient admission path, so
prefetch obeys foreground quiet periods and account serialization without creating Task Center
jobs. Start and terminal signals provide the narrow materialization lifecycle event; Phase 5 plugs
the bounded `Thread/get` worker into the coordinator's execution seam.

Phase 5 is complete. The coordinator now owns a production membership worker that sorts and
deduplicates explicit Thread targets, bounds every `Thread/get` by the cached session's negotiated
`maxObjectsInGet`, and rejects any response whose `list` plus `notFound` does not exactly account for
the requested ids. Each valid batch atomically replaces returned membership and marks represented
`notFound` snapshots stale, then derives missing child Email ids from SQLite as the Phase 6
checkpoint. Batch progress and post-commit signals are emitted without persistent Task Center jobs,
and invalidation is limited to committed mailbox/search windows containing affected
representatives. No child `Email/get` is issued yet.

Phase 6 is complete. Missing child Email ids are selected from normalized membership in bounded
SQLite slices and sent as explicit `Email/get.ids`; no result reference connects membership to
hydration. Each request is capped by `maxObjectsInGet`, reduced further when necessary to fit
`maxSizeRequest`, and committed independently through the optimistic-consistency projection
transaction before its affected Thread rows are invalidated. Current membership with missing child
coverage is now restart-recoverable work. Child `notFound` or a mismatched returned `threadId`
stales the membership and triggers a fresh bounded `Thread/get`; reconciliation is capped at two
attempts, after which the Thread remains stale for normal freshness retry. Successful completion is
derived from SQLite having no missing current members, without retaining an unbounded child-id
checkpoint in memory.

Phase 7 is complete. Expansion is now a persistent presentation intent in the message-list model:
one read-only SQLite snapshot proves current membership and complete child Email coverage before
any child rows are inserted. Incomplete expansion requests travel through the active mailbox or
search session and the typed GUI/daemon boundary to raise the existing coordinator target to
interactive priority; the GUI does not perform JMAP work or create placeholder rows. Normal
post-commit window invalidation retries pending reads, and a completed snapshot inserts all scoped
children in one model update. Collapsing clears the intent without cancelling shared prefetch.
Mailbox sessions retain mailbox filtering, while search sessions load the complete conversation.

Phase 8 is complete. Collapsed-Thread command admission now remains asynchronous through daemon
application coordination. The coordinator exposes a coalesced interactive wait that resumes only
after SQLite proves current membership, exact membership cardinality, and complete child Email
coverage, and propagates materialization/network failure to every affected waiter. Only then does
the application resolve mailbox-scoped or global Email ids, load exact Email metadata, check
rights, prepare optimistic records, and construct Undo history. Strict selection resolution rejects
incomplete coverage and never falls back to the representative Email. The parallel GUI tag-menu
helper no longer expands cached Thread subsets; it leaves aggregate check state unknown for a
collapsed Thread and sends the original Thread intent unchanged when an action is chosen.

The target architecture is:

```text
foreground page materialization
    Email/query (collapseThreads)
      -> bounded Email/get of representatives
      -> commit query window + representative Emails
      -> publish cache change
      -> GUI renders immediately

background Thread materialization
    bounded Thread/get for represented Threads
      -> persist Thread membership
      -> determine missing child Email ids from SQLite
      -> bounded Email/get batch
      -> commit/rebase/invalidate
      -> repeat until child coverage is complete
```

The final `Email/get` must never be driven directly by a result reference to
`Thread/get /list/*/emailIds`. A collapsed query may return at most one representative per Thread,
but those Threads can collectively contain far more Emails than the server's `maxObjectsInGet`
limit.

This work deliberately separates **query-window completeness** from **conversation child
materialization**. A collapsed list is usable before its Threads are fully hydrated.

## Product decisions

The implementation must preserve these product semantics:

- Mailbox views remain mailbox-scoped when a Thread is expanded. Javelin does not switch to a
  cross-mailbox conversation view as part of this work.
- Server search expansion shows the whole conversation rather than only the representative or the
  subset of Emails that happened to match/cache locally.
- Collapsed-row unread, flagged, attachment, tags, and similar status describe the representative
  Email. They must not change merely because additional children became cached.
- A global Thread membership count may establish that a row can expand, but it is not displayed as a
  mailbox-local count. A mailbox-local count is shown only when child coverage is sufficient to
  prove it.
- Page rendering does not wait for child materialization. The representative set commits first and
  becomes visible immediately.
- Thread child retrieval starts automatically after the page is usable. This is prefetch, not a
  user-triggered lazy-only path.
- If the user opens a Thread before the automatic prefetch finishes, the desired expansion waits on
  the existing daemon work. The message-list-wide progress indicator is the loading indication; no
  per-row loading widget is required.
- Actions remain available while that work is in progress. A whole-thread command waits for
  authoritative/complete membership when necessary; it never silently falls back to the
  representative or known cached subset.
- Existing online cache-retention behavior remains unchanged. This project does not introduce a new
  LRU or session-only child policy.
- Complete-offline mailboxes remain complete mirrors and keep their existing uncollapsed full-sync
  path. Online thread prefetch must not weaken that contract.
- Notification navigation materializes the target/contextual query window and selected Email first;
  it does not synchronously fetch the rest of the Thread before opening the message.
- Thread freshness is demand-scoped. Do not add a permanent global `Thread/changes` synchronizer in
  this implementation unless later evidence proves the Email-delta invalidation model insufficient.
- Large whole-thread mutations use best-effort bounded batches. Successful batches are not
  speculatively rolled back because a later batch failed. Per-object mutation state and Undo must
  reflect the actual settled result.

## New invariants

The following invariants are prerequisites, not optional implementation details:

1. A complete collapsed query window means that its ordered representative ids and required
   representative Email data are materialized. It does not mean every Email in every Thread is
   cached.
2. Thread membership freshness and child Email-object coverage are independent cache facts.
3. Presence of some child Email rows never proves complete Thread coverage.
4. A Thread with complete membership may still have partial child Email-object coverage.
5. Collapsed-row summary properties never depend on an opportunistic subset of cached children.
6. Child Email retrieval always uses explicit id batches bounded by negotiated JMAP limits.
7. A result reference may be used for a bounded representative dependency, but never to flatten an
   unbounded nested Thread membership into `Email/get`.
8. Whole-thread commands retain Thread intent until the daemon can resolve the intended Email set.
9. An uncached Email tombstone is normal in an online sparse cache unless it intersects tracked
   Thread/query/mutation state that requires reconciliation.
10. Complete-offline mailboxes continue to materialize every Email regardless of collapsed-query
    child coverage.
11. Every cache commit that writes confirmed Email objects rebases active Email projections before
    publishing invalidation.
12. The GUI never performs JMAP work or writes thread coverage state. SQLite remains the only local
    data plane.

## Scope

### In scope

- foreground representative-only collapsed mailbox and server-search materialization;
- negotiated query/get object limits;
- normalized Thread membership storage and freshness;
- automatic daemon-owned Thread prefetch;
- resumable, bounded child Email hydration;
- expansion waiting on materialization without a second network path;
- sparse-cache-aware Email delta reconciliation;
- authoritative whole-thread selection resolution;
- `maxObjectsInSet` batching for large Email operation groups;
- deterministic coverage tests across restart, races, and partial mutation outcomes;
- documentation updates and removal of obsolete assumptions.

### Out of scope

- changing mailbox-scoped expansion into a global conversation view;
- a new online cache eviction policy;
- a per-thread spinner or loading row;
- a global `Thread/changes` synchronization loop;
- changing complete-offline mailbox semantics;
- fetching aggregate summary metadata eagerly solely to preserve old thread-level unread/star/etc.
  aggregation;
- introducing a separate in-memory thread cache in the GUI or daemon.

## Phase 0: Baseline and regression fixtures

Before changing production behavior, add deterministic fixtures that reproduce the limit problem.
Use a fake Session with very small limits, for example:

```text
maxObjectsInGet = 2
maxObjectsInSet = 2
```

Return a collapsed query with two representatives whose Threads collectively contain more than two
Emails. The current chained final `Email/get` should be demonstrably illegal under that Session.
The replacement tests must assert that no emitted explicit `Email/get` contains more than two ids.

Capture existing product behavior that must remain stable:

- representative ordering and query-window totals;
- mailbox-scoped Thread expansion;
- server-search Thread expansion;
- quick-filter selected-row continuity;
- notification navigation;
- complete-offline mailbox browsing;
- collapsed Thread actions and Undo/Redo.

This phase gives later refactors a production-path regression harness rather than relying on helper
unit tests alone.

## Phase 1: Normalize Thread membership

### Schema

Replace operational dependence on `threads.email_ids_json` with normalized membership:

```text
thread_email_members
    account_id
    thread_id
    position
    email_id
```

Recommended key/index shape:

- primary key `(account_id, thread_id, email_id)`;
- unique/order index `(account_id, thread_id, position)`;
- reverse lookup index `(account_id, email_id)`.

The reverse lookup is important for processing an `Email/changes.destroyed` id that has no
corresponding `emails` row.

Evolve `threads` so it can represent membership state explicitly. At minimum it needs:

- account/thread identity;
- membership freshness (`current` versus `stale`, or an equivalent explicit type);
- global member count derived from the persisted membership snapshot;
- any JMAP Thread state/freshness metadata the implementation can validly associate with that
  snapshot.

Do not keep JSON membership as a second production representation after migration. The database
migration may read `email_ids_json` to seed normalized rows, then all readers/writers should move to
the normalized table in the same change.

### Repository API

Evolve `ThreadRepository` around coverage rather than merely serializing a domain `Thread`:

- replace membership atomically for one/many Threads;
- mark membership stale without deleting useful last-known members;
- read ordered membership;
- resolve Thread id by member Email id;
- query missing child Email ids by joining membership against `emails`;
- determine whether child coverage is complete for a current membership snapshot;
- count current-mailbox members only when the required Email membership rows are materialized.

Persist membership replacement and freshness in one transaction. A crash cannot leave a Thread
marked current with only half its new membership rows.

### QueryService migration

Remove `json_each(threads.email_ids_json)` readers in `QueryService`. Convert:

- `listThreadMessages`;
- `listMailboxThreadMessages`;
- message-list aggregation paths;
- any selection-resolution helpers;
- any diagnostics/tests that inspect Thread membership.

At this stage behavior may still fetch children eagerly; the goal is to make partial coverage
representable before introducing it.

## Phase 2: Make list rows independent of child Email coverage

Current message-list queries derive Thread-level count/unread/flag/attachment state from all cached
children. That becomes invalid once child rows are intentionally sparse.

Change the read model so:

- representative Email is the sole source for row subject/preview/date/from/unread/flag/attachment,
  tags, and similar properties;
- the ability to expand is independent of an exact mailbox-local message count;
- mailbox-local message count is optional/unknown until coverage proves it;
- global Thread member count is stored separately from mailbox-local count;
- a row never gains or loses unread/star/attachment state just because prefetch progressed.

`MessageListModel::CanExpandRole` must support a known multi-message Thread without requiring a
mailbox-local exact count. Accessibility text must likewise avoid claiming an exact mailbox count
until one is known.

The query representative remains stable according to the authoritative query window. Hydrating a
newer child does not replace the visible representative merely because it is now cached.

## Phase 3: Split foreground collapsed materialization

Update both current collapsed-page implementations:

- `MailboxRefreshExecutor` canonical/watched mailbox bootstrap;
- `JmapCore::performCollapsedQueryPage` mailbox continuation and server-search pages.

The foreground network dependency becomes:

```text
Email/query(collapseThreads: true)
  -> representative Email/get
```

Remove `Thread/get` and the final child `Email/get` from the foreground page-completion condition.

### Request limits

Clamp the collapsed query limit so its representative `Email/get` is legal under
`maxObjectsInGet`. Javelin's nominal window size may remain 100, but a server advertising 50 must
produce requests no larger than 50. The returned `position`, returned `limit`, and actual result
count remain authoritative for continuation.

The generic request envelope still must satisfy `maxCallsInRequest` and `maxSizeRequest`.

### Commit semantics

A successful foreground transaction must atomically:

- upsert every returned representative Email;
- rebase active Email projections;
- persist exact ordered query-window membership and query metadata;
- classify the window as complete/non-stale when every requested representative is accounted for;
- publish the existing bounded cache invalidation only after commit.

A missing representative from `Email/get` remains a materialization failure; child absence does not.

The GUI should become able to render the committed page before any `Thread/get` response exists.

## Phase 4: Introduce automatic Thread materialization coordination

Add a daemon-owned `ThreadMaterializationCoordinator` (name indicative) that accepts committed
mailbox/search-window targets and coalesces their represented Thread ids. It owns network execution;
the GUI owns only presentation intent and loading display.

Do **not** create one persistent `background_jobs` record per page. These are short-lived cache
prefetches and would turn normal scrolling into Task Center noise. Thread membership and child Email
coverage are the durable recovery state; a transient coordinator queue is scheduling state, not a
second cache truth.

Suggested characteristics:

- coalesce duplicate Thread ids across mailbox windows, continuation windows, and search windows;
- automatic page prefetch runs below interactive/foreground work but promptly after the
  representative page commits;
- an opened Thread or whole-thread command can raise/ensure the same target rather than creating a
  competing fetch path;
- use existing foreground-availability/account-serialization policy; if `WorkScheduler` cannot admit
  transient prefetch cleanly, add a narrow transient admission API rather than persisting every page
  as a semantic background job;
- after daemon restart, active/watched/restored windows enqueue missing coverage again and derive the
  remaining work from SQLite;
- completion/failure emits a narrow application event or cache/materialization invalidation so the
  relevant message-list session can update its loading state.

After the foreground page commit, enqueue Thread materialization for its representatives. A
prefetched continuation window may also hydrate automatically; scheduler priority must ensure this
does not delay user-visible foreground work.

Avoid duplicate work when several windows contain representatives from the same Thread. Durable
membership/missing-child queries make execution idempotent even when transient scheduling overlaps.

## Phase 5: Materialize Thread membership in bounded requests

The worker reads represented Thread ids from committed representative Email/query-window state and
requests `Thread/get` in batches no larger than `maxObjectsInGet`.

For each successful batch:

1. account exactly for requested Thread ids using `list` plus `notFound`;
2. replace membership atomically in `threads` + `thread_email_members`;
3. mark that membership snapshot current;
4. compute missing child Email ids from SQLite;
5. update job progress/checkpoint;
6. publish only the cache changes required for list rows whose expansion/count information changed.

Do not infer global Thread freshness from equality with an account-wide Email state token. A keyword
change can advance Email state without changing Thread membership.

If a Thread requested for a currently represented Email is `notFound`, treat that as a
reconciliation case rather than silently inventing a single-message Thread.

`maxObjectsInGet` bounds the number of Thread objects requested, not the length of each returned
`Thread.emailIds` array. RFC 8621 exposes no count-only Thread shape, so one pathological conversation
can still produce a large `Thread/get` response. That residual response-size risk is an accepted
tradeoff of automatic membership prefetch; the important guarantee here is that the subsequent
child `Email/get` fan-out is explicit and bounded.

## Phase 6: Hydrate child Emails in explicit bounded batches

Once membership is persisted, child Email requests are built from explicit ids selected from
SQLite. Never build this call with a result reference to `Thread/get /list/*/emailIds`.

Batch size must be at most `maxObjectsInGet` and also respect generic request-size validation. One
`Email/get` per envelope is the simplest initial policy and avoids creating another batching problem
through `maxCallsInRequest`; multiple bounded calls may be packed later only if measurement shows a
real benefit.

Each successful batch commits independently:

- upsert confirmed Email objects;
- rebase active Email projections for exactly those ids;
- apply normal affected-query invalidation rules where the newly learned confirmed Email state
  changes a tracked view;
- advance durable work progress;
- publish cache change after commit.

Do not retain an entire pathological Thread in memory until all batches finish.

### `notFound` race

If a child id from a current Thread membership snapshot is returned in `Email/get.notFound`, the
Thread may have changed between `Thread/get` and `Email/get`. Do not simply delete that id from the
membership table and call the Thread complete. Mark the Thread membership stale and reconcile it
with a fresh bounded `Thread/get`. Bound retries so a rapidly changing Thread cannot spin forever;
leave it stale and retry through normal freshness work after the limit.

### Completion

Child coverage is complete only when the current membership snapshot has no missing Email objects.
That fact should be derivable from SQLite after restart. It does not need a second in-memory boolean
owned by the worker.

## Phase 7: Make expansion wait on cache coverage

Today Thread expansion is effectively a read-only SQLite operation. Evolve it into a cache-backed
presentation intent that can wait for daemon materialization.

When the user expands:

- if current membership and required child Email coverage are complete, load and insert children
  immediately from SQLite;
- otherwise preserve the desired expanded state and ensure/raise the priority of the existing Thread
  materialization work;
- do not start a separate GUI-side network operation;
- do not insert a per-row loading placeholder;
- listen to normal post-commit cache invalidation and retry the pending expansion;
- once complete, insert the mailbox-scoped child rows in one coherent model update.

If the user collapses before completion, clear only the presentation intent. Automatic prefetch may
continue because it is page cache work rather than expansion ownership.

Search sessions use the same coverage mechanism but read the complete conversation rather than
mailbox-filtering the hydrated children.

## Phase 8: Make whole-thread commands authoritative

`MessageSelection` may continue to represent `SelectedCollapsedThread`, but selection resolution
must no longer mean "expand whatever is currently in SQLite".

Move the authoritative resolution contract into daemon application coordination:

1. retain the Thread intent from GUI command admission;
2. inspect current membership/child coverage;
3. if necessary, wait for or run bounded Thread materialization;
4. resolve the intended Email ids according to command context:
   - mailbox view: members currently in that mailbox;
   - search/global conversation context: entire Thread;
5. materialize any Email metadata required for rights checks, mutation base snapshots, optimistic
   projection, and Undo;
6. only then create exact per-Email optimistic mutation records.

Delete the representative-only fallback from `resolveMessageSelection` and any parallel GUI helper
that can silently narrow a collapsed-thread action.

Actions need not be disabled while coverage is loading. The existing global work indication tells
the user the operation is waiting. If the network is unavailable and the cache cannot prove the
whole intended set, return the normal actionable unavailable/network result; never submit a partial
subset as though it were the whole conversation.

## Phase 9: Bound large Email/set operation groups

A resolved large Thread can exceed `maxObjectsInSet` even after `/get` fan-out is fixed.

Update Email mutation submission so one logical operation group can dispatch several legal
`Email/set` batches. The application layer owns this batching because it owns operation-group and
partial-failure policy.

Requirements:

- each batch contains at most `maxObjectsInSet` Email updates/destroys;
- mutation journal records remain per object and retain their ordinary pending/in-flight/
  accepted/rejected/unknown lifecycle;
- one logical user command/history item spans all batches;
- batches are submitted sequentially when compare-and-swap state is required; if `ifInState` is
  used, a later batch uses the state returned by the preceding settled batch rather than reusing the
  operation's original state token;
- a deterministic failure in a later batch does not roll back already accepted earlier batches;
- ambiguous dispatched batches become `unknown` exactly as today;
- Undo is constructed from the subset that actually settled successfully, while unresolved unknown
  records remain unresolved rather than guessed;
- cache/window invalidations remain post-commit and bounded.

Add tests where batch 1 succeeds and batch 2 rejects, and where an intermediate transport failure is
ambiguous.

## Phase 10: Make Email delta sync sparse-cache aware

Audit `MailDeltaRefreshExecutor` and related refresh planning for assumptions that every relevant
Email is cached because collapsed Thread fetches used to hydrate children eagerly.

### Updated or destroyed uncached Email

An `Email/changes.updated` or `Email/changes.destroyed` id absent from `emails` is not automatically
evidence that the cache is incomplete or corrupt. In an intentionally sparse online cache this is
normal. Resolve intersections instead:

- if a destroyed id appears in `thread_email_members`, mark that Thread membership stale;
- if an updated id is a known but unmaterialized Thread member, keep child coverage incomplete; do
  not fetch it merely to preserve an eager-cache invariant;
- if the id appears in tracked query/window state, reconcile that query as required;
- if the id belongs to active mutation/history state, use the existing consistency rules;
- otherwise the uncached change can be irrelevant to the bounded online working set and need not be
  materialized before advancing the account Email state.

`Email/queryChanges` remains responsible for proving changes to a tracked collapsed query. Do not
force an account/mailbox full refresh solely because an untracked updated or destroyed Email was
never materialized locally.

### Created Emails and membership changes

A newly fetched Email includes `threadId`. If that Thread has cached membership, mark it stale (or
apply an exact safe membership update only when correctness can be proven). Thread merges surface as
Email delete/recreate behavior under RFC 8621 and should invalidate affected cached membership in the
same manner.

Do not add account-wide `Thread/changes` polling/sync in this phase. Refresh a stale Thread when it
is represented by active page-prefetch work, pending expansion, a command, or another concrete
consumer.

### Query correctness

Continue to use `Email/queryChanges`/query reconciliation for authoritative collapsed ordering and
membership. Thread child coverage cannot establish server query position.

## Phase 11: Preserve offline-full behavior

Keep `FullMailSyncService`'s uncollapsed complete enumeration as the source of truth for
**Keep complete offline copy**.

Review interactions so:

- its page size is bounded by negotiated `maxObjectsInGet`;
- it does not wait for the online Thread prefetch coordinator to consider Email metadata complete;
- complete mailbox expansion works entirely from local Email membership;
- restart during body hydration still resumes from existing durable state;
- normalized Thread membership may improve conversation reads but cannot become a prerequisite that
  weakens full-mailbox completeness.

An offline-complete mailbox already has the child Emails required for actions and expansion. No
network wait should be introduced there solely by this architecture change.

## Phase 12: Search, notification, and continuation integration

### Server search

Apply the same representative-first pipeline to server-search windows:

- query + representatives commit and render;
- automatic Thread materialization follows;
- expanding a result waits for full conversation coverage if necessary;
- whole-thread actions resolve the whole conversation.

Do not imply that local FTS over an online sparse cache covers messages that have never been
materialized; complete-offline mailboxes retain full local search coverage.

### Notification navigation

A notification route to a concrete Email should materialize/select that Email and its contextual
query window without synchronously hydrating old children. After the target is visible, ordinary
page Thread prefetch can hydrate the conversation.

### Infinite-scroll continuation

Every continuation window uses its actual returned representative count/position and gets its own
automatic Thread-prefetch work. Child commits remain outside loaded-prefix accounting and therefore
must not alter offsets or continuation anchors.

## Work scheduling and user-visible progress

The Thread materializer is background cache work but directly benefits visible pages. It should use
the existing scheduler's foreground-availability and account-serialization policy without turning
each page into a persistent user-managed background job.

The intended ordering is:

```text
interactive command / explicit foreground materialization
    > Thread materialization required by an opened Thread or command
    > automatic Thread prefetch for visible/retained page
    > bulk offline/index/maintenance work
```

A pending expansion or whole-thread command raises/ensures the same coordinator target rather than
creating a second fetch path. Automatic prefetch pauses/yields according to the existing foreground
network policy.

The existing message-list-wide progress bar is the user-facing loading state. Do not overload
`MessageListState::refreshInFlight` to mean child coverage: today query-window cache invalidation can
complete/cancel that state as soon as the representative page commits. Add a separate narrow session
state such as `threadMaterializationInFlight` (exact name not prescribed), and render the existing
bar while either foreground list refresh or Thread materialization for the retained visible scope is
active.

The daemon/coordinator must signal start/finish/failure for the relevant materialization scope, while
SQLite remains authoritative for whether Thread membership/children are actually complete. The GUI
may remember only transient presentation/task state. A failed prefetch stops the loading indicator;
a later expansion, command, refresh, or normal retry can ensure the missing coverage again.

No per-row spinner, loading child item, or Task Center entry is required for ordinary page prefetch.

## Cache invalidation policy

Thread hydration must not cause broad invalidation merely because new cache objects exist.

- Foreground representative commit invalidates the exact mailbox/search window it materialized.
- Thread membership commit invalidates rows whose expandability/count metadata changed.
- Child Email commits use the ordinary Email/query invalidation rules based on confirmed membership,
  thread identity, received time, and active projections.
- A child from Archive fetched while viewing Inbox is not by itself proof that Archive's current
  ordered query window changed; compare confirmed pre/post state and query dependencies as existing
  delta logic requires.
- Expanded-row presentation reloads after the relevant Thread/cache commit, but expanded children do
  not enter query-window membership accounting.

This should reduce the cross-mailbox churn caused by today's eager all-child fetch during every
collapsed page load.

## Failure and restart semantics

The implementation should make partial progress ordinary and recoverable:

- foreground page failure: window remains missing/partial according to existing query-window rules;
- Thread/get failure: page remains usable; Thread membership stays missing/stale, the visible
  prefetch indicator stops, and the target is retryable;
- child Email/get failure: already committed child batches remain valid; unresolved members remain
  missing and the visible prefetch indicator stops for that failed attempt;
- daemon restart: no per-page persistent job recovery is required. Active/watched/restored windows
  enqueue prefetch again, and the coordinator derives only missing membership/children from SQLite;
- authentication/network unavailability: the page remains usable; interactive expansion/commands
  surface the normal actionable failure if their required coverage cannot be obtained;
- membership race/notFound: mark Thread stale and reconcile rather than publishing false
  completeness;
- query state advancement during child work: does not invalidate already confirmed Email objects by
  itself; query-window and Thread-membership freshness are reconciled by their own rules.

Do not serialize thousands of child ids into a transient work queue or checkpoint merely to resume.
Durable normalized membership and Email rows are the recovery checkpoint.

## Test plan

### Protocol/request-limit tests

- collapsed query nominal limit 100 against `maxObjectsInGet = 2` emits at most two representatives;
- two Threads containing more than two total child Emails never produce a child get larger than two;
- Thread/get batches are bounded;
- child `Email/get` batches are explicit ids, not nested result references;
- request-size validation still applies;
- huge operation groups partition at `maxObjectsInSet`.

### Cache/schema tests

- migration converts JSON membership preserving order;
- membership replacement is atomic;
- reverse Email-id lookup works without an `emails` row;
- current membership + missing child rows reports partial coverage;
- child coverage becomes complete after final batch;
- stale membership is distinguishable from current membership with partial children;
- representative row properties do not change after child hydration;
- mailbox-local count remains unknown until provable.

### Query-window tests

- window becomes complete after representative commit before Thread materialization finishes;
- child commits do not alter loaded-prefix ids/offsets;
- continuation works with a server limit smaller than nominal window size;
- stale/local-projection coverage remains independent from Thread child coverage;
- quick-filter selection continuity is unchanged.

### GUI/application tests

- expanding a hydrated Thread is immediate;
- expanding an unhydrated Thread records intent and renders children after cache invalidation;
- collapse-before-completion does not reopen the row when hydration finishes;
- no per-row loading item is introduced;
- whole-thread action waits and resolves all intended members;
- representative-only fallback is impossible;
- mailbox action remains mailbox-scoped;
- search action/expansion uses the whole Thread;
- network-unavailable unresolved Thread reports failure rather than mutating a subset.

### Delta/race tests

- updated or destroyed uncached/untracked Email does not force full refresh;
- destroyed uncached Email present in `thread_email_members` marks that Thread stale;
- updated uncached Thread member remains missing until ordinary prefetch/interactive materialization
  requires it;
- created Email marks cached membership stale;
- Thread merge delete/recreate path invalidates affected membership;
- child `notFound` after Thread/get triggers bounded membership reconciliation;
- keyword-only Email state advancement does not stale every cached Thread.

### Mutation/Undo tests

- operation group spanning multiple `Email/set` batches succeeds as one logical history action;
- later deterministic batch rejection preserves earlier accepted changes and reports partial failure;
- ambiguous batch remains unknown and is not speculatively undone;
- Undo covers settled successful objects correctly;
- restart during multi-batch submission preserves exact lifecycle state.

### Offline and navigation tests

- complete-offline mailbox does not depend on Thread prefetch;
- full sync still resumes correctly across restart;
- notification target opens before old Thread children finish hydrating;
- server-search representative page renders before child materialization;
- automatic continuation prefetch remains lower priority than foreground work.

## Suggested implementation order

The safest landing sequence is deliberately different from a one-shot rewrite:

1. add regression fixtures for tiny JMAP object limits;
2. migrate Thread membership to normalized durable state while preserving current eager behavior;
3. change list/read semantics so partial child coverage cannot corrupt summaries/counts;
4. split foreground representative materialization from child fetching;
5. add the coalescing daemon Thread materialization coordinator, visible-scope progress state, and
   bounded Thread/get;
6. add explicit bounded child Email/get batching and resumable completion;
7. wire expansion to wait on cache coverage;
8. move whole-thread selection resolution to authoritative daemon materialization;
9. add bounded multi-request `Email/set` operation groups;
10. make Email delta reconciliation sparse-cache aware;
11. verify offline-full, search, notification, quick-filter, and infinite-scroll integrations;
12. perform a regression review specifically looking for code that still treats cached child presence
    as proof of completeness;
13. run focused tests, affected production build, and `scripts/check-debug.sh --full` before the
    implementation commit.

This order keeps each intermediate state internally coherent. In particular, the data model and row
semantics must learn how to represent partial Thread coverage **before** the fetch path begins
creating it routinely.

## Files and subsystems expected to change

This list is directional; implementation should search all readers/writers before editing as required
by `AGENTS.md`.

### Cache/JMAP library

- `src/jmap/cache/Database.cpp` — schema migration and indexes;
- `src/jmap/cache/ThreadRepository.*` — normalized membership and coverage API;
- `src/jmap/cache/QueryService.*` / `QueryReader.h` — Thread reads, list summaries, counts;
- `src/jmap/sync/MailboxRefreshExecutor.*` — representative-only foreground bootstrap;
- `src/jmap/JmapCore.cpp` — representative-only mailbox/search page materialization and bounded
  request helpers;
- `src/jmap/sync/MailDeltaRefreshExecutor.*` — sparse-cache tombstones and membership invalidation;
- JMAP request-limit helpers if the existing generic limit abstraction cannot express all required
  get/set batching cleanly.

### Application/daemon

- a daemon-owned Thread materialization coordinator and service composition for its lifecycle;
- `src/app/WorkScheduler.*` only as needed for transient prefetch admission/foreground-yield policy;
  ordinary page prefetch should not create one persistent `background_jobs` row per window;
- `src/app/MessageListMaterializationPort.h`, application events, and remote protocol as needed to
  ensure/raise a Thread target and report visible-scope materialization start/finish/failure;
- `src/app/MessageSelection.*` and `MailApplicationService.*` — authoritative Thread command
  resolution;
- Email mutation submission path — `maxObjectsInSet` operation-group batching;
- cache-change publication/invalidation where membership completion becomes observable.

### GUI

- message-list read model/model roles — unknown mailbox-local count and expandability;
- message-list session/header state — separate Thread-materialization-in-flight state so the existing
  global list progress indicator can remain visible after representative rows render;
- Thread expansion controller/model path — pending expansion intent driven by cache invalidation;
- accessibility text for known/unknown Thread counts;
- no JMAP or writable cache dependencies may be added.

### Tests/docs

- `tests/jmap/` mailbox/query/core/cache/delta tests;
- `tests/app/` selection, materialization, mutation, scheduler, and full-sync tests;
- GUI model/controller tests for pending expansion and accessibility;
- keep `ARCHITECTURE.md`, `QUERY_WINDOWS.md`, `OPTIMISTIC_CONSISTENCY.md`, and
  `OFFLINE_MAIL_ARCHITECTURE.md` synchronized with the final implementation.

## Completion criteria

This project is complete when all of the following are true:

- no collapsed mailbox/search page can generate an `Email/get` whose object count is determined by
  flattened Thread membership;
- every relevant query/get/set path obeys negotiated object limits;
- a representative page is renderable before its child Threads finish hydrating;
- Thread membership and child coverage are durable, explicit, and restart-safe;
- automatic background prefetch normally makes Threads ready before user expansion without blocking
  initial page display;
- pending expansion and whole-thread commands wait for the same daemon-owned materialization state;
- partial cache coverage can never silently narrow a user command or change collapsed-row summary
  semantics;
- uncached delta tombstones are handled correctly in the bounded online cache;
- huge whole-thread mutations have defined partial-success and Undo behavior;
- complete-offline mailboxes retain their full-mirror guarantee;
- focused, full, and regression verification pass without introducing a second source of truth or a
  GUI network path.
