# Mailbox Refresh And Real-Time Update Plan

## Status

This is a historical implementation plan. Its problem statements describe the pre-query-window,
pre-push, single-process code and must not be read as the current architecture. The implemented
system now uses authoritative mailbox query windows, incremental/delta refresh executors,
daemon-owned WebSocket/EventSource state changes, typed post-commit invalidations, and stable-ID
selection restoration.

Current invariants are defined in [ARCHITECTURE.md](ARCHITECTURE.md),
[QUERY_WINDOWS.md](QUERY_WINDOWS.md), [OPTIMISTIC_CONSISTENCY.md](OPTIMISTIC_CONSISTENCY.md), and
[DAEMON_GUI_ARCHITECTURE.md](DAEMON_GUI_ARCHITECTURE.md). This document remains useful as design
history and for understanding the intended progression that produced those subsystems.

## Goal

Make mailbox refresh feel seamless in the UI and use that same sync path as the foundation for real-time updates and desktop notifications.

This milestone is specifically about:

- refreshing a mailbox without visible list flashes
- preserving selection whenever the underlying conversation still exists
- avoiding unnecessary teardown and reconstruction of the message pane
- reducing mailbox refresh latency by using a single JMAP HTTP transaction
- preparing the cache and sync model for long-poll driven real-time updates

This milestone is not yet about:

- a full push-notification transport implementation
- background service lifecycle polish
- attachment/body prefetch policy changes

## Current Problems

### Transport

The current mailbox refresh path in [src/jmap/JmapCore.cpp](/home/ripdog/CLionProjects/Javelin-Mail/src/jmap/JmapCore.cpp) performs a full collapsed-thread fetch via multiple sequential HTTP requests:

1. `Email/query`
2. representative `Email/get`
3. `Thread/get`
4. full `Email/get`

This does not yet use JMAP result references/chained methods in a single request envelope.

### Cache

The cache already has useful building blocks:

- `SyncStateRepository`
- `SyncPlanner`
- `SyncReconciler`
- `QueryService`

But the active mailbox refresh path does not yet use query-state-driven incremental refresh. It still overwrites mailbox/thread/email state more broadly than necessary.

There is also an inconsistency in sync-state query keys. Some tests use keys such as `mailbox:mbx-inbox`, while parts of `JmapCore` currently persist raw mailbox ids or empty keys. This must be normalized before incremental mailbox sync becomes reliable.

### UI

The message list and mailbox tree both refresh with `beginResetModel()` / `endResetModel()`, which causes:

- visible list flashes
- unstable selection
- message pane churn

The message pane is also explicitly refreshed after mailbox refreshes, even when the selected conversation effectively still exists.

### Selection Semantics

The message list is a collapsed-thread list, but row identity and selection currently center on `emailId`.

That is unstable for this UI. When a new message arrives in an existing thread:

- the visible conversation is still the same thread
- the representative email may change
- the current implementation can treat this as row replacement instead of row update/move

For a collapsed-thread list, the stable visible identity should be `threadId`.

## Architectural Direction

The mailbox refresh path should become:

1. determine the canonical query descriptor for the selected mailbox
2. decide whether the refresh is initial, incremental, or fallback-full
3. fetch all needed JMAP method calls in a single request envelope
4. apply cache updates transactionally
5. update the UI incrementally from SQL-backed post-refresh state
6. emit notification candidates from the same refresh result

This gives us one refresh engine that works for:

- user-triggered refresh
- mailbox change while browsing
- future long-poll driven updates

## Immediate Priority

Before pushing further on mailbox-refresh-specific transport code, add proper generic support for JMAP chained methods in the JMAP API layer.

This is a foundation task for the mailbox refresh milestone, not separate cleanup. The refresh work needs chained methods deeply enough that building them ad hoc inside `JmapCore` would create the wrong long-term abstraction.

Preferred end-state:

- an attractive typed request-builder API for multi-call JMAP request envelopes
- reusable typed support for JMAP result references
- mailbox refresh built on top of that generic builder rather than special-case JSON glue in `JmapCore`

Non-goals for this immediate step:

- building a giant abstraction for every imaginable JMAP feature before we need it
- keeping legacy request-building paths alive in parallel if the new builder cleanly replaces them

The goal is to build exactly enough general infrastructure that mailbox refresh, incremental refresh, and later real-time sync all use the same clean API.

## Chained Methods Foundation

Introduce generic chained-method support under `src/jmap/api/`.

This should be done before more mailbox-refresh-specific transport work lands.

### Required capabilities

- represent JMAP result references as typed values, not ad hoc JSON fragments
- allow `Get`-style requests to use either explicit ids or result references
- support multi-method request envelopes through a typed builder API
- keep envelope serialization generic so chained and non-chained requests share the same path

### Preferred API shape

The preferred end-state is a small typed request-builder that feels natural to compose.

Indicative direction only:

- `RequestBuilder`
- `ResultReference`
- typed `GetRequest` variants for:
  - explicit ids
  - ids sourced from an earlier method result

