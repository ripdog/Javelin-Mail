# Offline Mail Architecture

## Invariants

- SQLite remains the UI source of truth. Mailbox models read effective `email_mailboxes`
  membership: confirmed server state with active optimistic mutations already rebased onto it.
- Selecting **Keep complete offline copy** means every Email in that server mailbox has cached
  metadata and a complete raw RFC 5322/MIME source. A mailbox is never marked complete while any
  source is missing.
- Raw sources live in `mail-vault/v1`, beside the application cache database. SQLite records
  identity, retention, progress, and recovery work; it does not contain multi-gigabyte MIME BLOBs.
- Confirmed server destruction removes the local reference and mailbox projection. Unchecking a
  mailbox changes its downloaded sources to evictable cache; it does not destroy them unless the
  user explicitly chooses cache cleanup. User-facing mailbox cleanup may remove bodies alone, but
  clearing SQLite-backed mailbox state must also clear that mailbox's cached bodies. Explicit
  cleanup is a daemon-owned durable background job: it is committed before the GUI reports that it
  was queued, appears in Task Center, and is reconstructed from its persisted checkpoint after a
  daemon restart.
- Foreground work owns network priority. An in-flight background request may finish, but no further
  background request starts until every startup, freshness, and user-initiated request has ended,
  followed by a five-second quiet period. The quiet period also applies at process startup.
- WebSocket failures are process-local and shared by method and state-change transports. A failure
  selects HTTP for at most 15 minutes; the process then actively retries WebSocket. Failure state is
  never persisted, so every new process attempts the advertised WebSocket endpoint.

## Vault and recovery

Raw messages are SHA-256-addressed under `objects/sha256`. Per-account, per-mailbox `messages`
directories contain hard links named by Email id, making the mirror directly usable without an
export operation. Network downloads stream into a bounded-memory incoming file inside the vault,
are hashed incrementally, and are promoted to their content-addressed path with an atomic
same-filesystem rename. Stale partial incoming downloads are discarded during daemon startup before
network work begins. SQLite then records the object and Email reference and appends filesystem
projection jobs in the same cache transaction.

Projection jobs are replayable. A crash may leave an unprojected immutable object or a queued link,
but cannot make SQLite claim a completed full mirror. Mailbox cache cleanup also treats persisted
unlink projection jobs as eviction candidates, so a crash after references are removed but before
physical vault eviction can resume without losing track of the object. Combined body/SQLite cleanup
always removes body ownership first; replay is therefore safe at every committed boundary. Objects
with no retained Email reference and no pending projection are garbage-collection candidates.

Existing `raw_message_sources` rows are an explicit migration source only while
`raw_message_sources_to_vault` is incomplete. Each source is installed and verified before its
vault reference is committed. After all references exist, legacy BLOB rows are cleared and normal
reads stop consulting the old table.

## Full synchronization and pagination

A full-sync job enumerates an uncollapsed, newest-first `Email/query` in 100-object pages and
materializes each page through `Email/get`. Every commit advances the durable crawl cursor and task
progress and also extends the ordinary collapsed `mailbox_query_windows` prefix using the real
server `queryState`. The GUI therefore consumes background pages through exactly the same cache
records as user-requested pages; no private staging read path exists. Explicitly synchronized
mailboxes retain every materialized window instead of the bounded online working set.

The first id anchors subsequent pages. Generation, anchor, committed position, totals, and state
tokens survive process termination, so recovery resumes at the first uncommitted page. Transient
failures preserve that cursor; only an explicit JMAP `anchorNotFound` starts a new generation.
`Email/queryChanges` is bounded by the cached prefix tail, rebases every contiguous retained window,
fetches additions plus updates to objects already known locally, and ignores changes wholly beyond
the cached prefix. Later state-change refreshes update the same Email rows and effective membership.
Raw message bodies are downloaded only after metadata enumeration completes. Hydration derives
aggregate progress from SQLite but retains at most 256 pending Email ids in memory at a time.

The `fetching` phase is itself durable. Restarting the daemon during body hydration does not repeat
the full `Email/query`/`Email/get` crawl: after the normal foreground reconnect catch-up, the offline
job reconciles its generation membership against current `email_mailboxes` and derives remaining
work from Emails without a vault reference matching their current `blobId`. This naturally includes
new mail and messages moved into any position in the mailbox while Javelin was offline. Hydration
rechecks membership after each download pass and promotes the generation to `complete` only in the
same transaction that verifies no current mailbox Email is missing its matching raw source. Mail
arriving during hydration therefore extends the same generation instead of creating a transient
false-complete state.

Complete mailboxes paginate locally from effective membership. Partial synchronized mailboxes use
their durable contiguous window prefix, while notification-only and visible online mailboxes retain
bounded server query windows. Optimistic mailbox changes invalidate affected windows
transactionally. For a complete mailbox the replacement window is immediately regenerated from
effective local membership, so a move appears in both source and destination without waiting for
JMAP and without treating the visible page as the mailbox.

## Tasks and search

Semantic long-running work is persisted in `background_jobs`, with checkpoints, progress, durable
pause, retry, and dependency records. The process-owned scheduler exposes interactive, foreground,
freshness, bulk, derived, and maintenance priorities. The status bar and tray summarize active work;
the modeless Task Center renders the committed job model and provides pause/resume/retry controls.

Each account has a disposable `indexes/<account>/search.sqlite3` FTS5 index. It contains normalized
subject/body text and a source hash, not MIME or attachment bytes. MIME parsing runs away from the
GUI thread. Bulk indexing reads pending refs in bounded batches, uses one indexing worker, and parses
only the primary text body so attachment payloads are never decoded for search. Each batch keeps one
raw MIME source in memory at a time and commits the FTS/indexed-hash bookkeeping in batch
transactions. Search results are joined back to the main cache for current effective objects and are
merged with JMAP search by the existing search session. Deleting the index is always safe; indexing
rebuilds it from the vault.
