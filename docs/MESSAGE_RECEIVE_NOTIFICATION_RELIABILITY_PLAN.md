# Message Receive And Notification Reliability Plan

## Goal

Make the long-poll message receive path reliable by removing cache-scope mismatches,
async lifetime races, duplicate refresh paths, and stale UI propagation around live
mail updates and desktop notifications.

This plan focuses on the current path:

1. JMAP event-source update
2. watched mailbox refresh
3. SQLite cache reconciliation
4. notification candidate planning
5. desktop notification activation
6. visible mailbox/search tab refresh

## Non-Goals

- Full offline mailbox mirroring.
- Supporting multiple JMAP protocol variants or fallback transports.
- Reworking message-body or attachment fetch policy.
- Replacing the existing Qt Widgets UI model/view structure.

## Current Failure Modes

### Account-wide cache replacement from mailbox-window refresh

`MailboxRefreshExecutor` currently falls back from incremental refresh to a full
collapsed mailbox fetch, then calls account-wide replacement methods:

- `ThreadRepository::replaceAll(accountId, fetch.threads)`
- `EmailRepository::replaceAll(accountId, fetch.emails)`

Those repository calls delete all cached rows for the account, while the fetch
contains only the watched mailbox's collapsed window. This can evict unrelated
mailbox data, older pages, search-visible rows, raw-message references, and
thread records.

### Sync state can advance before cache reconciliation is durable

The full fallback path updates query sync state before replacing thread/email
cache rows. If later cache writes fail or the process exits mid-refresh, the
stored query state can say the mailbox is current while the cache is stale.

### Long-poll refresh uses mutable run context across suspension

`LongPollService::refreshWatchedMailbox()` reads `m_runContext`, awaits network
and database work, then reads `m_runContext` again when emitting signals and
notifications. A stop, restart, or settings change during the await can retarget
or clear the context.

### Overlapping live refreshes are not coalesced

Multiple state events can trigger concurrent mailbox refreshes. Competing
refreshes may issue overlapping JMAP calls and write sync/cache state in
different orders.

### Missing local updated ids are silently skipped

The incremental path fetches only `Email/changes.updated` ids that already exist
locally. If a previous broad replacement or cache miss removed an email, its
future update can be skipped while the email state still advances.

### UI invalidation is too narrow

`mailboxRefreshed` refreshes the tree and reloads the active watched mailbox
only when that exact mailbox tab is active. Inactive mailbox/search tabs for the
same account are not marked stale for ordinary live updates.

### Notification suppression is session-local

Notification de-duplication is in-memory and cleared on service stop. This is
acceptable as a secondary guard, but it is fragile while refresh summaries can be
distorted by cache replacement or restart timing.

## Target Architecture

### Product decisions

- Live reactivity should be global across the account, not Inbox-only. If mail
  arrives in another folder, mailbox counts and relevant open mailbox views
  should update promptly.
- Desktop notifications should still be generated only for newly cached unread
  mail in Inbox.
- Notification de-duplication should primarily fall out of correct cache
  reconciliation: if an email is already in the local cache, it is not new and
  should not notify. Persisted notification suppression is only worth adding if
  correct cache reconciliation still leaves a concrete duplicate case.
- Full fallback should rebuild the visible mailbox window first. Older or
  unopened windows should refetch when opened rather than forcing broad
  background cache expansion.
- Search results may remain mostly static. If cheaply available, mark inactive
  search tabs stale on account changes; do not invest heavily in background
  live search refresh yet.

### Cache scope

Mailbox refresh must never use account-wide destructive replacement unless the
operation really fetched the complete account-level object set.

Introduce narrowly named repository operations for the real intent:

- upsert fetched email summaries
- upsert fetched thread records
- remove destroyed emails by explicit id
- update mailbox membership for known affected emails
- replace a mailbox query window without deleting unrelated account data

Account-wide methods may remain for initial account bootstrap where the whole
object set was actually fetched, but call sites should make that explicit.

### Refresh transaction boundaries

Cache changes and sync-state writes for one refresh should be committed as one
logical unit.

Preferred end state:

- collect the complete reconciliation plan in `MailboxRefreshExecutor`
- apply it through one repository/transaction boundary
- write sync state last inside that transaction
- return notification candidates only after the transaction succeeds

### Long-poll context ownership

Live refresh should snapshot the run context before the first await:

- copy `std::shared_ptr<RunContext> runContext = m_runContext`
- capture `generation`
- build API context from the snapshot
- after await, verify the generation is still active before emitting signals

Signals and notifications should use only the captured context for account,
mailbox, mailbox name, API URL, and credentials.

### Global account reactivity

Long-poll state changes are account-level signals. The live service should not
assume only the watched Inbox mailbox is affected.

Preferred direction:

- keep one event-source stream per account
- on `Email` or `Mailbox` state changes, refresh mailbox metadata/counts for the
  account
- refresh open mailbox tabs for the account, prioritizing visible tabs
- run Inbox notification planning against newly cached unread Inbox emails
- avoid fetching full content for unopened mailboxes unless needed for counts or
  an open view

This keeps the UI reactive without turning the cache into a full offline mirror.

