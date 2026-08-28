# Core Mail Architecture Cleanup — Implementation Plan

## Status

This is the accepted implementation checklist for the core mail architecture cleanup.

The goal is to fix the confirmed correctness problems while **reducing**, not expanding, the number
of state-transition paths in Javelin. The implementation should prefer narrow ownership rules and
existing ordering guarantees over new general-purpose synchronization machinery.

All checklist items are initially incomplete. Update this document as phases land.

### Branch handoff checkpoint — 2026-08-27

Current branch: `core-mail-architecture-cleanup`.

Implemented through the notification architecture cutover: account Email-state ownership/rebaseline,
per-Email notification consumption, baseline-gated active notification mailboxes, transactional event
creation, import/optimistic-move suppression, local-only desktop outbox delivery, legacy
notification-state retirement, and removal of notification-only mailboxes from configured
presentation query interests. The original state-token horizon representation was subsequently
removed after live testing proved that duplicating the account Email cursor could silently disable
notifications when the two tokens diverged.

Latest validation before this checkpoint:

- `javelin_jmap_sync_tests`: all 88 test cases passed (1413 assertions) after scanner removal.
- Notification-focused sync tests cover per-Email dedup/re-entry, eligibility, transactional rollback,
  baseline activation semantics, and local delivery revalidation.
- Legacy notification migration regression passed and preserves cached Email data.
- Focused daemon integration test `notification-only mailboxes are not presentation sync interests`
  passes after the Phase 4 configuration change.
- `scripts/check-debug.sh --full` has **not** yet been run on the branch; checklist items requiring it
  remain intentionally unchecked.

Next work should continue Phase 4 rather than reopening the retired notification scanner: verify
open/retained/offline mailbox interests remain intact, add request-count/fake-transport proof that a
notification-only mailbox causes no presentation `Email/query`, then continue into Phase 5 IPC cache
invalidation cleanup.

## Guiding rules

The entire implementation is constrained by these rules:

- SQLite remains the daemon-owned source of truth.
- Account-wide `Email` object state and mailbox/query state are independent and have independent
  owners.
- Query windows describe presentation coverage, never arrival history.
- New-mail notification is derived from committed Email/mailbox transitions, never reconstructed
  from whatever happens to be cached.
- New-mail notification identity belongs to the Email, not to a mailbox.
- One retained Email can generate at most one new-mail notification event.
- Desktop-notification delivery failure never causes mail synchronization.
- GUI command completion does not become a second source of cache truth.
- Prefer existing epochs/events and ordering guarantees over introducing a new general-purpose
  synchronization framework.
- Preserve existing user-facing behavior unless the existing behavior is demonstrably wrong and the
  intended replacement behavior is explicitly specified here.
- One logical change per commit, with regression tests added before or alongside each behavioral
  change.
- Do not remove an old compensating path until the replacement invariant is covered by tests and is
  actually enforced.

## Explicit non-goals

- Do not redesign the JMAP transport layer.
- Do not introduce a general distributed transaction or synchronization framework between daemon and
  GUI.
- Do not make online accounts complete offline mirrors.
- Do not make query-window recovery fetch the complete account.
- Do not make notification delivery dependent on the GUI being open.
- Do not use query-window membership as a proxy for mail arrival.
- Do not add a second durable historical notification ledger under a new name.
- Do not broaden this work into unrelated message-body, attachment, FTS, or general cache-retention
  redesign unless a required invariant cannot otherwise be satisfied.

---

# Phase 0 — Lock down current behavior and regressions

Before changing production behavior, add tests that reproduce the architectural failures and define
what must remain user-visible after the cleanup.

## 0.1 Global Email-state ownership regression

- [x] Construct an account with at least two cached mailboxes, such as Inbox and Archive.
- [x] Establish global account `Email` state `S1`.
- [x] Simulate a cached Archive Email changing on the server so account `Email` state becomes `S2`.
- [x] Perform an Inbox-only full/collapsed materialization whose representative `Email/get` reports
      state `S2`, without fetching the changed Archive Email.
- [x] Assert that the global account `Email` sync state remains `S1` after the Inbox-only
      materialization.
- [x] Run account-wide Email reconciliation from `S1` and prove the changed Archive Email is
      reconciled before the global state advances to `S2`.
- [x] Cover ordinary forced mailbox refresh.
- [x] Cover mailbox `Email/queryChanges` returning `cannotCalculateChanges`.
- [x] Cover mailbox `Email/queryChanges` returning `tooManyChanges`.
- [x] Cover a mailbox refresh whose `Email/get.state` is newer than the stored global Email state.
- [x] Cover multiple watched mailbox refreshes occurring sequentially.

The test must demonstrate that a mailbox/window fetch cannot make an unrelated cached Email
permanently stale by skipping an account-wide Email state range.

## 0.2 Notification behavioral contract

The following matrix is the user-facing notification specification. Add production-path regression
coverage for every row before replacing the current scanner.

- [x] New unread Email arrives in one notification-enabled mailbox -> exactly 1 notification event.
- [x] New unread Email simultaneously belongs to Inbox and another notification-enabled mailbox ->
      exactly 1 notification event total.
- [x] A notified unread Email is moved from Inbox to another notification-enabled mailbox -> 0
      additional notification events.
- [x] A notified unread Email is moved by the server between notification-enabled mailboxes -> 0
      additional notification events.
- [x] A notified Email is archived and later restored unread -> 0 additional notification events.
- [x] A notification event is durably queued, desktop delivery fails, and the Email is moved before
      retry -> 0 additional notification events.
- [x] A previously unnotified Email legitimately enters an active notification mailbox because of a
      new server-side mail transition -> exactly 1 notification event if it is still unread.
- [x] An Email already notified elsewhere later enters Inbox -> 0 additional notification events.
- [x] An existing read Email enters Inbox -> 0 notification events.
- [x] An old Email is manually marked unread -> 0 notification events merely because of the unread
      transition.
- [x] A user manually moves an old unread Email into Inbox -> 0 new-mail notification events.
- [x] A user imports unread Email into Inbox -> 0 new-mail notification events.
- [x] Old unread Email is discovered by complete-offline synchronization -> 0 notification events.
- [x] Old unread Email becomes visible because a query window is rebuilt/materialized -> 0
      notification events.
- [x] A mailbox query is rebuilt after `cannotCalculateChanges` -> 0 historical notification events.
- [x] An existing account is configured/bootstraped with unread mail already present -> 0 historical
      notification events.
