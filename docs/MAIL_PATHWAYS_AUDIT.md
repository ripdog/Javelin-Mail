# Mail pathways audit — 2026-09-05

Audited revision: `d5cf9772`. This is a static code and test review, not a runtime
verification or an implementation of the recommendations. Production code is unchanged.

The architecture is substantially sound. The best simplification is to make committed
changes and outstanding demand the organizing concepts, reducing the coordination between
independent refresh paths. Preserve the daemon/GUI split and the distinct meanings of object
state, query coverage, optimistic intent, notification consumption, and offline completeness.

## Current flow and behavior to preserve

| Decision | Current owner and behavior |
| --- | --- |
| When to synchronize | `AccountSyncCoordinator`: push tokens, explicit requests, reconnect/resume catch-up, newly watched mailboxes, and notification baseline work feed debounced/coalesced demand. Equal persisted tokens suppress already-applied push. |
| What to fetch | `MailDeltaRefreshExecutor` advances account Email/Mailbox state. Created Emails are materialized; updated Emails are fetched when tracked. Query-affecting changes select mailbox reconciliation. `MailboxRefreshExecutor` handles canonical windows; `MailQueryApplicationService` handles GUI window requests. |
| How to reconcile | SQLite transactions install server objects, state tokens, query coverage changes, and rebased mutation projections. Query results prove ordered membership independently of object-cache contents. |
| When to notify | Proven unread creation or entry into enabled mailboxes creates a per-Email consumption marker and outbox event in the Email transition transaction. Local moves/imports and initial enablement have suppression/baseline rules. |
| How to deliver | `MailNotificationService` claims and revalidates outbox events. `DaemonBackgroundController` invokes desktop delivery and acknowledges or releases claims. Delivery retries are local; startup recovers claims. |
| When to update views | Application `cacheCommitted` signals feed the invalidation publisher and daemon IPC epoch. GUI mailbox/search sessions reload effective SQLite state asynchronously and reject obsolete reads. Query request replies also trigger cache reads. |
| What stays in the background | Thread hydration, complete offline metadata/MIME mirrors, vault projection, and indexing remain daemon work. Foreground activity gates background dispatch. |

Retain infinite scrolling, query-authoritative positions/totals, stable selection and viewport
continuity, tab-owned expansion intent, quick-filter continuity, local search snapshots,
notification activation, optimistic edits and Undo/Redo, background operation without a GUI,
and complete offline mirrors with filesystem MIME storage. A cached representative is not a
complete Thread, and a complete query window is not a complete offline mailbox.

## Findings, in recommended priority order

### 1. Publish each committed delta independently of subsequent work

**High priority; concrete failure paths in the code.**

`MailDeltaRefreshExecutor.cpp:1426` commits a delta page, then recursively fetches the
continuation at line 1442. A continuation error returns only `OperationError`, discarding
the earlier page's accumulated summary. The same pattern occurs when later rebaseline work
fails. Earlier SQLite objects, tokens, and notification events remain committed.

`AccountSyncCoordinator.cpp:567` handles that error before emitting either the notification
wake-up or cache invalidation. A retry starts from the advanced cursor; it need not recreate
the earlier event and therefore need not set `notificationEventsCreated`. Startup recovery or
another new event can eventually drain the outbox, but the failed pass itself does not arrange
a local delivery retry for its already committed event.

There is a second publication gap in `refreshWatchedMailboxOnce`: the successful account
delta is accumulated while mailbox fallback/query requests run. Authentication/transient
query failures return around lines 720–725 before the final `cacheCommitted` at line 737.
An earlier successful mailbox window in the loop can likewise lose its publication when a
later mailbox fails. Successful commits can therefore be invisible until another invalidation;
slow later requests also delay ordinary count/metadata updates.

**Simplification:** make one bounded delta page return its committed effects and continuation
demand. Publish those effects before awaiting another page or query. Likewise publish each
successful query commit immediately; the existing event-loop publisher can still coalesce
adjacent invalidations. Errors describe unfinished work and never replace completed effects.
Do not hold a SQLite transaction across the network or enlarge the transaction to the entire
catch-up. Wake the outbox consumer from each committed event-producing transition.

**Regression cases:** page one creates mail, page two fails; retry finds no additional changes;
assert both UI publication and delivery of page one's event. Also commit a delta/window then
fail the next mailbox request. Cover supersession and restart between pages. Existing
`MailDeltaRefreshExecutorTest` has atomic rollback/notification tests, but these multi-step
publication cases need coverage through the coordinator and notification service.

### 2. Give overlapping query materialization one admission and commit policy

**High priority; code-level ordering risk, not reproduced in this audit.**