### Full fallback meaning

An incremental mailbox refresh uses JMAP state tokens to ask the server for
changes since the last known state. A "full fallback" is the recovery path when
the server cannot calculate that delta, the local state is missing, or local
cache drift makes the delta unsafe to apply.

Full fallback does not mean fetching every message in the account. In the
current implementation it means rebuilding a mailbox's collapsed query window
from `Email/query`, representative `Email/get`, `Thread/get`, and final
`Email/get`. The reliability problem is that this mailbox-window rebuild is
currently written through account-wide destructive replacement methods.

### Refresh coalescing

`LongPollService` should allow at most one live mailbox refresh at a time.

Suggested state:

- `bool m_refreshInFlight`
- `bool m_refreshAgainRequested`

If an update arrives while a refresh is running, record that another refresh is
needed and run exactly one follow-up refresh after the current one completes.

### Recovery from missing updated ids

If `Email/changes.updated` references ids missing from the local cache, the
executor should treat that as recoverable cache drift:

- fetch those ids directly when possible, or
- force a full mailbox-window refresh, or
- mark the mailbox query state as needing rebuild without advancing email state

Do not advance email state after silently ignoring missing updated ids.

### UI propagation

Live cache changes should invalidate all affected account tabs.

Suggested behavior:

- always refresh mailbox tree counts for the account
- mark inactive mailbox/search tabs for the account stale
- reload the active matching mailbox immediately
- reload stale tabs from cache/server when activated according to existing tab
  refresh policy

## Implementation Phases

### Phase 1: Guard live refresh lifetime - done

- Snapshot `RunContext` in `LongPollService::refreshWatchedMailbox()`.
- Gate post-await signal emission by generation.
- Use captured context values for `mailboxRefreshed` and `notificationRaised`.
- Add tests for restart/stop during in-flight refresh if practical with the
  existing long-poll test harness.

### Phase 2: Coalesce live refreshes - done

- Add single-flight state to `LongPollService`.
- Coalesce multiple `onUpdate()` calls into one active refresh plus one pending
  follow-up.
- Ensure cancellation/restart clears in-flight bookkeeping safely.
- Test that two rapid updates do not run two overlapping mailbox refreshes.

### Phase 3: Remove destructive mailbox fallback writes - done

- Add repository methods with mailbox/window-scoped names.
- Replace `replaceAll()` calls in `MailboxRefreshExecutor` fallback with
  upsert/reconcile operations.
- Keep account-wide replacement only for true account bootstrap paths.
- Add regression tests proving a full fallback refresh for Inbox does not delete
  cached Archive/Sent emails or unrelated thread rows.

### Phase 4: Make refresh reconciliation atomic - partially done

- Moved query/email sync-state updates to the end of the fallback path so state
  does not advance before cache writes and mailbox membership reconciliation.
- Still consider a transaction boundary that spans thread/email writes and
  sync-state updates.
- Add a failure-injection-style unit test if the existing database test helpers
  make that reasonable.

### Phase 5: Recover from local cache drift - done

- Detect `Email/changes.updated` ids missing from the local cache.
- Fetch missing ids or force a mailbox-window rebuild before advancing state.
- Add a regression test where an updated email is missing locally and verify the
  cache/state do not silently skip it.

### Phase 6: Broaden UI invalidation - done

- Extend live refresh signals or add a dedicated account/mailbox invalidation
  signal.
- Mark inactive mailbox/search tabs stale on live updates for the same account.
- Keep immediate reload for the active matching mailbox.
- Treat search tabs as static unless stale marking and refresh-on-activation are
  trivial with the existing tab machinery.
- Add focused UI-level tests only if there is already a suitable harness;
  otherwise verify manually and keep the logic small.

### Phase 7: Revisit notification de-duplication - done for current evidence

- After cache reconciliation is fixed, decide whether in-memory suppression is
  enough.
- Prefer deriving notification eligibility from cache novelty: if an email was
  already cached before the refresh, it is not notification-worthy.
- If a concrete duplicate case remains across restarts, add a small persisted
  notification table keyed by account id, mailbox id, and email id.
- Avoid suppressing genuinely new unread mail that shares a thread with an older
  notification.

### Phase 8: Remove duplicate observer drift - done

- Removed the unused `LongPollMailboxObserver` path and its dedicated tests.
- The app live path now owns refresh result interpretation and notification
  publishing.

## Testing Checklist

- Inbox fallback refresh preserves cached emails in another mailbox.
- Inbox fallback refresh preserves unrelated thread rows.
- Full fallback reports notification candidates only for genuinely new unread
  emails in the watched mailbox.
- Refresh failure does not advance query or email sync state.
- Missing local updated ids do not get skipped while advancing email state.
- Restart during in-flight refresh does not emit stale mailbox or notification
  signals.
- Multiple rapid long-poll updates do not overlap refresh writes.
- Inactive tabs become stale after live account changes.
- Notification activation still opens the target mailbox/message after refresh
  coalescing and context snapshotting.

## Open Questions

- For global reactivity, should unopened mailboxes refresh counts only, or should
  the cache maintain enough recent summaries per mailbox to open any mailbox
  instantly after an update?