- [x] Notifications are enabled for a populated mailbox -> 0 historical notification events.
- [x] A genuinely new eligible Email arrives after notifications are enabled -> exactly 1
      notification event.
- [x] The GUI is closed when eligible mail arrives -> the notification still occurs.
- [x] The daemon restarts after durable event creation but before desktop delivery -> exactly 1
      delivered notification total.
- [x] The same committed server delta is replayed after a crash -> 0 duplicate events.
- [x] A pending Email is marked read on another client before desktop retry -> no stale popup.
- [x] A pending Email is moved out of the notification mailbox on another client before desktop retry
      -> no stale popup.
- [x] A pending Email is destroyed before desktop retry -> no stale popup.
- [x] Desktop notification delivery fails and retries -> zero JMAP requests are caused solely by the
      retry.

These are specification tests, not optional edge cases.

## 0.3 Notification historical-cache regression

- [x] Seed an old unread Email in a notification-enabled mailbox.
- [x] Seed server-covered query-window state that establishes the historical presentation context.
- [x] Ensure no legitimate post-baseline notification event exists for it.
- [x] Trigger mailbox `Email/queryChanges` recovery through a full query-window rebuild.
- [x] Assert that merely observing/materializing the old Email in query/window state cannot create a
      notification event or consumption marker.

## 0.4 Notification retry regression

- [x] Queue a legitimate pending notification event.
- [x] Fail desktop delivery through the daemon background delivery path.
- [x] Mark the Email read and retry; assert that no popup is emitted.
- [x] Repeat with the Email moved out of the mailbox.
- [x] Repeat with the Email destroyed.
- [x] Assert that a transient local SQLite/read failure leaves the event retryable rather than
      incorrectly classifying it as ineligible.
- [x] Assert through the daemon background delivery path that these retries issue zero JMAP requests
      solely for notification delivery.

## 0.5 IPC semantic regression

While the current invalidation flags still exist:

- [x] Round-trip an optimistic-projection-only daemon change through the real IPC codec.
- [x] Remove the new-mail-only/both-flags cases when the generic wire semantic is retired.
- [x] Round-trip a change carrying neither flag.
- [x] Assert that the GUI never invents one semantic merely because another maps to
      `ChangedDomain::MessageMetadata`.

This test may later be simplified or removed if `hasNewMail` is removed from generic invalidation.

## 0.6 GUI mutation-flow regression

- [x] Add a representative optimistic mutation regression through the real daemon application service.
- [x] Assert that the daemon commits the optimistic SQLite projection.
- [x] Assert that the daemon publishes the corresponding cache invalidation before command completion.
- [x] Assert that a real mailbox session reaches the correct presentation from the cache event without
      requiring a controller-manufactured replacement refresh.
- [x] Cover mailbox-membership mutation through Archive; the source and destination sessions update
      from the authoritative projected cache path.

---

# Phase 1 — Give global `Email` state one owner

The global account Email token describes the account-wide JMAP `Email` object collection. It must not
be advanced by a mailbox/query materializer that only fetched a subset of account Emails.

## 1.1 Make the ownership invariant explicit

- [x] Document in the relevant sync interfaces that global `Email` state may be advanced only by code
      that has reconciled the locally relevant account-wide Email object delta represented by that
      transition.
- [x] Treat mailbox `EmailQuery` state as query/window-owned state only.
- [x] Treat `mailbox_query_windows`, ordered query membership, window coverage, and window provenance
      as mailbox/query-owned state only.
- [x] Ensure an arbitrary `Email/get.state` received while populating a mailbox window is not treated
      as authority to install the global account Email token.

## 1.2 Stop full mailbox materialization from advancing global Email state

- [x] Remove the global Email-state write from the full mailbox materialization path in
      `MailboxRefreshExecutor`.
- [x] Continue to commit fetched representative Emails normally.
- [x] Continue to rebase active optimistic projections against confirmed Email objects.
- [x] Keep the response Email state available only where it is useful for validation/debugging; do not
      install it as the account synchronization cursor.
- [x] Add/adjust focused tests proving a forced mailbox refresh cannot skip unrelated Email changes.

This should be a narrow correctness change, not a sync rewrite.

## 1.3 Separate account Email delta work from mailbox query delta work

- [x] Audit `refreshCollapsedMailboxThreadsIncrementally()` and related paths for bundled
      `Email/changes` ownership.
- [x] Move account-wide `Email/changes` progression into the account Email synchronizer/
      `MailDeltaRefreshExecutor` path.
- [x] Keep mailbox synchronization responsible for `Email/queryChanges`, ordered query membership,
      and bounded window materialization.
- [x] Ensure mailbox-query failure does not discard or roll back an independently valid account Email
      delta merely because both happened to be sent in one JMAP request envelope.
- [x] Ensure account Email-delta failure does not force mailbox code to invent a new global Email
      baseline from representative data.

Target responsibility split:

```text
Account Email synchronizer
    Email/changes
    Email/get for locally relevant changed/created objects
    commit object changes
    advance global Email state

Mailbox synchronizer
    Email/queryChanges
    update/rebuild ordered mailbox window
    never advance global Email state
```

## 1.4 Preserve least-request ordering in `AccountSyncCoordinator`

- [x] Reconcile account Email object state before performing query work that depends on the new object
      state.
- [x] After the Email commit, determine which tracked mailbox/query windows actually require
      reconciliation.
- [x] Avoid turning every Email metadata/keyword change into an `Email/queryChanges` request.
- [x] Preserve existing cheap paths for changes that cannot affect query membership/order.
- [x] Publish cache invalidation only for committed changes.

Logical ordering:

```text
push/catch-up indicates Email state changed
  -> reconcile account Email objects
  -> commit Email delta + global state
  -> determine affected tracked queries
  -> reconcile only required query windows
  -> publish committed invalidations
```

## 1.5 Implement explicit account Email rebaseline recovery

When account-wide `Email/changes` itself returns `cannotCalculateChanges`, `tooManyChanges`, or an
otherwise unrecoverable delta gap, recover without using mailbox representative windows as an
account-state substitute.

- [x] Define the locally relevant Email working set from SQLite.
- [x] Include cached Email rows.
- [x] Include complete-offline membership that must remain complete.
- [x] Include active query-window representatives.
- [x] Include materialized Thread children whose Email state is retained.
- [x] Include Emails required by active optimistic mutations.
- [x] Include other durable Email references whose correctness requires object reconciliation, such as
      retained raw-message/vault references, if they can outlive normal Email cache membership.