The coordinator's `m_refreshInFlight` protects its own loop only. GUI requests independently
enter `MailQueryApplicationService::requestMailboxWindow` (`MailApplicationService.cpp:1844`)
and `MailQueryMaterializer::queryMailboxPage` (`JmapCore.cpp:3724`). The service has no mailbox
request coalescer; its search request map tracks lifetime/retirement rather than single-flight
execution. Observing a newly opened mailbox also schedules a coordinator refresh.

The page materializer fetches, upserts representative Emails, reapplies active projections,
and replaces the window. It does not capture/check a refresh generation across that network
await. In particular, with no local mutation involved, a delayed older query response can
replace objects or ordered membership already installed by a newer delta/query. Mutation
rebasing is necessary but cannot by itself establish the ordering of two server reads.

**Simplification:** route canonical refresh and interactive materialization through one daemon
query coordinator with a full query/window identity. Coalesce equivalent requests and specify
how force-refresh and anchored navigation differ. Put a common stale-response check at the
materialization commit boundary, including overlap with account deltas. Use captured local
revision/equality evidence; never order opaque server tokens. Keep protocol execution separate
from application demand policy. A global lock around all mail work would unnecessarily block
foreground work and unrelated accounts.

**Regression cases:** opening an uncached mailbox concurrent with its observation refresh;
two requests for the same window completing in reverse order; delayed page versus a newer
account delta; force-refresh during ordinary materialization; closing an anchored view while
its response is pending. Assert final SQLite state as well as GUI state.

### 3. Bound push latency instead of restarting one shared debounce indefinitely

**Medium priority; concrete scheduling behavior.**

Every routed push invokes `scheduleDebouncedRefresh`, which unconditionally restarts the
750 ms timer (`AccountSyncCoordinator.cpp:300`, `:831`). Sustained events spaced less than
750 ms apart can indefinitely postpone synchronization. Calendar/contact/identity traffic
also shares this timer. It additionally carries endpoint retry deadlines and deferred work,
so a new push changes the wake-up time of unrelated pending demand.

**Simplification:** retain the first scheduled flush deadline while merging demand. Represent
endpoint eligibility separately and schedule the next permitted attempt without resetting the
age of pending work. Preserve the existing one-active-pass plus merged-follow-up behavior.
This can be a small local scheduler, not another durable jobs framework.

**Regression cases:** uninterrupted pushes faster than the debounce period still advance Email
state within a bounded interval; unrelated groupware traffic cannot starve mail; endpoint
backoff is respected; a push during a running pass produces a coalesced follow-up.

### 4. Use exact query identities in invalidations and request completion

**Medium priority; concrete identity loss with avoidable reload/completion coupling.**

`MailboxQueryWindowChange` (`MailApplicationTypes.h:17`) carries mailbox, offset, and limit,
but omits the query key/sort. SQLite distinguishes those queries. `MailboxSession.cpp:239`
matches committed windows using the reduced identity and can cancel its in-flight request
bookkeeping when any such match arrives. A canonical received-date commit can consequently
be mistaken for completion of another sort's window. Search invalidations already carry a
query key. `MailApplicationService.cpp:591` even reconstructs the canonical sort to discover
which window needs Thread hydration.

Both mailbox and search sessions also implement similar arbitration between invalidations,
reply completion, read generations, and pending pagination. These are cache reads, not an
extra optimistic object store, but the duplicated state machines are difficult to reason about.

**Simplification:** carry the persisted query identity through the bounded IPC contract.
Centralize window-read scheduling and completion rules in a small shared helper, retaining
separate mailbox/filter/search policy. A reply can establish the requested window's actual
position and completion; an invalidation says committed state changed. Coalesce their SQLite
reads without treating an unrelated commit as fulfillment of the request. Cache-hit replies
still need to complete even when they create no new invalidation.

**Regression cases:** two tabs for one mailbox with different sorts; invalidation before/after
reply; cached response without invalidation; anchored response changing position; closing or
changing a tab during a read. Preserve selected-message continuity and search snapshots.

### 5. Separate background-work dependencies from presentation invalidations

**Medium priority; concrete unnecessary work.**

`DaemonBackgroundController.cpp:189` interprets nearly any mail cache change as a reason to
replay maintenance, catch up offline mailboxes, and refresh the tray count. Opening a search
page or committing a query window is sufficient. `FullMailSyncService.cpp:822` then iterates
every configured offline scope for that account, opens transactions, reconciles membership,
and checks missing raw sources. The running-account dirty flag coalesces some work but does
not narrow its cause or affected mailbox set.

