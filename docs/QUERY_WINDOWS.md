# Query Windows and Pagination

JMAP query membership and cached objects are different kinds of state. Email rows answer what is
known about an object; an ordered query window answers which representative IDs occupy a particular
server result range. The application must never reconstruct a server position by applying SQL
`OFFSET` to the subset of Email objects currently cached.

Mailbox and search windows persist the query identity, requested offset and limit, returned
position and enforced limit, total, `queryState`, and ordered representative Email IDs. The GUI
joins those IDs to cached Email data in stored order. A missing window is a cache miss even when the
database contains enough unrelated mailbox rows.

Quick search is offline-first. It captures one complete, thread-collapsed FTS result snapshot and
paginates only that immutable snapshot, so indexing activity cannot reorder later pages. The UI
may promote that tab once to an authoritative server search. Promotion replaces the local snapshot
with a session-scoped server query identity; it is not a reversible display toggle.

An online search session retains its fetched windows until the tab is explicitly closed and reads
ahead after the visible window commits. Small result sets acquire every server window in the
background; large result sets retain a bounded read-ahead. Prefetch may extend a manifest only
while every response has the initial `queryState`. A different state stops acquisition and marks
the session stale rather than combining ranges from different ordered queries. Closing the tab
deletes only that session's windows. Application shutdown does not delete them because restored
tabs reuse the same session identity.

Every watched mailbox has a canonical window: offset 0, limit 100, `receivedAt` descending, and
`collapseThreads: true`. Background synchronization must materialize that window whenever it is
missing or invalid; caching Email objects without the matching authoritative membership window is
not a successful mailbox materialization. A post-commit cache change names the canonical window so
an open view reloads it from SQLite.

An Email or EmailQuery state token is not evidence that this ordered coverage exists. Push-state
deduplication may skip a background refresh only when the state tokens are current and every
configured canonical mailbox window is authoritative.

Sequential navigation anchors the next request to the final representative in the visible window.
This preserves continuity when messages are inserted above the viewport. The returned `position`
is authoritative, and the returned ID count—not the requested page size—determines the next logical
offset. Previous-page and invalid-offset recovery use the server-enforced limit and refresh the
target window.

First-page, last-page, and direct page-number navigation use a positional `Email/query`, calculated
from the server-enforced limit and the authoritative total. These jumps do not fetch intermediate
pages. Unlike anchored next-page navigation, positional jumps identify a result range rather than a
stable boundary Email, so concurrent insertions or removals may change which conversations occupy
the requested page before its query executes.

For a complete offline mailbox, the same controls resolve missing windows directly from effective
SQLite membership for every supported sort order. The canonical mailbox query state versions these
locally generated windows so a cached arbitrary page is reused only while it remains current.

Notification navigation is not pagination. If its Email is absent from the current window, the
application requests an anchored window with `anchorOffset: 0`, persists the server-returned
position and ordered IDs, and selects the target from that committed cache state. This contextual
window does not alter the canonical window's identity or infer offsets from locally cached rows.

For a contiguous cached prefix, `Email/queryChanges` uses the final cached representative as
`upToId`, applies removals and indexed additions across every retained page, and advances all those
windows to the returned query state in one transaction. Updates are fetched only for objects already
cached, plus additions that fall into the prefix; changes outside partial coverage are harmless.
Sparse online windows that cannot be rebased exactly are invalidated. Optimistic Email mutations
invalidate affected mailbox windows and account search windows in the same
`MutationProjectionTransaction` as their projected Email state.

Invalidated mailbox and search windows retain their server-ordered representative IDs as a stale
projection scaffold. The GUI may join those positions to SQLite's effective Email state so
removals and keyword changes render immediately, but a stale window is never a pagination cache hit
and cannot derive new positions or totals. The application replaces it with an exact JMAP query
window. After an optimistic Email mutation is submitted, the application coordinator refreshes an
active search tab explicitly; inactive search tabs refresh when activated.

Totals are authoritative conversation counts for `collapseThreads: true`. Partial cached counts
are diagnostic values only and must not replace query totals. Expanded thread members and retained
message-view selections are outside pagination accounting and do not alter the visible range.