- [x] Fetch the working set in bounded `Email/get` batches respecting negotiated JMAP limits.
- [x] Account for every requested Email ID as either returned or `notFound`.
- [x] Apply returned Email replacements through normal optimistic-projection rebasing.
- [x] Reconcile confirmed destroyed/`notFound` Emails through normal deletion/membership cleanup.
- [x] Mark affected query windows stale/reconcilable where object changes invalidate their retained
      representation.
- [x] Install the new global Email state only after every locally relevant Email has been accounted
      for against that state.
- [x] Ensure cancellation/supersession cannot install a partially reconciled state token.

The key invariant is:

```text
Never install Snew until every locally relevant retained Email has been reconciled against Snew.
```

## 1.6 Keep recovery crash-safe without inventing a new framework

- [x] Keep network recovery bounded: fetch the retained working set through negotiated-size
      `Email/get` batches rather than one unbounded request.
- [x] Apply the fully accounted local rebaseline in one SQLite transaction, keeping the old global
      Email state visible until the replacement objects, projection rebases, deletions, and new state
      commit atomically.
- [x] Ensure a daemon restart can safely retry an interrupted rebaseline from the old state without
      duplicate or destructive side effects; no durable partial-recovery checkpoint is introduced.
- [x] Reuse existing synchronization generations/cancellation where sufficient; do not add a general
      transaction coordinator.
- [x] Add chunked/streaming local persistence only if measurement shows the final atomic transaction
      is a real scalability problem. No such requirement was demonstrated during this cleanup, so the
      deliberately more complex durable partial-recovery design remains unnecessary and was not
      introduced.

## 1.7 Separate initial bootstrap from steady-state mailbox refresh

Fresh account bootstrap is allowed to establish an initial account baseline, but ordinary mailbox
refresh must not retain that hidden responsibility.

- [x] Introduce or clarify an explicit initial mail-baseline/bootstrap operation.
- [x] Establish the initial global Email state deliberately as part of bootstrap.
- [x] Materialize configured initial mailbox windows through their normal query/window ownership.
- [x] Ensure steady-state `MailboxRefreshExecutor` no longer has a "sometimes establishes account
      object state" mode.
- [x] Add tests distinguishing initial bootstrap from an ordinary forced mailbox refresh.

## 1.8 Validate Phase 1

- [x] Test an Email changed outside the currently refreshed mailbox.
- [x] Test an Email destroyed outside the currently refreshed mailbox.
- [x] Test an active optimistic mutation during account rebaseline.
- [x] Test `Email/get.notFound` during rebaseline.
- [x] Test server state advancing while bounded rebaseline work is in progress.
- [x] Test refresh supersession/cancellation during recovery.
- [x] Test daemon restart/retry behavior for any durable recovery checkpoint that is introduced.
      (Not applicable: rebaseline recovery intentionally introduces no durable partial checkpoint.)
- [x] Confirm keyword-only changes still avoid unnecessary query requests.
- [x] Run the focused sync suite.
- [x] Run `scripts/check-debug.sh --full`.

---

# Phase 2 — Replace notification discovery with committed per-Email events

This is a deliberate behavioral-preservation phase. The current query-window scanner is wrong, but
its replacement must not introduce duplicate notifications when unread mail moves between
notification-enabled mailboxes.

## 2.1 Define notification identity separately from mailbox eligibility

The primary invariant is:

> A retained `(account_id, email_id)` may generate at most one new-mail notification event. Mailbox
> membership determines whether an as-yet-unnotified Email is eligible; mailbox identity does not
> create a new notification lifetime.

- [x] Treat notification identity as per-Email, not per-mailbox.
- [x] Ensure an Email simultaneously present in several notification-enabled mailboxes creates one
      event total.
- [x] Ensure moving a previously notified unread Email between any notification-enabled mailboxes
      creates zero additional events.
- [x] Ensure leaving all notification-enabled mailboxes and later re-entering does not reset the
      Email's notification history.
- [x] Ensure successful desktop delivery does not reset the Email's notification history.

## 2.2 Define what constitutes a legitimate first notification transition

Absence from the per-Email notification marker is necessary but not sufficient. The synchronization
path must also prove that the Email is part of a genuinely incoming server transition after the
relevant notification mailbox has completed its baseline.

A notification may be created only when all of these are true:

- the Email has not already consumed its notification event;
- the transition is not merely cache/query discovery or bootstrap materialization;
- the transition is not a Javelin-originated move/import that should be suppressed;
- the Email is unread after reconciliation;
- the Email belongs to at least one active notification mailbox after reconciliation.

- [x] Model this decision explicitly in the daemon synchronization/application layer.
- [x] Do not derive it from query-window insertion/removal.
- [x] Do not derive it merely from `unread && inNotifiableMailbox` on current cache state.
- [x] Do not treat arbitrary `$seen` changes as arrival.
- [x] Do not treat a user's own mailbox move as arrival when the server confirms it.

Conceptually:

```text
proven legitimate post-baseline incoming/eligibility transition?
  no  -> no event
  yes -> already consumed for this Email?
           yes -> no event
           no  -> create consumption marker + outbox atomically
```

## 2.3 Add per-Email notification-consumption state

Introduce local SQLite state conceptually equivalent to:

```sql
mail_notification_state (
    account_id   TEXT NOT NULL,
    email_id     TEXT NOT NULL,
    notified_at  TEXT NOT NULL,

    PRIMARY KEY (account_id, email_id),
    FOREIGN KEY (account_id, email_id)
        REFERENCES emails(account_id, email_id)
        ON DELETE CASCADE
)
```

The exact schema may vary, but the semantics may not.

- [x] Create the state only when Javelin creates a legitimate new-mail notification event.
- [x] Never populate it merely because an Email is downloaded, cached, indexed, opened, searched,
      offline-synchronized, or exposed by a query window.
- [x] Keep server-derived Email state separate from Javelin-local notification bookkeeping.
- [x] Enforce uniqueness at the database layer so replay/races cannot create a second consumed event
      for the same retained Email.
- [x] Tie ordinary cleanup to the Email lifetime where safe.

This table is not a renamed `observed_notification_emails`. The old table means "Javelin encountered
this Email while scanning cached presentation state"; the new table means "Javelin actually created
this Email's one new-mail event."