**Simplification:** produce semantic committed effects once: effective membership changed,
blob/source availability changed, mailbox counts changed, window coverage changed, and outbox
ready. Use the same facts to derive bounded UI invalidations and target offline/index/tray work.
Query-only changes should not imply offline membership work. Include an explicit account-wide
scope for rebaseline/recovery so narrowing never misses unseen changes. This can be an extension
of the existing change value, not a generic event bus or a second persisted object model.

**Regression cases:** loading search/continuation windows does not reconcile all offline
mailboxes; a move catches up both affected scopes; new/blob-changed Emails schedule raw-source
work; keyword-only changes update presentation without offline enumeration; startup still
recovers durable unfinished work.

### 6. Consolidate notification retry ownership without removing durable state

**Lower priority; maintainability opportunity.**

Delivery is distributed between `MailNotificationService` (`MailApplicationService.cpp:1556`)
and `DaemonBackgroundController.cpp:163`: the service retries acknowledgement/release failures,
while the controller maintains a separate 60-second account delivery retry timer. Notification
baseline retries are separately divided between account runtime configuration and coordinator
execution. These phases have different meanings and should remain distinguishable.

**Simplification:** let the notification service own the complete local delivery loop through
a narrow desktop-delivery port. Keep baseline activation with synchronization; keep persistent
consumption and the outbox. Move the existing notification implementation into its own `.cpp`
so ownership is evident. `MailApplicationService.cpp` currently contains 5,669 lines across
many already-separated classes; splitting by those existing classes is useful mechanical
cleanup, but it does not replace the behavioral fixes above.

Retain claim-time revalidation against effective membership/read state, local-only retries,
and restart recovery. Do not replace durable deduplication with cache novelty or Thread identity.
Also distinguish one durable event per Email from an exactly-once external desktop display:
a crash between external acceptance and SQLite acknowledgement crosses two systems.

## Implementation sequence and validation

### Executor contract — implementation decisions

Implement the six findings incrementally within the existing component boundaries. The
following decisions resolve the design gaps; names, helper layout, and routine code mechanics
are left to the executor. Do not introduce a generic event bus, replace the scheduler, or
redesign notification eligibility. Reduced duplication is the aim, not a mandatory LoC target.

**Committed effects (finding 1).** Make a normal delta step perform one bounded transition,
returning its committed summary plus explicit remaining domain/rebaseline work. The application
drives continuation and publishes each result before the next await. Keep rebaseline's existing
atomic promotion rules: intermediate network batches are not committed account transitions.
On supersession, retry unfinished demand, never erase previously published effects. Audit every
commit followed by a fallible read or await in the affected paths, including the page
materializer's post-commit summary read. Build response data before committing where practical;
otherwise ensure a subsequent error cannot hide the commit. Publish committed facts using the
captured account identity; run cancellation prevents further work, not publication of a commit
that already happened. Removed-account teardown must not revive its runtime. Recovery continues
to use durable SQLite state/outbox and the existing GUI reconnect barrier; no durable UI event
log is needed.

**Query admission (finding 2).** Use one daemon-side owner for mailbox/search query demand;
extend the existing query application service or a helper it owns. The account coordinator
submits canonical demand through a narrow port instead of directly owning a competing query
execution path. Preserve the incremental canonical query algorithm and bounded page algorithm.
Equivalent requests share execution and return a result to every live waiter. Equivalence
includes account, query kind/key, offset, limit, anchor, and anchor offset. An explicit refresh
cannot be satisfied by a cache hit or by a request dispatched before that refresh; merge such
refreshes into one follow-up. Anchored and unanchored requests never share merely because their
nominal offsets match. Closing a view detaches its waiter; retain existing search retirement
rules so a late response cannot recreate retired windows. Other waiters/background demand live
on independently. Foreground work promotes existing demand rather than adding another fetch.

**Commit ordering (finding 2).** First add the delayed-page/newer-delta reproducer. Coalescing
alone is insufficient: different queries share Email rows. Default to a conservative local
per-account mail-cache revision, captured before network work and checked inside the write
transaction, alongside the existing mutation fence. Advance it atomically whenever a commit
can supersede fetched Email summaries or query membership, including delta, query, Thread,
offline materialization, and relevant cleanup. Mutation generation still protects mutation
admission/settlement; do not rename or reuse it as a network-refresh counter. Put revision
handling in shared storage/consistency primitives, not scattered GUI/application bookkeeping.
Do not count unrelated Contacts, content-only downloads, or notification acknowledgements.
An invalid fence rejects the entire fetched commit as superseded and retains demand; it is
neither a transport error nor successful current coverage. Prevent overlapping stale retries
through query admission and yield between retries. Keep unrelated accounts independent. A
narrower existing guard may be reused if production-path tests prove it covers these same
writers; do not add a parallel revision mechanism unnecessarily. Never infer server-token order.

