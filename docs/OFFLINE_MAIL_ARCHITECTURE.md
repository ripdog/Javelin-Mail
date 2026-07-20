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
  mailbox changes its downloaded sources to evictable cache; it does not immediately destroy them.
- Foreground work owns network priority. An in-flight background request may finish, but no further
  background request starts until every startup, freshness, and user-initiated request has ended,
  followed by a five-second quiet period. The quiet period also applies at process startup.
- WebSocket failures are process-local and shared by method and state-change transports. A failure
  selects HTTP for at most 15 minutes; the process then actively retries WebSocket. Failure state is
  never persisted, so every new process attempts the advertised WebSocket endpoint.

## Vault and recovery

Raw messages are SHA-256-addressed under `objects/sha256`. Per-account, per-mailbox `messages`
directories contain hard links named by Email id, making the mirror directly usable without an
export operation. Installation uses a same-directory atomic commit. SQLite then records the object
and Email reference and appends filesystem projection jobs in the same cache transaction.

Projection jobs are replayable. A crash may leave an unprojected immutable object or a queued link,
but cannot make SQLite claim a completed full mirror. Objects with no retained Email reference and
no pending projection are garbage-collection candidates.

Existing `raw_message_sources` rows are an explicit migration source only while
`raw_message_sources_to_vault` is incomplete. Each source is installed and verified before its
vault reference is committed. After all references exist, legacy BLOB rows are cleared and normal
reads stop consulting the old table.

## Full synchronization and pagination

A full-sync job enumerates an uncollapsed, newest-first `Email/query` in bounded pages, materializes
each page through `Email/get`, downloads missing raw sources, and promotes its staging generation
only after complete coverage. The first id anchors subsequent pages, and the persisted generation,
anchor, and position make enumeration restartable without treating any page as the mailbox. If the
anchor becomes invalid, the incomplete staging generation is discarded and safely restarted.
Later state-change refreshes update the same Email rows and effective membership; changes observed
while a full job is running mark the account dirty, and a follow-up catch-up downloads any source
whose cached blob id does not match current metadata before the mirror can settle.

Complete mailboxes paginate locally from effective membership. Partial, notification-only, and
visible mailboxes retain bounded server query windows. Optimistic mailbox changes invalidate those
windows transactionally. For a complete mailbox the replacement window is immediately regenerated
from effective local membership, so a move appears in both source and destination without waiting
for JMAP and without treating the visible page as the mailbox.

## Tasks and search

Semantic long-running work is persisted in `background_jobs`, with checkpoints, progress, durable
pause, retry, and dependency records. The process-owned scheduler exposes interactive, foreground,
freshness, bulk, derived, and maintenance priorities. The status bar and tray summarize active work;
the modeless Task Center renders the committed job model and provides pause/resume/retry controls.

Each account has a disposable `indexes/<account>/search.sqlite3` FTS5 index. It contains normalized
subject/body text and a source hash, not MIME or attachment bytes. MIME parsing runs away from the
GUI thread. Search results are joined back to the main cache for current effective objects and are
merged with JMAP search by the existing search session. Deleting the index is always safe; indexing
rebuilds it from the vault.