## 2.4 Verify Email-lifetime cleanup is safe

A simple `ON DELETE CASCADE` is desirable only if cache deletion cannot later make a still-existing
server Email look newly arrived.

- [x] Audit all paths that remove normal `emails` cache rows.
- [x] Distinguish server-confirmed destruction from local cache eviction/retention cleanup.
- [x] Prove that an Email whose notification marker cascades away cannot later be rediscovered as an
      old server Email and generate a false new-mail event.
- [x] If local Email rows can be evicted while the server object still exists, preserve notification
      deduplication independently of transient presentation/cache residency rather than naïvely tying
      it to a query window.
- [x] Add a regression test for any cache-evict-and-rediscover path that exists.

Audit result: normal production code does not evict an `emails` row while retaining the same live
server Email for later rediscovery. Email-row deletion is server-confirmed destruction/`notFound` or
local temporary-draft replacement; developer cache clearing removes memberships/windows rather than
the Email row. The consumption marker therefore follows ordinary Email lifetime safely.

Do not recreate an unbounded forever ledger merely to avoid doing this analysis.

## 2.5 Keep delivery outbox distinct from event-consumption state

The two concepts answer different questions:

```text
mail_notification_state
    "Has this Email already consumed its one new-mail event?"

mail_notification_outbox
    "Which already-created notification events still need desktop delivery?"
```

- [x] Keep outbox delivery state separate from per-Email event-consumption state.
- [x] Do not make deletion of a delivered outbox row make the Email eligible again.
- [x] Do not use mailbox ID as part of the uniqueness boundary that permits another new-mail event.
- [x] Allow the outbox to record mailbox ID as presentation/navigation/batching context only.

## 2.6 Create event-consumption state and outbox atomically with Email reconciliation

For a first legitimate eligible transition, conceptually perform:

```text
BEGIN

apply/reconcile confirmed Email state
apply mailbox membership/keywords
rebase optimistic projections

if legitimate post-baseline transition
   and unread
   and in >=1 notification-enabled mailbox
   and no mail_notification_state row:
       insert mail_notification_state
       insert mail_notification_outbox

advance global Email state

COMMIT
```

- [x] Insert the per-Email consumption row and durable outbox event in the same transaction.
- [x] Keep them in the same logical transition as the Email state and global Email-token advancement
      that proves eligibility.
- [x] Prevent a crash state where the consumption marker exists but no delivery event was queued.
- [x] Prevent a crash state where an event is queued without the consumption marker and can be queued
      again.
- [x] Replaying the same server delta after a crash must be idempotent.

## 2.7 Establish notification activation after bootstrap

Existing mail at account setup is baseline state, not newly arrived mail.

- [x] Keep notification mailboxes inactive until the account mail baseline is complete.
- [x] Ensure existing unread mail discovered during initial bootstrap creates zero notification
      events.
- [x] Ensure the baseline does not depend on scanning query windows into an observation ledger.
- [x] Activate the configured notification mailbox set atomically with the completed Email baseline.
- [x] Ensure subsequent genuine post-baseline Email transitions can notify normally.
- [x] Cover complete-offline historical mailbox enumeration and prove it cannot manufacture new-mail events.

## 2.8 Activate newly enabled notification mailboxes only after a fresh baseline

Enabling notifications for a populated mailbox must not notify existing mail.

- [x] Fence in-flight Email reconciliation before baselining a newly enabled notification mailbox.
- [x] Keep newly enabled mailboxes out of the active notification set while the baseline is running.
- [x] Atomically replace the active notification mailbox set in the final Email-baseline transaction.
- [x] Do not duplicate the account Email state token in notification storage; `sync_state.Email` is
      the sole account Email cursor.
- [x] Do not require inserting per-Email consumption markers for every historical Email merely to
      establish the baseline.
- [x] Ensure the first genuine post-activation incoming transition notifies normally; activation is
      the committed completion of the enablement baseline, not the settings-toggle instant.
- [x] Cover notification settings changes while synchronization is in flight.

## 2.9 Preserve local-operation provenance

Javelin-originated state changes must not come back from the server and masquerade as incoming mail.

- [x] Preserve enough operation/mutation provenance to suppress notification creation for the user's
      own mailbox moves when server confirmation arrives.
- [x] Cover Move, Archive, Restore, Junk, Not Junk, and mailbox add/remove mutations.
- [x] Ensure optimistic projection does not delete or reset existing per-Email notification state.
- [x] Ensure server reconciliation of an optimistic move preserves existing per-Email notification
      state.
- [x] Prefer existing optimistic journal/application-command provenance over a new generic origin
      tracking framework.

## 2.10 Suppress local imports explicitly

User-imported mail is not incoming new mail even if the server reports newly created JMAP Email
objects.

- [x] Carry import provenance through the existing import/application-operation machinery far enough
      for account Email reconciliation to suppress new-mail event creation for Javelin-originated
      imports.
- [x] Do not solve this by relying on query-window history.
- [x] Do not generate one notification per imported unread message.
- [x] Add a regression test importing a large unread batch into a notification-enabled mailbox and
      assert zero notification events.

## 2.11 Handle multiple qualifying mailboxes deterministically

A single Email may belong to several notification-enabled mailboxes. It still creates one event.

- [x] Attribute the event to one mailbox deterministically for wording/navigation/batching.
- [x] Prefer the mailbox whose legitimate incoming membership transition made the Email eligible when
      unambiguous.
- [x] Otherwise prefer a primary Inbox-role mailbox when present.
- [x] Otherwise choose a deterministic notification-enabled mailbox.
- [x] Ensure attribution never changes event uniqueness.
- [x] Ensure batching does not duplicate one Email because it has multiple qualifying memberships.

## 2.12 Remove query-window-based notification discovery

Once the new path is production-tested:

- [x] Remove `NotificationRepository::enqueueUnreadMailboxEmails()` or reduce it so it no longer
      discovers notification events by scanning cache/query state.
- [x] Remove the `mailboxRefreshed -> scan cached windows -> infer new mail` architecture.
- [x] Remove `observed_notification_emails` from active notification logic.
- [x] Audit `RefreshNotificationPlanner` and `notificationCandidates`; delete notification planning
      that duplicates the authoritative committed-Email transition path.