The intent is that mailbox refresh code should read like a sequence of typed method definitions, not like hand-built JSON wiring.

### Design rules

- generic support belongs in `src/jmap/api/`, not in `JmapCore`
- transport should remain dumb; it sends an already-built request envelope
- method-specific serializers should reuse shared result-reference support rather than each inventing their own mini format
- avoid dual APIs if one clean builder can replace the other

### First deliverable

The first deliverable should be enough generic support to express the mailbox refresh sequence as one request envelope:

1. `Email/query`
2. `Email/get` using `#ids` from `Email/query`
3. `Thread/get` using `#ids` from representative `Email/get`
4. `Email/get` using `#ids` from `Thread/get`

Once that generic support exists, mailbox refresh should be rewritten to use it immediately.

## Canonical Query Identity

Introduce a canonical mailbox query descriptor for any visible message list window.

Suggested fields:

- `accountId`
- `mailboxId`
- `sort = receivedAt desc`
- `collapseThreads = true`
- `limit`
- `offset`

Introduce a helper that converts this to a stable persisted sync key.

Suggested format:

`mailbox:<mailboxId>|sort:receivedAt:desc|collapseThreads:true|limit:<N>|offset:<N>`

Rules:

- do not persist raw mailbox ids directly as query keys
- do not mix mailbox object sync state with mailbox query state
- use exactly the same key shape in production code and tests

## Refresh Pipeline

Introduce a dedicated mailbox refresh executor in the JMAP library.

Suggested type:

- `MailboxRefreshExecutor`

Suggested responsibilities:

- build the canonical query descriptor
- inspect `SyncStateRepository`
- choose initial fetch vs incremental query refresh
- serialize chained JMAP method calls
- parse responses
- reconcile cache state
- return a refresh summary for the UI and notification layer

This should live in `src/jmap/sync/` or a nearby JMAP-library-only location, not in the GUI.

## Phase 1: Visual Stability First

### 1. Incremental message list model updates

Update [src/gui/messages/MessageListModel.cpp](/home/ripdog/CLionProjects/Javelin-Mail/src/gui/messages/MessageListModel.cpp) to stop using full model resets for ordinary refreshes.

Instead:

- load the new SQL-backed list
- diff old vs new list
- apply inserts/removes/moves/data-changed incrementally

The existing `QueryDiff` utility can be reused, but it should evolve to reflect collapsed-thread identity rather than only `emailId`.

### 2. Preserve selection by thread identity

Update the visible message-list identity to use `threadId` as the stable row key for collapsed-thread views.

Selection policy:

- if the selected thread still exists after refresh, preserve it
- if the representative email changed, keep the thread selected and update the opened email only if required
- only fall back to another row when the selected thread truly disappeared

### 3. Stop unnecessary message-pane refreshes

Adjust [src/gui/shell/MainWindow.cpp](/home/ripdog/CLionProjects/Javelin-Mail/src/gui/shell/MainWindow.cpp) so mailbox refresh completion does not blindly refresh [src/gui/messageview/MessageViewContainer.cpp](/home/ripdog/CLionProjects/Javelin-Mail/src/gui/messageview/MessageViewContainer.cpp).

Policy:

- if the selected thread remains present and the currently displayed email is still valid, do not rebuild the message pane
- if the selected email disappeared but the thread still exists, switch to the thread's new representative email without clearing the pane first
- only show the unavailable/placeholder state when the conversation truly vanished

### 4. Mailbox tree model can remain secondary

The mailbox tree may continue to use resets initially if needed. The primary user-visible pain is the center list and message pane.

If the mailbox tree still causes noticeable churn later, give it the same incremental treatment.

## Phase 2: Single HTTP Transaction For Full Mailbox Refresh

Replace the current sequential mailbox fetch sequence with a single JMAP request envelope containing chained method calls.

Target call shape:

1. `Email/query` for the mailbox window
2. `Email/get` for representative rows using a result reference to `Email/query`
3. `Thread/get` for thread ids derived from the representative `Email/get`
4. `Email/get` for all thread member emails derived from `Thread/get`

Even though the server still processes multiple methods, this should be one HTTP POST.

Benefits:

- lower latency
- less visible refresh delay
- lower transport overhead
- same logical fetch behavior as today, with reduced user-facing slowness

This phase should be implemented using the generic chained-method/request-builder API from the immediate-priority work above, not with mailbox-specific request glue inside `JmapCore`.

## Phase 3: Incremental Mailbox Refresh

After canonical query keys are in place, replace full mailbox re-fetches with query-state-driven incremental refresh.

Recommended rollout order:

1. use `Email/queryChanges` plus `Email/changes` as the incremental gate for the visible mailbox query
2. if both deltas are empty, treat refresh as a no-op and skip mailbox rebuild work entirely
3. if only object-level `Email/changes.updated` ids affect already cached messages, fetch and upsert just those messages
4. if the query membership/order changes, message creation/destruction changes thread composition, or the server returns `cannotCalculateChanges` / `tooManyChanges`, fall back to the single-transaction full mailbox fetch