**Scheduling (finding 3).** Preserve the existing 750 ms batching interval as a maximum wait
from first pending push when no request/backoff blocks execution. Later pushes merge without
postponing that deadline. Endpoint eligibility is a separate lower bound on dispatch; do not
reset backoff for ordinary pushes. Preserve current explicit-user/network-recovery reset policy.
After an active pass, process merged demand at the earliest eligible opportunity. Use monotonic
time for deadlines. Test dispatch bounds rather than promising server completion within 750 ms.

**IPC and GUI completion (finding 4).** Window identity is account + query kind/key + persisted
offset/limit; mailbox identity remains available for broad membership invalidation. Carry the
query key through every producer, merge operation, serializer, validator, and GUI adapter.
Use the existing protocol compatibility/version mechanism for the changed wire contract; no
old/new-shape fallback. The correlated request reply owns request completion and any actual
anchored position. An exact-window invalidation schedules a cache read but does not cancel or
complete the network request. Completion plus a current cache read settles visible loading;
cache-hit completion must work without an invalidation. If an earlier read cannot be proven to
cover the reply, reread SQLite. Errors preserve useful rows and end that request's loading.
Share read coalescing/generation mechanics only where behavior is identical; no new session
inheritance framework. Retain existing reconnect/scope invalidation and stale-view behavior.

**Background dependencies (finding 5).** Extend the existing committed-change value with typed
effect scopes sufficient for these consumers. Distinguish explicit account-wide scope from an
empty affected set; bounded overflow widens scope rather than silently dropping affected IDs.
Derive effects from actual before/after effective data, not the initiating operation's name:
a query fetch that changes membership/blob data is not query-only. Offline catch-up consumes
affected old/new mailbox membership and blob changes; raw-source availability wakes relevant
hydration/index work; unread/count changes wake tray reads; queued vault projection work wakes
maintenance. Unknown scope/rebaseline/startup conservatively uses account-wide recovery.
Preserve the existing optimistic-projection exclusion for remote offline catch-up and existing
pause/retry/complete-mirror rules. Notify the same consumers after settlement when required.
UI domains remain derived presentation invalidations, not the scheduler's dependency API.

**Notification ownership (finding 6).** Move local delivery/retry policy into
`MailNotificationService`; inject the existing desktop delivery behavior through a narrow typed
port at daemon composition. Preserve current grouping, activation routes, delivery success
semantics, and retry intervals. Failed acknowledgement retries acknowledgement only; failed
release retries release before making that claim deliverable again. Neither may trigger a
duplicate delivery while locally unresolved. Baseline activation stays in account sync and
must not be merged with delivery retries. Keep durable claim recovery and consumption tables;
this change does not claim exactly-once desktop display across crashes. Move only implementation
files needed for this ownership change; broad mechanical service splitting is optional.

Each finding's regression cases above are acceptance criteria. A passing test must exercise the
real commit/publication or IPC/session path, not just a new decision helper. Complete one stage,
review for distant regressions, run the documented checks, and commit before proceeding. Do not
leave both old and new execution paths active as a transitional fallback. If a suspected race
is already prevented, record the evidence/test and omit the redundant mechanism.

1. Add failure-ordering tests and fix committed-effect publication first.
2. Bound debounce latency with deterministic scheduler/transport tests.
3. Establish shared query admission and stale-response rules, covering actual SQLite commits.
4. Carry exact window identities through IPC and simplify GUI read/completion arbitration.
5. Narrow offline/index/tray dependencies and consolidate local notification delivery ownership.
6. Mechanically split existing service implementations and refresh architecture documents.

Use the repository's documented focused checks and final full suite for each production change.
Relevant existing suites include `AccountSyncCoordinatorTest`, `MailDeltaRefreshExecutorTest`,
`MailboxRefreshExecutorTest`, `JmapCoreTest`, `MailboxSessionTest`, `SearchSessionTest`,
`GuiMailApplicationEventsTest`, `GuiDaemonSessionRecoveryTest`, `OptimisticMutationPresentationTest`,
`MailNotificationServiceTest`, `FullMailSyncServiceTest`, and `ThreadMaterializationCoordinatorTest`.
Review restart, failure, and ordering effects separately after implementation.

Documentation also needs a current-state pass: `QUERY_WINDOWS.md` still labels Thread
materialization pending although current code has the coordinator/worker, and the historical
receive-notification reliability plan contains cache-novelty/Inbox-only guidance superseded by
the later cleanup plan. Archive or clearly mark historical policy rather than letting future
changes choose between conflicting descriptions.

No build or tests were run for this documentation-only audit. Findings above distinguish
directly visible control flow from concurrency risks requiring deterministic reproduction.