- [x] Ensure a full/continued/query rebuild cannot create notification events.
- [x] Ensure complete-offline synchronization cannot create historical notification events.

## 2.13 Migrate legacy notification state safely

The existing observation/outbox history contains historical false positives and should not become
new architectural truth.

- [x] Add a database migration for the new notification-consumption state/outbox schema as required.
- [x] Do not mechanically translate `observed_notification_emails` rows into the new semantic marker.
- [x] Remove or retire the legacy observation table once no production code depends on it.
- [x] Discard old delivered outbox history rather than preserving historical false positives forever.
- [x] For legacy pending rows, either validate them against current Email state and retain only
      provably eligible events, or discard them if their provenance is ambiguous.
- [x] Do not destroy Email/mailbox data as part of this migration.

## 2.14 Validate Phase 2

- [x] Run the full Phase 0 notification behavior matrix.
- [x] Add race coverage for duplicate server delta replay.
- [x] Add concurrent/simultaneous membership coverage for multiple notification-enabled mailboxes.
- [x] Add optimistic move + server confirmation coverage.
- [x] Add notification enablement during synchronization coverage.
- [x] Add bootstrap/restart coverage.
- [x] Run focused notification/sync tests.
- [x] Run `scripts/check-debug.sh --full`.

---

# Phase 3 — Make desktop notification delivery a purely local outbox consumer

Once Phase 2 owns event creation correctly, desktop delivery should be a small local state machine.
It must never request network synchronization merely to retry a popup.

## 3.1 Separate event creation from delivery

- [x] Keep mail synchronization responsible for creating durable notification events.
- [x] Keep the desktop notification controller responsible only for claiming/delivering pending
      outbox events.
- [x] Remove any delivery-path assumption that another mailbox refresh is needed to discover pending
      work.

## 3.2 Retry directly from SQLite

On desktop delivery failure:

- [x] Release or expire the dispatch claim locally.
- [x] Schedule a local retry timer.
- [x] Re-read pending outbox events from SQLite at retry time.
- [x] Do not call `requestAccountSynchronization()`.
- [x] Do not call mailbox synchronization merely to retry delivery.
- [x] Assert through tests that delivery retry remains entirely inside the SQLite-backed notification
      service/background retry path, with no JMAP transport or account-sync dependency.

## 3.3 Revalidate current eligibility before every dispatch

Immediately before showing a pending notification:

- [x] Verify the Email still exists.
- [x] Verify it still belongs to the event's currently relevant notification context, or determine a
      valid current navigation context if attribution is allowed to move.
- [x] Verify it is still unread.
- [x] If it is known no longer eligible, cancel/remove the pending delivery event.
- [x] If local state cannot be read because of a transient SQLite/local failure, keep the event
      pending for retry instead of treating uncertainty as ineligibility.

Per-Email consumption state remains consumed even if the pending popup is cancelled because the Email
was read/moved before delivery. Reading the mail elsewhere must not make it eligible for a future
"new" popup.

## 3.4 Preserve existing batching intentionally

- [x] Batch pending outbox events using the existing user-facing notification style where practical.
- [x] Ensure each Email contributes at most once to a batch.
- [x] Keep a representative actionable target where the platform supports notification actions.
- [x] Continue routing Archive/Mark Read/Reply actions through the normal daemon application command
      and optimistic-consistency paths.
- [x] Do not let batching become a second event-discovery mechanism.

## 3.5 Bound delivery history

- [x] Prefer deleting successfully delivered outbox rows after durable success acknowledgement.
- [x] If a diagnostic delivered history is retained, bound it by time/count and purge it through local
      maintenance.
- [x] Ensure correctness does not depend on retaining delivered outbox rows forever.
- [x] Ensure the per-Email notification-consumption state, not delivered outbox history, enforces
      duplicate prevention.

## 3.6 Recover pending delivery after daemon restart

- [x] Recover or expire stale dispatch claims at daemon startup.
- [x] Re-read pending outbox events locally.
- [x] Revalidate eligibility.
- [x] Retry delivery without requesting account synchronization solely for notification recovery.
- [x] Prove crash-after-commit-before-popup results in exactly one eventual popup, not zero or two.

## 3.7 Validate Phase 3

- [x] Run notification retry/read/move/destroy regression tests.
- [x] Run daemon restart/claim recovery tests.
- [x] Confirm notification actions still work while the GUI is closed.
- [x] Confirm desktop delivery failure/retry remains local and generates no JMAP work solely for
      notification recovery.
- [x] Run `scripts/check-debug.sh --full`.

---

# Phase 4 — Remove notification-only mailboxes from presentation query observation

Today notification configuration contributes watched mailbox/query interests because the scanner
needs query windows. Once notification eligibility comes from committed Email transitions, this
coupling is unnecessary.

## 4.1 Separate notification configuration from presentation interests

- [x] Stop unioning notification-only mailbox selections into watched mailbox/query-window interests
      solely for notification discovery.
- [x] Keep open/retained mailbox tabs and explicit offline synchronization as legitimate presentation
      or storage interests.
- [x] Keep notification mailbox configuration available to Email transition eligibility logic without
      forcing an `Email/query` window.
- [x] Preserve existing behavior for a mailbox that is both notification-enabled and legitimately
      watched for presentation/offline reasons.

## 4.2 Verify least-request behavior

- [x] Enabling notifications for an unopened online mailbox must not warm a canonical presentation
      query window solely for notifications.
- [x] Incoming mail for a notification-only mailbox must still be eligible for notification through
      account Email synchronization.
- [x] Opening the mailbox later materializes its normal query window through existing tab/session
      policy; a cache-miss `MailboxSession` requests its canonical window independently of notification configuration.
- [x] Confirm mailbox counts/metadata that are independently required remain correct without using
      message query windows as notification infrastructure; account mailbox-state synchronization remains independent.

## 4.3 Validate Phase 4

- [x] Add request-count/fake-transport coverage showing notification-only configuration does not cause
      unnecessary query-window traffic.
- [x] Re-run the notification behavior matrix with GUI closed; daemon-only delivery/action tests and the daemon sync matrix require no GUI process.
- [x] Run `scripts/check-debug.sh --full`.

---

# Phase 5 — Make cache invalidation semantics precise across IPC

Do this after notification behavior no longer depends on generic `hasNewMail` cache invalidation.
Keep the protocol change narrow.

## 5.1 Remove `hasNewMail` from generic invalidation if no longer needed

