# Query Windows and Pagination

JMAP query membership and cached objects are different kinds of state. Email rows answer what is
known about an object; an ordered query window answers which representative IDs occupy a particular
server result range. The application must never reconstruct a server position by applying SQL
`OFFSET` to the subset of Email objects currently cached.

Mailbox and search windows persist the query identity, requested offset and limit, returned
position and enforced limit, total, `queryState`, and ordered representative Email IDs. The GUI
joins those IDs to cached Email data in stored order. A missing window is a cache miss even when the
database contains enough unrelated mailbox rows.

Sequential navigation anchors the next request to the final representative in the visible window.
This preserves continuity when messages are inserted above the viewport. The returned `position`
is authoritative, and the returned ID count—not the requested page size—determines the next logical
offset. Previous-page and invalid-offset recovery use the server-enforced limit and refresh the
target window.

`Email/queryChanges` membership changes invalidate retained mailbox windows. Definitive destroyed
Emails are removed in the same cache transaction. Updated Emails are fetched and rebased before a
replacement window is committed, so mailbox membership changes cannot leave stale rows shifting a
large mailbox. Optimistic Email mutations invalidate affected mailbox windows and account search
windows in the same `MutationProjectionTransaction` as their projected Email state.

Totals are authoritative conversation counts for `collapseThreads: true`. Partial cached counts
are diagnostic values only and must not replace query totals. Expanded thread members and retained
message-view selections are outside pagination accounting and do not alter the visible range.
