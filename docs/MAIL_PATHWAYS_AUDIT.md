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