- [x] Audit all remaining consumers of `MailCacheChange::hasNewMail`.
- [x] If notification discovery is its only real semantic consumer, remove it from generic cache
      invalidation.
- [x] Keep the daemon-internal indexing trigger precise: `emailObjectsChanged` describes committed
      Email-row changes without carrying or implying notification eligibility.
- [x] If some UI still needs an explicit new-mail event, model that as a dedicated typed event rather
      than inferring it from generic metadata invalidation.
- [x] Ensure no GUI code reconstructs "new mail" from `ChangedDomain::MessageMetadata`.

## 5.2 Preserve `optimisticProjection` explicitly only if useful

- [x] Audit whether GUI/session consumers genuinely need to know that a committed cache state contains
      an unresolved optimistic projection.
- [x] If they do, add/retain an explicit IPC field and round-trip it faithfully.
- [x] If they do not, remove the dead semantic rather than deriving it from `hasNewMail` or metadata
      domains. (Not applicable: list sessions use it to identify projected metadata changes.)
- [x] Never set `optimisticProjection = hasNewMail` as a reconstruction shortcut.

## 5.3 Stop manufacturing fake changed domains

- [x] Remove the fallback that emits `MailQueryWindows` when the actual changed-domain set is empty.
- [x] Add a precise `MailTags` or equivalent domain if tag-definition changes require GUI cache
      invalidation.
- [x] Do not emit a fake mail-query invalidation for cache changes with no query-window effect.
- [x] Add focused codec/consumer tests for every new or changed domain.

## 5.4 Keep the wire shape small

- [x] Retain the existing useful typed fields such as account ID, mailbox IDs, mailbox/search windows,
      message-content Email IDs, changed domains, and epoch.
- [x] Add/remove only fields required to preserve real semantics.
- [x] Do not turn this phase into a wholesale cache-event protocol redesign.

## 5.5 Validate Phase 5

- [x] Run IPC round-trip tests.
- [x] Run GUI invalidation/session tests.
- [x] Confirm notification behavior is unchanged because it no longer depends on generic cache
      invalidation.
- [x] Run `scripts/check-debug.sh --full`.

---

# Phase 6 — Remove GUI command completion as a second presentation-update path

The daemon already owns optimistic cache projection. The GUI should not independently recreate the
same state when a command future completes. Avoid a generalized barrier unless the existing epoch
and socket ordering genuinely cannot provide the required guarantee.

## 6.1 Determine and test actual event ordering first

Instrument/test a representative optimistic mutation:

```text
daemon:
    commit optimistic SQLite projection
    allocate/publish cache invalidation
    complete remote action

GUI:
    receive invalidation
    receive action completion
```

- [x] Record the current ordering guarantees at the daemon IPC boundary.
- [x] Determine whether the invalidation can be synchronously allocated/enqueued before command
      completion without changing user-visible latency.
- [x] Determine whether socket delivery can allow the command reply to be observed before its
      invalidation.
- [x] Do not introduce a new barrier framework before this test demonstrates a need.

## 6.2 Prefer synchronous invalidation epoch reservation/publication

The desired smallest contract is:

```text
commit cache projection
  -> synchronously allocate/publish invalidation epoch E
  -> complete command carrying/associated with E
```

- [x] Reuse the daemon's existing cache/boundary epoch if it can represent this ordering.
- [x] Ensure the epoch associated with command completion is post-commit, not the previous
      `m_currentEpoch()` observed before the pending zero-timer invalidation is allocated.
- [x] Keep optimistic state visible immediately from the committed local cache event; do not wait for
      server `Email/set` settlement.

## 6.3 Gate remote command completion on the existing epoch only if necessary

If the command reply can arrive before invalidation `E` at the GUI:

- [x] Temporarily hold that command completion until the GUI has observed invalidation epoch `>= E`.
      (Not required: mutation invalidations are synchronously queued first.)
- [x] Apply this only to projection-producing actions that need presentation ordering. (Not
      applicable because no completion gate is required.)
- [x] Reuse existing timeout/daemon-recovery behavior. (Not applicable because no completion gate
      is required.)
- [x] Do not introduce a general-purpose distributed consistency/barrier subsystem.

Ordering result: mutation services emit only after committing their SQLite projection. Their direct
Qt connection now flushes `CacheInvalidationPublisher` synchronously; `DaemonProcess` allocates the
authoritative boundary epoch and queues the boundary event before the remote-action coroutine can
publish completion. Local socket event writes preserve that enqueue order, so a second GUI-side
completion barrier would add machinery without closing a real ordering gap.

## 6.4 Remove GUI-local projection refreshes

Once ordering is proven:

- [x] Remove controller-driven `refreshMessageListPreservingSelection()` calls whose only purpose is
      to mirror the daemon's committed optimistic projection.
- [x] Remove direct `MessageListModel::setEmailRead(...)` mutation used as a second projection path.
- [x] Remove duplicate expanded-thread refreshes caused solely by command completion when normal
      cache invalidation already supplies the committed state.
- [x] Remove/reduce `mailboxMembershipChanged`, `messageMetadataChanged`, `junkStateChanged`, and
      `emailMarkedRead` presentation effects where they duplicate cache truth.
- [x] Retain command-completion signals only for status/error/action lifecycle effects that are not
      cache projection.
- [x] Ensure search-tab staleness derives from authoritative cache invalidation rather than a parallel
      command-result heuristic.

Target path:

```text
user action
  -> daemon application command
  -> optimistic SQLite projection
  -> authoritative cache invalidation
  -> MailboxSession/SearchSession
  -> presenter/model
```

## 6.5 Preserve responsiveness

- [x] Verify mutation-to-visible-projection latency remains effectively immediate.
- [x] Confirm GUI presentation does not wait for remote JMAP mutation settlement.
- [x] Confirm failure/retry/rollback paths still flow through the optimistic-consistency journal and
      authoritative cache invalidation.
- [x] Add regression coverage for rapid successive mutations so removing the duplicate GUI refresh
      path does not reintroduce flapping or stale selection.

## 6.6 Validate Phase 6

- [x] Run the GUI mutation-flow regression tests from Phase 0.
- [x] Cover mark read/unread.
- [x] Cover flag/tag changes.
- [x] Cover archive/delete/move/copy where applicable; permanent destroy intentionally remains visible until authoritative settlement, matching its existing non-optimistic contract.
- [x] Cover Junk/Not Junk.
- [x] Cover rapid optimistic actions and rollback/failure.
- [x] Run `scripts/check-debug.sh --full`.