This staged approach gets us immediate wins without pretending the first incremental version can already reconcile every collapsed-thread edge case.

### Initial fetch

For a mailbox with no saved query state:

- run the single-envelope full mailbox fetch
- persist the query state token
- persist any related email object state needed for follow-up object refresh

### Incremental refresh

For a mailbox with saved query state:

- issue `Email/queryChanges` for the canonical mailbox query
- fetch changed objects needed to reconcile the visible window
- issue `Email/get` only for created/updated ids that matter
- refresh `Thread/get` as needed for affected threads

Fallback policy:

- on `cannotCalculateChanges` or `tooManyChanges`, invalidate the mailbox query state and rerun the full single-envelope query bootstrap

### Cache application

Cache updates for one refresh pass should commit together.

That transaction should include:

- thread upserts
- email upserts
- email removals
- query state updates
- any derived mailbox counters if refreshed in the same pass

This avoids transient states where the query list and message view disagree.

## Phase 4: Notification Candidates

The mailbox refresh result should explicitly report newly visible arrivals.

Suggested output shape:

- inserted thread ids
- inserted representative email ids
- unread/new inbox arrivals eligible for notification

Rules:

- notification detection belongs in the JMAP library refresh path, not in ad hoc GUI diffing
- notification candidates should be derived from the pre-refresh and post-refresh visible mailbox state
- only new arrivals should notify; representative changes within an already visible thread should not produce duplicate notifications

This gives the future long-poll path a clean way to trigger desktop notifications without duplicating sync logic.

## Phase 5: Real-Time Updates

When long-poll or event-source updates arrive:

1. map changed types to affected accounts/mailboxes
2. trigger the same mailbox refresh executor
3. apply the same incremental cache reconciliation
4. emit the same notification candidates
5. let the same models update incrementally

This keeps real-time behavior and manual refresh behavior identical apart from the trigger.

## Proposed Class And API Changes

### New types

- `MailboxQueryDescriptor`
- `MailboxQueryKey`
- `MailboxRefreshExecutor`
- `MailboxRefreshSummary`
- `NotificationCandidate`
- `RequestBuilder`
- `ResultReference`

### Existing types to evolve

- `QueryDiff`
  - make collapsed-thread diffing first-class
  - support thread-stable row identity
- typed API request structs/serializers
  - support result references generically
  - support attractive multi-call request construction
- `MessageListModel`
  - apply incremental diffs instead of reset
- `MainWindow`
  - preserve selection and avoid forced message-pane refresh
- `JmapCore`
  - delegate mailbox refresh work to the new executor instead of embedding the whole flow inline
  - stop owning mailbox-specific chained-request construction

### Sync state cleanup

Normalize sync-state usage across:

- mailbox object sync
- email object sync
- mailbox query sync

Use explicit object/query semantics rather than ambiguous raw strings.

## Recommended Landing Order

### Patch 1

Add generic chained-method/request-builder support in `src/jmap/api/`.

- add reusable typed result-reference support
- add an attractive multi-method request-builder API
- move chained mailbox-fetch request construction out of `JmapCore`

This is the front-of-queue foundation for the rest of the milestone.

### Patch 2

Stabilize the message list and message pane.

- add thread-aware selection helpers
- update `MessageListModel` to diff/apply incrementally
- stop unconditional message-pane refresh on mailbox refresh completion

This should remove most of the visible flash before transport work even lands.

### Patch 3

Normalize mailbox query keys and sync-state naming.

- add canonical key helpers
- migrate existing mailbox refresh code to use them consistently
- update tests to the canonical format

### Patch 4

Refactor mailbox refresh transport into a single request envelope.

- keep behavior otherwise equivalent
- replace sequential HTTP transactions with chained methods in one POST
- express it through the generic request-builder API rather than special-case serialization

### Patch 5

Introduce `MailboxRefreshExecutor` and incremental mailbox refresh.

- initial fetch path
- incremental `Email/queryChanges` path
- fallback full requery path

### Patch 6

Emit notification candidates from mailbox refresh summaries.

- no desktop integration required yet
- just make the data available from the refresh engine

### Patch 7

Wire long-poll updates into the same refresh executor.

## Test Plan

Add or extend tests for:

- message-list diff application preserving selection across inserts/removes/moves
- collapsed-thread representative change preserving thread selection
- message pane remaining stable when selected thread survives refresh
- canonical mailbox query key generation
- single-envelope chained mailbox fetch request serialization
- incremental mailbox refresh with `Email/queryChanges`
- fallback behavior on `cannotCalculateChanges`
- notification candidate generation for new inbox arrivals only

## First Implementation Target

Start with Patch 1.

It reduces risk for every later stage because the refresh, incremental sync, and real-time update work will all build on the same clean chained-method abstraction.

Concretely, the first patch should focus on:

- introducing reusable chained-method support in `src/jmap/api/`
- introducing an attractive multi-call request-builder API
- moving mailbox chained request construction onto that new API immediately

After that, return to the UI stabilization and incremental mailbox refresh steps already outlined above.
