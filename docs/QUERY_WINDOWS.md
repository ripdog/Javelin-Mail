# Query Windows and Pagination

JMAP query membership and cached objects are different kinds of state. Email rows answer what is
known about an object; an ordered query window answers which representative IDs occupy a particular
server result range. The application must never reconstruct a server position by applying SQL
`OFFSET` to the subset of Email objects currently cached.

Mailbox and search windows persist the query identity, requested offset and limit, returned
position and enforced limit, total, `queryState`, and ordered representative Email IDs. The GUI
joins those IDs to cached Email data in stored order. A missing window is a cache miss even when the
database contains enough unrelated mailbox rows.

Every watched mailbox has a canonical window: offset 0, limit 100, `receivedAt` descending, and
`collapseThreads: true`. Background synchronization must materialize that window whenever it is
missing or invalid; caching Email objects without the matching authoritative membership window is
not a successful mailbox materialization. A post-commit cache change names the canonical window so
an open view reloads it from SQLite.

Sequential navigation anchors the next request to the final representative in the visible window.
This preserves continuity when messages are inserted above the viewport. The returned `position`
is authoritative, and the returned ID count—not the requested page size—determines the next logical
offset. Previous-page and invalid-offset recovery use the server-enforced limit and refresh the
target window.

Notification navigation is not pagination. If its Email is absent from the current window, the
application requests an anchored window with `anchorOffset: 0`, persists the server-returned
position and ordered IDs, and selects the target from that committed cache state. This contextual
window does not alter the canonical window's identity or infer offsets from locally cached rows.

`Email/queryChanges` membership changes invalidate retained mailbox windows. Definitive destroyed
Emails are removed in the same cache transaction. Updated Emails are fetched and rebased before a
replacement window is committed, so mailbox membership changes cannot leave stale rows shifting a
large mailbox. Optimistic Email mutations invalidate affected mailbox windows and account search
windows in the same `MutationProjectionTransaction` as their projected Email state.

Invalidated mailbox windows retain their server-ordered representative IDs as a stale projection
scaffold. The GUI may join those positions to SQLite's effective Email state so removals and
keyword changes render immediately, but a stale window is never a pagination cache hit and cannot
derive new positions or totals. The application replaces it with an exact JMAP query window.

Totals are authoritative conversation counts for `collapseThreads: true`. Partial cached counts
are diagnostic values only and must not replace query totals. Expanded thread members and retained
message-view selections are outside pagination accounting and do not alter the visible range.