---

# Phase 7 — Narrow list-session invalidation dependencies

This is a safe performance/stability cleanup after the larger ownership changes.

## 7.1 Define explicit session dependencies

- [x] Enumerate the cache domains/window identities that can change a `MailboxSession` result.
- [x] Enumerate the cache domains/window identities that can change a `SearchSession` result.
- [x] Include relevant mailbox/query window replacement.
- [x] Include relevant Email metadata/membership represented in loaded rows.
- [x] Include Thread materialization changes where they affect represented/expanded rows.
- [x] Include tag-definition changes if row rendering depends on them.
- [x] Exclude unrelated Contacts, Calendar, Identities, and unrelated message-content downloads.

## 7.2 Advance refresh generation only for relevant invalidations

- [x] Move `refreshGeneration.begin(...)` behind dependency/intersection checks.
- [x] Keep cache epoch observation separate from cancellation of an in-flight list read if necessary.
- [x] Ensure a same-account irrelevant invalidation cannot cause needless read cancellation/retry.
- [x] Keep relevant optimistic metadata changes capable of invalidating the session normally.

## 7.3 Validate Phase 7

- [x] Contact invalidation does not restart an Inbox list read.
- [x] Calendar invalidation does not restart an Inbox list read.
- [x] Unrelated message-content invalidation does not restart an Inbox list read.
- [x] Relevant mailbox query-window replacement does restart/reload as required.
- [x] Relevant Thread completion does refresh represented expansion state.
- [x] Relevant optimistic metadata update does refresh the row.
- [x] Run `scripts/check-debug.sh --full`.

---

# Phase 8 — Move expanded-Thread presentation state to the owning tab/session

`MessageListModel` is a reusable global presentation model. Expanded Thread intent belongs to a
mailbox/search tab or session rather than to the reusable model's lifetime.

## 8.1 Store expansion intent per tab/session

- [x] Add `expandedThreadIds` or equivalent to mailbox/search tab presentation state.
- [x] Keep the model capable of applying expansion/collapse, but do not make it the sole durable owner
      of which Threads a tab intends to keep expanded.
- [x] Preserve existing mailbox-scoped/search-scoped Thread materialization semantics.

## 8.2 Restore by stable identity

- [x] On tab rebind, intersect retained expanded Thread IDs with Threads represented by the current
      query/session.
- [x] Restore valid expansions.
- [x] Discard expansion IDs no longer represented.
- [x] Ensure changing query identity/filter semantics does not restore expansion into an unrelated
      result set.
- [x] Ensure closing the tab naturally drops its expansion state.

Expected user-visible behavior:

```text
expand Thread in Inbox
  -> switch to another workspace tab
  -> return to Inbox
  -> expansion is preserved when still valid
```

## 8.3 Validate Phase 8

- [x] Cover mailbox tab -> Calendar/Contacts/Compose -> mailbox tab.
- [x] Cover mailbox A -> mailbox B -> mailbox A.
- [x] Cover search tab expansion state.
- [x] Cover Thread disappearing while the tab is inactive.
- [x] Run `scripts/check-debug.sh --full`.

---

# Phase 9 — Remove dead or deceptive composition

Do this only after functional ownership has settled.

## 9.1 Remove unused daemon message-list session composition

- [x] Re-audit daemon consumers of `MessageListSessionFactoryService`.
- [x] If there is still no daemon consumer, remove its daemon construction/exposure.
- [x] Keep GUI message-list sessions in `GuiServices`, backed by daemon materialization and read-only
      SQLite access as currently intended.
- [x] Update architecture documentation/comments so process ownership is obvious.

## 9.2 Rationalize duplicate epoch concepts only if it is genuinely simpler

- [x] Re-audit the `CacheInvalidationPublisher` internal epoch and daemon boundary epoch after Phase 6.
- [x] If one has no independent semantic consumer, collapse to one authoritative commit/invalidation
      epoch.
- [x] If both still have legitimate responsibilities, document them and leave them alone. (Not
      applicable: the publisher epoch had no independent consumer and was removed.)
- [x] Do not expand scope for aesthetic purity; the two separately identified follow-ups below remain out of this cleanup.

## 9.3 Validate Phase 9

- [x] Build all daemon and GUI targets.
- [x] Run architecture/unit tests affected by service construction.
- [x] Run `scripts/check-debug.sh --full`.

---

# Phase 10 — Separate follow-up audits, not blockers

These items were noticed during the audit but should not be bundled into the core ownership cleanup
unless a prior phase proves they are required.

## 10.1 FTS metadata freshness follow-up

- [x] Confirm whether an Email subject can change while the raw MIME/content hash used by the index
      remains unchanged.
- [ ] If confirmed, give the FTS document a freshness identity that covers every indexed input, such
      as raw-body hash + indexed metadata/version + index-format version.
- [x] Keep this as a separate logical commit/project.

Audit result: `MailIndexService` selects stale work by raw-vault `content_hash` while the indexed
document also contains the independently synchronized JMAP Email subject. A subject-only server
change can therefore leave the FTS subject stale. The freshness-identity correction remains a
separate follow-up by design and is not part of this architecture cleanup.

## 10.2 Targeted message-content recovery follow-up

- [x] Audit the path where raw/message-content fetch returns 404 or changes while downloading and
      currently triggers account-wide synchronization.
- [ ] If practical, replace that broad recovery with targeted Email reconciliation.
- [x] Keep this separate from the state-ownership and notification changes above.

Audit result: `MessageContentClient` returns `MessageContentUnavailable` for an HTTP 404, and
`MessageContentApplicationService::requestMessageContent()` currently responds by requesting an
account-wide synchronization. Replacing that recovery with a targeted Email reconciliation requires
a separate application-port change and remains intentionally outside this cleanup.

---

# Migration and compatibility checklist

## Notification migration

- [x] Introduce the new notification-consumption representation through a normal schema migration.
- [x] Do not reinterpret `observed_notification_emails` as proof that an Email already generated a
      legitimate notification event.
- [x] Remove/retire the old observation table after the scanner is gone.
- [x] Delete historical delivered outbox rows that have no correctness purpose.
- [x] Validate or discard ambiguous legacy pending rows rather than surfacing historical false
      positives.
- [x] Preserve all normal Email/mailbox data.

## IPC compatibility

