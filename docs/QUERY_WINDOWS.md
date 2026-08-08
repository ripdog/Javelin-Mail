# Query Windows and Infinite Scrolling

JMAP query membership and cached objects are different kinds of state. Email rows answer what is
known about an object; an ordered query window answers which representative IDs occupy a particular
server result range. The application must never reconstruct a server position by applying SQL
`OFFSET` to the subset of Email objects currently cached.

Mailbox and search windows persist the query identity, requested offset and limit, returned
position and enforced limit, total, `queryState`, and ordered representative Email IDs. The GUI
joins those IDs to cached Email data in stored order. A missing window is a cache miss even when the
database contains enough unrelated mailbox rows.

Quick search is offline-first. It captures one complete, thread-collapsed FTS result snapshot and
exposes progressively larger prefixes of that immutable snapshot as the user scrolls, so indexing
activity cannot reorder already-visible results. The UI may promote that tab once to an
authoritative server search. Promotion replaces the local snapshot with a session-scoped server
query identity; it is not a reversible display toggle.

An online search session retains the bounded server windows that have become part of the visible
list. It may also read ahead by one window so reaching the end of the loaded prefix usually requires
no network wait. A positional prefetched window is consumed only when its `queryState` matches the
preceding window. If it is missing or belongs to a different query state, continuation falls back to
a bounded anchored server request. An anchored response may legitimately advance `queryState`; in
that case the older visible prefix remains a continuity snapshot and the aggregate list is marked
stale rather than being misreported as one authoritative query generation. Closing the tab deletes
only that session's windows. Application shutdown does not delete them because restored tabs reuse
the same session identity.

Workspace persistence stores only each open list's query/session identity and a compact manifest of
loaded `(offset, limit)` windows. It never serializes message rows, totals, query states, or result
payloads into settings. On restore the session rebuilds the longest available current prefix from
SQLite; unavailable trailing windows are discarded and can be loaded again by scrolling. A wholly
missing first window triggers normal bounded materialization after the cache read.

Every watched mailbox has a canonical window: offset 0, limit 100, `receivedAt` descending, and
`collapseThreads: true`. Window validity has two independent axes. Provenance is `server`,
`locallyProjected`, or `stale`; materialization is `complete` or `partial`. Authoritative current
presentation requires a complete non-stale materialization. Server-relative continuation metadata
requires server provenance. This prevents a locally complete mutation projection from being
mistaken for authoritative query positioning, and prevents a partially materialized server response
from being shown merely because it carries a new query state. Background synchronization
materializes a missing, partial, or stale canonical window. A post-commit cache change names the
window so an open
view reloads effective SQLite state.

A GUI already presenting a complete window may retain its rows as a continuity snapshot while that
window is stale and an authoritative replacement is pending. Such a snapshot is not evidence of
current position, total, or ordered membership and cannot by itself prove the next server range. It
exists only to preserve stable selection, detail content, and viewport rather than blanking or
replacing the user's view. Reconciliation installs the replacement by stable object identity.

An Email or EmailQuery state token is not evidence that this ordered coverage exists. Push-state
deduplication may skip a background refresh when the state tokens are current and every configured
canonical mailbox window is display-current. Only `server` coverage proves authoritative remote
query positioning.

The message list has no user-visible pages. It initially presents one bounded query window and asks
for another bounded window when the viewport approaches the end of the loaded prefix. Mailbox
continuation anchors the request to the final loaded representative with `anchorOffset: 1`. This
preserves continuity when messages are inserted above the viewport. The server-returned `position`
is authoritative, and the returned representative count—not the configured window size—determines
the next logical offset. The GUI appends the committed cache rows to its virtualized list rather than
replacing the visible model. If that anchored continuation observes a newer `queryState`, the older
prefix is retained for reading continuity but the aggregate is stale until a later reconciliation.

Changing mailbox, search criteria, sort order, or another query identity resets the loaded prefix to
its first window. Refresh likewise reconciles from the beginning of the current query while retaining
useful existing rows until the replacement cache state commits. There is deliberately no first,
previous, next, last, or direct page-number interaction, and the GUI never implements infinite
scrolling by issuing an ever-growing `limit`.

For a complete offline mailbox, bounded internal windows may be resolved directly from effective
SQLite membership for every supported sort order. The canonical mailbox query state versions these
locally generated windows so a cached range is reused only while it remains current. Offline
enumeration stages one generation against a fixed mailbox query state. If that query state changes
between internal batches, the staging generation is abandoned and restarted; mixed membership
generations are never promoted. The account-wide Email state may legitimately advance because of
an unrelated mailbox or a keyword update while a large mailbox is being enumerated. Those object changes
are reconciled through `Email/changes`; they do not restart enumeration. Push catch-up updates a
completed generation's membership from the committed `email_mailboxes` delta and queues only raw
sources whose current blob is absent.

Notification navigation is not pagination. If its Email is absent from the current window, the
application requests an anchored window with `anchorOffset: 0`, persists the server-returned
position and ordered IDs, and selects the target from that committed cache state. This contextual
window does not alter the canonical window's identity or infer offsets from locally cached rows.

For a contiguous cached prefix, `Email/queryChanges` uses the final cached representative as
`upToId`, applies removals and indexed additions across every retained window, and advances all those
windows to the returned query state in one transaction. Updates are fetched only for objects already
cached, plus additions that fall into the prefix; changes outside partial coverage are harmless.
Sparse online windows that cannot be rebased exactly become stale. Optimistic Email mutations mark
affected mailbox windows and account search windows `locallyProjected` in the same
`MutationProjectionTransaction` as their projected Email state. The effective Email projection and
coverage classification are both durable across restart.

Locally projected windows retain their server-ordered IDs as a scaffold. Mailbox display is rebuilt
from effective SQLite membership, including locally decidable removals, additions, thread
representatives, metadata, and totals. Online search keeps definite removals and metadata changes;
unknown membership remains conservative. Neither kind may derive authoritative positions.
Navigation, explicit refresh, or a server change proven to affect that query replaces the
scaffold. A different account Email state alone is not proof that every query changed. Background
sync first applies one account-wide `Email/changes` delta and compares fetched objects with their
confirmed cached versions. Normal mailbox queries depend on mailbox membership, thread identity,
and received time, so read/flag keyword-only changes update their rows without `Email/query` or
`Email/queryChanges`. A locally decidable external membership change marks only its old and new
mailbox windows stale and partial. The object and mailbox counts still commit immediately, but an
open affected view performs targeted query reconciliation because an external insertion's exact
position is not proven by `Email/changes` and `Email/get`. Unaffected mailboxes are never scanned.
Successful local mutations do not automatically refresh active mailbox or search tabs. A complete
`locallyProjected` mailbox window satisfies normal materialization directly from effective SQLite
state. GUI presentation staleness never upgrades that request into a forced server query; only an
explicit user refresh may request server reconciliation, and the daemon still owns the transport.

Every `/changes` materialization must account for each requested ID exactly once in either the
corresponding `/get` list or `notFound`. A `notFound` ID is a late tombstone and is removed locally.
If `/get.state` differs from `/changes.newState`, the fetched objects are committed but the client
immediately continues `/changes` from the committed token. Duplicate, missing, unexpected, or
wrong-account materialization is not published as complete.

Totals are authoritative conversation counts for `collapseThreads: true`. Partial cached counts
are diagnostic values only and must not replace query totals. Expanded thread members and retained
message-view selections are outside query-window accounting and do not alter the loaded prefix.