- [x] Follow Javelin's existing same-version daemon/GUI protocol expectations.
- [x] Do not introduce mixed-version compatibility machinery unless the project already promises that
      behavior elsewhere; incompatible peers continue to be rejected by the existing handshake.
- [x] Keep protocol changes narrowly scoped and test codec round trips.

---

# Required end-state invariants

Every item below must be directly testable before this project is considered complete.

- [x] A mailbox query/window refresh cannot advance the account-wide `Email` state token.
- [x] Account `Email` state advances only after all locally relevant retained Email objects represented
      by that transition have been reconciled.
- [x] A query window becoming populated can never by itself create a new-mail notification.
- [x] Complete-offline synchronization of historical mail creates zero historical new-mail
      notifications.
- [x] Enabling notifications on an existing populated mailbox creates zero immediate historical
      notifications.
- [x] Initial account bootstrap creates zero historical new-mail notifications.
- [x] A legitimate post-baseline unread Email transition into notification eligibility creates one
      durable notification event.
- [x] One retained Email can generate at most one new-mail notification event.
- [x] Moving an already-notified unread Email between any number of notification-enabled mailboxes
      creates zero additional notification events.
- [x] An Email simultaneously belonging to multiple notification-enabled mailboxes creates one event,
      not one per mailbox.
- [x] Mailbox ID is notification metadata/context, not notification uniqueness identity.
- [x] Successful desktop delivery may delete the outbox row without making the Email notification-
      eligible again.
- [x] Per-Email notification-consumption state is created only when a legitimate event is created;
      cache observation never creates it.
- [x] User-initiated mailbox movement cannot generate a new-mail event when later confirmed by server
      synchronization.
- [x] User-initiated mail import cannot generate new-mail events for the imported historical mail.
- [x] Replaying the same server transition after a crash cannot create another notification event.
- [x] Notification deduplication survives optimistic mailbox movement and subsequent server
      reconciliation.
- [x] A crash after Email/event transaction commit but before desktop delivery results in eventual
      delivery of that one event.
- [x] A crash before Email/event transaction commit creates neither a partially consumed event nor a
      detached outbox event.
- [x] Desktop notification delivery failure performs zero JMAP requests solely for retry.
- [x] A pending notification for an Email subsequently read, moved out, or destroyed is cancelled
      rather than displayed.
- [x] Delivered notification history is bounded.
- [x] Notification-only mailbox configuration does not require presentation query windows.
- [x] The GUI never infers "new mail" from generic metadata invalidation.
- [x] An optimistic mail mutation has one presentation data path: daemon SQLite projection -> cache
      invalidation -> session -> model.
- [x] Command completion does not manually recreate the optimistic cache projection in the GUI.
- [x] Contact/Calendar/Identity/unrelated-content activity cannot unnecessarily invalidate an
      unrelated in-flight mailbox list read.
- [x] Calendar/Contacts/Compose tab activation cannot clear or mutate latent mail presentation state.
- [x] Expanded Thread intent belongs to the relevant mail tab/session rather than the reusable global
      message-list model.
- [x] Query/window state, Email-object state, notification-consumption state, notification-delivery
      state, and GUI presentation state each have one clearly defined owner.

---

# Suggested commit sequence

Keep the work reviewable. One item below should normally correspond to one logical commit; split an
item further if implementation naturally contains independently reviewable behavioral changes.

- [x] 1. Add global Email-state ownership regression tests.
- [x] 2. Stop full mailbox refresh from advancing global Email state.
- [x] 3. Separate account Email delta progression from mailbox query delta progression.
- [x] 4. Add safe bounded account Email rebaseline recovery.
- [x] 5. Separate initial bootstrap Email-state establishment from steady-state mailbox refresh.
- [x] 6. Add the complete notification behavioral-contract regression matrix.
- [x] 7. Add per-Email notification-consumption state with safe lifetime cleanup semantics.
- [x] 8. Add explicit notification baseline activation for bootstrap and notification enablement;
      notification storage must not duplicate the account Email cursor.
- [x] 9. Generate one per-Email notification event from proven committed Email transitions,
      atomically with Email-state commit.
- [x] 10. Preserve/suppress notification provenance across local mailbox mutations and imports.
- [x] 11. Add deterministic mailbox attribution/batching for single per-Email events.
- [x] 12. Remove query-window notification discovery and `observed_notification_emails`.
- [x] 13. Make notification delivery retries local and revalidate pending events.
- [x] 14. Remove notification-only mailboxes from query-window watch interests.
- [x] 15. Bound/purge notification delivery history and complete legacy migration cleanup.
- [x] 16. Remove `hasNewMail` from generic invalidation or replace its remaining use with a precise
      typed event.
- [x] 17. Preserve or remove `optimisticProjection` explicitly across IPC; never infer it from another
      semantic.
- [x] 18. Remove fake cache domains/add a precise tag invalidation domain if required.
- [x] 19. Establish projection invalidation-before-command-completion ordering using the existing
      epoch where possible.
- [x] 20. Remove GUI command-completion projection refresh/model mutation paths.
- [x] 21. Narrow mailbox/search session invalidation dependencies.
- [x] 22. Move expanded Thread presentation intent to tab/session ownership.
- [x] 23. Remove unused daemon message-list session composition if still dead.
- [x] 24. Rationalize duplicate epochs only if the result is demonstrably simpler.

---

# Target architecture

The intended end state has fewer ownership paths than the current implementation:

```text
JMAP server state
       |
       v
account Email synchronizer ----------------------+
       |                                         |
       |                                         v
       |                              notification eligibility
       |                                         |
       |                              per-Email consumption marker
       |                                         + outbox
       |                                         |
       v                                         v
SQLite confirmed + optimistic Email truth   local desktop delivery
       |
       +-------------------------+
       |                         |
       v                         v
mailbox/search query       authoritative cache invalidation
reconciliation                   |
       |                         v
       +--------------------> GUI sessions
                                 |
                                 v
                           presentation model
```

The system should no longer contain these compensating loops:

```text
query-window observation -> infer arrival -> observation ledger -> notification

desktop notification failure -> force account sync -> rescan query windows

command completion -> manually reload/mutate GUI model
    while
cache invalidation -> independently reload the same model

mailbox representative fetch -> install account-wide Email state
```

The cleanup is complete when those loops are gone and each remaining state transition has one clear
owner and one tested user-facing meaning.
